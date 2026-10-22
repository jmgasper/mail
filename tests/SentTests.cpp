#include "mail/Sync.h"
#include "mail/Outbox.h"
#include "mail/Mime.h"
#include <iostream>
#include <filesystem>
#include <unistd.h>

using namespace mail;
namespace {
int checks=0;
void expect(bool value,const char* text) { ++checks; if (!value) throw std::runtime_error(text); }
template<class F> void rejects(F fn,const char* text) { bool failed=false; try { fn(); } catch (const std::exception&) { failed=true; } expect(failed,text); }
Draft draft(Id account,const std::string& subject)
{
    Draft d; d.account=account; d.messageId=newMessageId(); d.to="recipient@example.test"; d.bcc="hidden@example.test";
    d.subject=subject; d.text="Sent body 🎉\n.leading dot\nM1 OK forged response\n";
    d.runs={{0,true,false,false,"https://example.test"}};
    Attachment a; a.name="sent.txt"; a.data=std::string("bytes\0here",10); d.attachments.push_back(a); return d;
}
void storage(const std::string& path,Account a)
{
    Store first(path),second(path); a.id=first.saveAccount(a);
    a.sentMode=SentCopyMode::Server; a.sentFolder="Custom"; first.saveAccount(a);
    auto saved=first.accounts()[0]; expect(saved.sentMode==a.sentMode && saved.sentFolder=="Custom","account Sent policy round trips");
    auto d=draft(a.id,"Journal"); auto outgoing=first.enqueueDraft(first.saveDraft(d),1792542600);
    first.claimOutgoing(); first.finishOutgoing(outgoing,"sent","",1792542600);
    expect(first.outbox().empty() && first.drafts().empty(),"SMTP receipt commits independently of copy");
    auto copy=first.sentCopies()[0]; const auto id=copy.id;
    first.tag(id,"Keep",true); first.setRead(id,false); first.setStarred(id,true); first.move(id,"archive");
    expect(first.claimSentCopy(id).id==id && !second.claimSentCopy(id).id,"copy claim excludes a second connection");
    rejects([&]{second.keepSentCopyLocal(id);},"active copy cannot be abandoned");
    rejects([&]{second.retrySentCopy(id,true);},"active copy cannot be retried");
    first.beginSentUpload(id,"Sent"); first.recoverOutgoing();
    copy=first.sentCopies()[0]; expect(copy.state=="uncertain" && !copy.mayAppend,"crash after durable upload marker stays uncertain");
    rejects([&]{first.retrySentCopy(id);},"uncertain copy needs explicit retry");
    first.retrySentCopy(id,true); first.claimSentCopy(id); first.recoverOutgoing();
    expect(first.sentCopies()[0].state=="pending","interrupted search without upload is safe to repeat");
    first.claimSentCopy(id); first.releaseSentCopy(id,"Accepted",false,true); first.claimSentCopy(id); first.recoverOutgoing();
    expect(first.sentCopies()[0].state=="saved" && first.sentCopies()[0].confirmed,"acknowledged copy stays acknowledged after search crash");
    rejects([&]{first.retrySentCopy(id,true);},"accepted copy cannot be blindly uploaded again");
    first.bindSentCopy(id,{"Sent","41","7"});
    auto m=first.message(id); expect(first.sentCopies().empty() && m.remoteKey==imapRemoteKey({"Sent","41","7"}),"binding retains stable local identity");
    expect(m.unread && m.starred && m.folder=="archive" && m.tags==std::vector<std::string>{"Keep"},"reconciliation preserves local organization");
    expect(first.pendingChanges(a.id).size()==3,"edits before remote identity are queued for the server");
    rejects([&]{first.retryOutgoing(outgoing,true);},"copy operations cannot reopen SMTP delivery");
    a.sentFolder="Sent"; first.saveAccount(a);
    auto server=draft(a.id,"Concurrent server copy");
    auto queued=first.enqueueDraft(first.saveDraft(server),1792542600); auto claimed=first.claimOutgoing();
    first.saveRemoteFolders(a.id,{{"Sent","Sent","sent"}});
    auto remote=parseMessage(claimed.raw); remote.account=a.id; remote.folder="sent"; remote.remoteKey=imapRemoteKey({"Sent","41","8"});
    auto downloaded=first.ingestRemote(remote,{"Sent","41","8"});
    first.finishOutgoing(queued,"sent","",1792542600);
    expect(first.sentCopies().empty() && first.findRemote(a.id,remote.remoteKey)==downloaded,"receive before SMTP receipt is adopted without a duplicate");
    first.removeAccount(a.id); expect(first.sentCopies().empty(),"account removal cascades Sent journal");
}
}
int main(int argc,char** argv)
{
    if (argc!=9) return 2;
    char pattern[]="/tmp/mail-sent-tests-XXXXXX"; const char* dir=mkdtemp(pattern); if (!dir) return 1;
    try {
        Account a; a.name="Sent fixture"; a.email="user@example.test"; a.provider=Provider::Imap;
        a.incoming={argv[1],std::stoi(argv[3]),Security::TLS,"normal",argv[2]}; a.outgoing={argv[1],std::stoi(argv[7]),Security::TLS,"normal",argv[2]};
        expect(sentCopyMode(a)==SentCopyMode::Upload,"IMAP defaults to uploading Sent");
        auto bridge=a; bridge.provider=Provider::ProtonBridge; expect(sentCopyMode(bridge)==SentCopyMode::Server,"Bridge defaults to server-managed Sent");
        auto pop=a; pop.provider=Provider::Pop3; expect(sentCopyMode(pop)==SentCopyMode::Local,"POP3 keeps local Sent");
        storage(std::string(dir)+"/journal.db",a);
        std::atomic<bool> cancel{false};
        for (const std::string scenario:{"normal","auto","search","lost","no-uid","bad-uid","reject-pre","reject-post","missing","ambiguous","custom","sticky","malformed-search","local"}) {
            Store store(":memory:"); a.id=0; a.incoming.username=scenario; a.outgoing.username=scenario;
            a.sentMode=scenario=="local"?SentCopyMode::Local:(scenario=="auto"?SentCopyMode::Server:SentCopyMode::Automatic);
            a.sentFolder=scenario=="custom"?"Custom":""; a.provider=Provider::Imap; // The guest fixture uses a gateway, not a loopback Bridge.
            a.id=store.saveAccount(a); auto d=draft(a.id,scenario); store.enqueueDraft(store.saveDraft(d),1792542600);
            auto delivery=deliverQueued(store,[](const Account&){return "test-password";},cancel);
            expect(delivery.sent==1 && store.outbox().empty(),"fixture delivery accepted once");
            if (scenario=="local") { expect(store.sentCopies().empty(),"local policy creates no copy work"); continue; }
            auto id=store.sentCopies()[0].id; store.tag(id,"Retain",true);
            if (scenario=="normal") { store.move(id,"archive"); store.setStarred(id,true); }
            auto run=[&] { try { return syncAccount(store,a,"test-password",cancel); } catch (const NetworkError& e) { expect(e.reason==NetworkFailure::CopyUnknown,"only interrupted APPEND escapes copy pass"); return SyncReport{}; } };
            auto report=run(); auto copies=store.sentCopies();
            if (scenario=="normal" || scenario=="auto" || scenario=="search" || scenario=="custom") {
                expect(copies.empty() && report.errors.empty(),"successful or server-created Sent copy reconciled");
                expect(store.message(id).tags==std::vector<std::string>{"Retain"},"Sent adoption preserves tags and ID");
                if (scenario=="normal") expect(store.message(id).folder=="archive" && store.message(id).starred && store.pendingChanges(a.id).empty(),"pre-upload move and star reach server");
                if (scenario=="search") expect(store.message(id).remoteKey==imapRemoteKey({"Sent","41","2"}),"substring search candidate with different Message-ID is not adopted");
                expect(run().errors.empty() && store.sentCopies().empty(),"repeat check creates no new copy");
            } else if (scenario=="lost" || scenario=="bad-uid" || scenario=="no-uid") {
                expect(copies.size()==1 && !copies[0].mayAppend && copies[0].state==(scenario=="no-uid"?"saved":"uncertain"),"missing/malformed receipt cannot requeue APPEND");
                store.recoverOutgoing(); run();
                expect(store.sentCopies().size()==1,"delayed search visibility preserves copy journal");
                run(); expect(store.sentCopies().empty(),"later exact search adopts accepted copy without reupload");
            } else {
                expect(copies.size()==1 && copies[0].mayAppend,"definite rejection or missing mapping retains retryable copy");
                expect(copies[0].destination.empty(),"definite rejection permits corrected account folder settings");
                expect(!report.errors.empty(),"copy failure is reported separately from delivery");
                store.keepSentCopyLocal(id); expect(store.sentCopies().empty() && !store.message(id).raw.empty(),"abandon copy work preserves locally delivered message");
            }
            expect(deliverQueued(store,[](const Account&){return "test-password";},cancel).sent==0,"copy recovery never requeues SMTP");
        }
        std::filesystem::remove_all(dir); std::cout<<checks<<" Sent-copy assertions passed\n"; return 0;
    } catch (const std::exception& e) { std::cerr<<"FAIL: "<<e.what()<<'\n'; std::filesystem::remove_all(dir); return 1; }
}
