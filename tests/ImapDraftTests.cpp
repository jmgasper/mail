#include "mail/Provider.h"
#include "mail/Compose.h"
#include <algorithm>
#include <iostream>

using namespace mail;
namespace {
int checks=0;
void expect(bool value,const char* why) { ++checks; if (!value) throw std::runtime_error(why); }
template<class F> void failure(F action,NetworkFailure reason,const char* why)
{ bool matched=false; try { action(); } catch (const NetworkError& error) { matched=error.reason==reason; } expect(matched,why); }
std::string wire(const Account& account,const std::string& scenario)
{
    Draft d; d.account=1; d.messageId="<imap-draft-"+scenario+"@example.test>";
    d.subject="Saved draft 🎉"; d.bcc="hidden@example.test"; d.text="Body 🎉"; d.runs={{0,true,false,false,{}}};
    d.attachments={{"tiny.bin","application/octet-stream","",std::string("a\0z",3),false}};
    return composeDraftMessage(d,account,1792542600);
}
}
int main(int argc,char** argv)
{
    if (argc!=9) return 2;
    try {
        std::atomic<bool> cancel{false}; Account a; a.id=1; a.name="IMAP draft fixture"; a.email="user@example.test"; a.provider=Provider::Imap;
        a.incoming={argv[1],std::stoi(argv[3]),Security::TLS,"normal",argv[2]}; a.outgoing=a.incoming;
        for (const std::string scenario:{"normal","legacy","rev2","expunge-denied","lost-delete","starttls"}) {
            a.incoming.username=scenario; a.incoming.security=scenario=="starttls"?Security::StartTLS:Security::TLS;
            a.incoming.port=std::stoi(argv[scenario=="starttls"?4:3]);
            auto p=connectMail(a,"test-password",cancel); auto folders=p->folders();
            auto target=std::find_if(folders.begin(),folders.end(),[](const RemoteFolder& f){return f.role=="drafts";});
            expect(target!=folders.end(),"Drafts special-use folder is available");
            auto snapshot=p->snapshot(*target);
            expect(snapshot.epoch=="41" && snapshot.nextUid=="50","draft snapshot preserves UIDVALIDITY and UIDNEXT, including UID gaps");
            expect(snapshot.messages.size()==3 && snapshot.messages[0].draft && !snapshot.messages[0].deleted
                && snapshot.messages[1].deleted && !snapshot.messages[2].draft,"snapshot distinguishes drafts, deleted copies and ordinary messages");
            RemoteLocation old{target->id,snapshot.epoch,"3"}; auto original=p->fetchDraft(old);
            expect(original && original->draft && !original->deleted && original->raw.find("Original draft")!=std::string::npos,"draft read returns complete content and flags without marking it read");
            failure([&]{p->removeDraft({target->id,"99","3"},original->raw);},NetworkFailure::Protocol,"changed mailbox identifiers block draft deletion");
            failure([&]{p->removeDraft(old,"different bytes");},NetworkFailure::Protocol,"a different original MIME cannot authorize removal");
            auto ordinary=p->fetchDraft({target->id,"41","5"});
            failure([&]{p->removeDraft(ordinary->location,ordinary->raw);},NetworkFailure::Protocol,"an ordinary message cannot be removed as a draft");
            expect(!p->fetchDraft({target->id,"41","9"}),"a missing UID has an explicit empty read result");
            p->removeDraft({target->id,"41","9"},"gone");
            auto raw=wire(a,scenario); auto receipt=p->appendDraft(*target,raw,1792542600);
            expect(scenario=="legacy"?!receipt:(receipt && receipt->epoch=="41" && receipt->locator=="50"),"APPEND preserves a valid receipt or reports its absence");
            auto uploaded=p->fetchDraft({target->id,"41","50"});
            expect(uploaded && uploaded->draft && uploaded->raw==raw,"saved draft keeps BCC, rich text and binary MIME exactly");
            if (scenario=="lost-delete") {
                failure([&]{p->removeDraft(old,original->raw);},NetworkFailure::Connection,"interrupted removal is reported for later verification");
                p=connectMail(a,"test-password",cancel);
            }
            p->removeDraft(old,original->raw); auto removed=p->fetchDraft(old);
            expect(!removed || removed->deleted,"draft removal confirms disappearance or its targeted Deleted flag");
            auto unrelated=p->fetchDraft({target->id,"41","4"});
            expect(unrelated && unrelated->deleted,"removing one draft never expunges another client's deleted message");
            expect(p->snapshot(*target).nextUid=="51","next upload baseline advances after APPEND");
        }
        a.incoming.security=Security::TLS; a.incoming.port=std::stoi(argv[3]);
        for (const std::string scenario:{"lost","no-uid","bad-uid","reject-pre","reject-post","missing-body","missing-flags","ignore-delete","missing-next","bad-next"}) {
            a.incoming.username=scenario; auto p=connectMail(a,"test-password",cancel); RemoteFolder folder{"Drafts","Drafts","drafts"};
            const auto raw=wire(a,scenario);
            if (scenario=="missing-body" || scenario=="missing-flags") {
                failure([&]{p->fetchDraft({"Drafts","41","3"});},NetworkFailure::Protocol,"incomplete server draft data is an error, not deletion evidence"); continue;
            }
            if (scenario=="missing-next") { expect(p->snapshot(folder).nextUid.empty(),"missing UIDNEXT is not guessed from the greatest visible UID"); continue; }
            if (scenario=="bad-next") { failure([&]{p->snapshot(folder);},NetworkFailure::Protocol,"invalid UIDNEXT is rejected"); continue; }
            if (scenario=="ignore-delete") {
                auto original=p->fetchDraft({"Drafts","41","3"});
                failure([&]{p->removeDraft(original->location,original->raw);},NetworkFailure::Protocol,"an ignored Deleted flag cannot falsely confirm cleanup"); continue;
            }
            if (scenario=="reject-pre" || scenario=="reject-post") {
                failure([&]{p->appendDraft(folder,raw,1792542600);},NetworkFailure::Rejected,"tagged APPEND rejection remains distinct from acceptance uncertainty");
                expect(!p->fetchDraft({"Drafts","41","50"}),"rejected APPEND creates no saved draft"); continue;
            }
            if (scenario=="no-uid") expect(!p->appendDraft(folder,raw,1792542600),"accepted APPEND may lack a UID receipt");
            else failure([&]{p->appendDraft(folder,raw,1792542600);},NetworkFailure::CopyUnknown,"lost or malformed APPEND receipt remains uncertain");
            p=connectMail(a,"test-password",cancel); auto copy=p->fetchDraft({"Drafts","41","50"});
            expect(copy && copy->raw==raw && p->snapshot(folder).nextUid=="51","an accepted uncertain upload is discoverable without another APPEND");
        }
        cancel=true; failure([&]{connectMail(a,"test-password",cancel);},NetworkFailure::Cancelled,"cancelled draft connection never starts");
        std::cout<<checks<<" IMAP draft transport checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr<<"FAIL: "<<error.what()<<'\n'; return 1; }
}
