#include "mail/Provider.h"
#include "mail/Sync.h"
#include "mail/Outbox.h"
#include <fstream>
#include <iostream>
#include <iterator>

using namespace mail;
namespace {
int assertions=0;
void expect(bool ok,const char* message) { ++assertions; if (!ok) throw std::runtime_error(message); }
template<class F> void rejects(F fn,const char* message)
{
    bool rejected=false; try { fn(); } catch (const std::exception&) { rejected=true; } expect(rejected,message);
}
Account account(const std::string& host,const std::string& certificate,int incoming,int outgoing,Provider provider,Security security)
{
    Account a; a.name="Protocol fixture"; a.email="user@example.test"; a.provider=provider;
    a.incoming={host,incoming,security,"user",certificate};
    a.outgoing={host,outgoing,Security::TLS,"user",certificate}; return a;
}
}
int main(int argc,char** argv)
{
    if (argc!=9) { std::cerr<<"Usage: mail_protocol_tests HOST CA IMAPS IMAP_STARTTLS POP3S POP3_STARTTLS SMTPS SMTP_STARTTLS\n"; return 2; }
    try {
        std::atomic<bool> cancel{false};
        auto a=account(argv[1],argv[2],std::stoi(argv[3]),std::stoi(argv[7]),Provider::Imap,Security::TLS);
        auto p=connectMail(a,"test-password",cancel);
        auto folders=p->folders();
        expect(folders.size()==4,"LIST excludes nonselectable parents and handles literal names");
        expect(folders[0].role=="inbox" && folders[1].role=="archive","special-use folders map to their actual server names");
        expect(folders[2].name=="Envoyé","modified UTF-7 folder names decode for display");
        expect(folders[3].id=="Long folder","literal mailbox names are parsed");
        auto snapshot=p->snapshot(folders[0]);
        expect(snapshot.epoch=="41" && snapshot.messages.size()==2,"snapshot retains UIDVALIDITY and message UIDs");
        expect(snapshot.messages[0].unread && !snapshot.messages[0].starred,"unread flags are retained");
        expect(!snapshot.messages[1].unread && snapshot.messages[1].starred,"read/starred flags are retained");
        auto raw=p->fetch(snapshot,snapshot.messages[0]);
        expect(raw.find("Subject: Remote message")!=std::string::npos && raw.find("M6 OK forged completion")!=std::string::npos
            && raw.find("End of literal")!=std::string::npos,"literal message bytes cannot terminate a command response");
        expect(p->snapshot(folders[0]).messages[0].unread,"downloading uses PEEK and does not mark mail read");
        RemoteLocation location{folders[0].id,snapshot.epoch,snapshot.messages[0].locator};
        p->setFlag(location,true,true); p->setFlag(location,false,true);
        auto changed=p->snapshot(folders[0]);
        expect(!changed.messages[0].unread && changed.messages[0].starred,"read and star changes reach the server");
        p->setFlag(location,true,false);
        changed=p->snapshot(folders[0]);
        expect(changed.messages[0].unread && changed.messages[0].starred,"removing Seen preserves unrelated flags");
        auto wrong=location; wrong.epoch="999";
        rejects([&]{p->setFlag(wrong,false,false);},"UIDVALIDITY reset blocks stale mutations");
        auto moved=p->move(location,folders[1]);
        expect(moved.folder=="Archive" && moved.epoch=="77" && moved.locator=="5","MOVE records the server's new UID and epoch");
        expect(p->snapshot(folders[0]).messages.size()==1 && p->snapshot(folders[1]).messages.size()==1,"MOVE updates both server folders");

        a.incoming.port=std::stoi(argv[4]); a.incoming.security=Security::StartTLS;
        auto starttls=connectMail(a,"test-password",cancel);
        expect(!starttls->folders().empty(),"IMAP STARTTLS authenticates after encryption");
        a.incoming.security=Security::TLS; a.incoming.port=std::stoi(argv[3]);
        rejects([&]{connectMail(a,"wrong-password",cancel);},"incorrect credentials fail");
        a.incoming.certificate.clear();
        rejects([&]{connectMail(a,"test-password",cancel);},"untrusted server certificate fails closed");
        a.incoming.certificate=argv[2];
        a.incoming.host="localhost/../../bad";
        rejects([&]{connectMail(a,"test-password",cancel);},"hostnames cannot inject URL paths");
        a.incoming.host=argv[1]; cancel=true;
        rejects([&]{connectMail(a,"test-password",cancel);},"cancellation prevents a connection"); cancel=false;

        a=account(argv[1],argv[2],std::stoi(argv[5]),std::stoi(argv[7]),Provider::Pop3,Security::TLS);
        auto pop=connectMail(a,"test-password",cancel);
        auto popSnapshot=pop->snapshot(pop->folders()[0]);
        expect(popSnapshot.messages.size()==2 && popSnapshot.messages[0].key=="pop3:stable-one","POP3 tracks UIDLs instead of volatile message numbers");
        auto popRaw=pop->fetch(popSnapshot,popSnapshot.messages[0]);
        expect(popRaw.find("\r\n.leading dot\r\n")!=std::string::npos,"POP3 dot transparency is decoded");
        expect(pop->snapshot(pop->folders()[0]).messages.size()==2,"POP3 retrieval retains server messages");
        a.incoming.security=Security::StartTLS; a.incoming.port=std::stoi(argv[6]);
        auto popTls=connectMail(a,"test-password",cancel);
        expect(popTls->snapshot(popTls->folders()[0]).messages.size()==2,"POP3 STARTTLS authenticates after encryption");

        const std::string outgoing="From: user@example.test\r\nTo: recipient@example.test\r\nSubject: SMTP fixture\r\n\r\n.leading dot\r\nGoodbye\r\n";
        sendSmtp(a,"test-password","user@example.test",{"recipient@example.test","hidden@example.test"},outgoing,cancel);
        ++assertions;
        rejects([&]{sendSmtp(a,"test-password","user@example.test",{"reject@example.test"},outgoing,cancel);},"rejected recipients prevent a successful send");
        rejects([&]{sendSmtp(a,"test-password","user@example.test",{"bad@example.test\r\nRCPT TO: victim@example.test"},outgoing,cancel);},"envelope injection is rejected");
        a.outgoing.port=std::stoi(argv[8]); a.outgoing.security=Security::StartTLS;
        sendSmtp(a,"test-password","user@example.test",{"recipient@example.test"},outgoing,cancel); ++assertions;
        Store delivery(":memory:"); a.id=delivery.saveAccount(a);
        Draft draft; draft.account=a.id; draft.messageId=newMessageId(); draft.to="recipient@example.test"; draft.bcc="hidden@example.test";
        draft.subject="Queued delivery"; draft.text="Outbox fixture 🎉";
        auto id=delivery.saveDraft(draft); delivery.enqueueDraft(id,1792542600);
        auto result=deliverQueued(delivery,[](const Account&){return "test-password";},cancel);
        expect(result.sent==1 && delivery.outbox().empty(),"queued message is delivered through SMTP STARTTLS and acknowledged");
        Query sent; sent.folder="sent"; expect(delivery.conversations(sent,2000000000).size()==1,"SMTP acceptance creates the local Sent copy");
        draft.messageId=newMessageId(); draft.to="reject@example.test"; draft.bcc.clear();
        id=delivery.saveDraft(draft); delivery.enqueueDraft(id,1792542600);
        result=deliverQueued(delivery,[](const Account&){return "test-password";},cancel);
        expect(result.failed==1 && delivery.outbox()[0].state=="failed","pre-DATA rejection remains a failed outbox entry");
        delivery.returnToDraft(delivery.outbox()[0].id);
        draft.messageId=newMessageId(); draft.to="drop@example.test";
        id=delivery.saveDraft(draft); delivery.enqueueDraft(id,1792542600);
        result=deliverQueued(delivery,[](const Account&){return "test-password";},cancel);
        expect(result.uncertain==1 && delivery.outbox()[0].state=="uncertain","connection loss after accepted DATA reports uncertain delivery");
        result=deliverQueued(delivery,[](const Account&){return "test-password";},cancel);
        expect(result.sent==0 && result.failed==0 && result.uncertain==0,"a later send pass does not resend uncertain mail");
        Store store(":memory:");
        a=account(argv[1],argv[2],std::stoi(argv[3]),std::stoi(argv[7]),Provider::Imap,Security::TLS);
        a.id=store.saveAccount(a);
        auto report=syncAccount(store,a,"test-password",cancel);
        expect(report.errors.empty() && report.downloaded==2,"sync downloads messages from multiple IMAP folders");
        report=syncAccount(store,a,"test-password",cancel);
        expect(report.errors.empty() && report.downloaded==0,"repeat sync does not download or duplicate cached messages");
        Query inbox; inbox.account=a.id;
        auto conversations=store.conversations(inbox,2000000000);
        expect(conversations.size()==1,"downloaded messages appear in the account inbox");
        auto message=store.thread(conversations[0].id)[0];
        store.setRead(message.id,false); store.setStarred(message.id,false);
        report=syncAccount(store,a,"test-password",cancel);
        expect(report.errors.empty() && report.applied==2 && store.pendingChanges(a.id).empty(),"sync applies and acknowledges durable local edits");
        expect(store.message(message.id).unread && !store.message(message.id).starred,"post-sync reconciliation preserves applied edits");
        std::ifstream caFile(argv[2]); std::string ca((std::istreambuf_iterator<char>(caFile)),std::istreambuf_iterator<char>());
        auto expected=describeCertificate(ca);
        expect(expected.selfSigned && expected.sha256.size()==95,"certificate details include a SHA-256 fingerprint");
        Endpoint probe{argv[1],std::stoi(argv[3]),Security::TLS,"",""};
        expect(fetchServerCertificate(probe,"imap",cancel).sha256==expected.sha256,"implicit TLS presents the server certificate");
        probe.port=std::stoi(argv[4]); probe.security=Security::StartTLS;
        expect(fetchServerCertificate(probe,"imap",cancel).pem==expected.pem,"IMAP STARTTLS certificate is retrieved before login");
        probe.port=std::stoi(argv[6]);
        expect(fetchServerCertificate(probe,"pop3",cancel).sha256==expected.sha256,"POP3 STARTTLS certificate is retrieved");
        probe.port=std::stoi(argv[8]);
        expect(fetchServerCertificate(probe,"smtp",cancel).sha256==expected.sha256,"SMTP STARTTLS certificate is retrieved");
        auto flattened=ca; for (auto& c:flattened) if (c=='\n') c=' ';
        auto pasted=account(argv[1],flattened,std::stoi(argv[3]),std::stoi(argv[7]),Provider::Imap,Security::TLS);
        expect(!connectMail(pasted,"test-password",cancel)->folders().empty(),"pasted certificate text, even flattened to one line, is trusted");
        pasted.incoming.certificate="-----BEGIN CERTIFICATE----- not base64! -----END CERTIFICATE-----";
        rejects([&]{connectMail(pasted,"test-password",cancel);},"invalid pasted certificate text is rejected");
        std::cout<<assertions<<" protocol assertions passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr<<"FAIL: "<<error.what()<<'\n'; return 1; }
}
