#include "mail/Gmail.h"
#include "mail/Store.h"
#include "mail/Clipboard.h"
#include "mail/Outbox.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <iostream>

using namespace mail;
using Json=nlohmann::json;
namespace {
int checks=0;
void expect(bool value,const char* text) { ++checks; if(!value) throw std::runtime_error(text); }
template<class F> void rejects(F action,const char* text)
{ bool failed=false; try { action(); } catch(const std::exception&) { failed=true; } expect(failed,text); }
template<class F> void failure(F action,NetworkFailure reason,const char* text)
{ bool failed=false; try { action(); } catch(const NetworkError& e) { failed=e.reason==reason; } expect(failed,text); }
Account account()
{ Account a; a.id=1; a.name="Draft fixture"; a.email="author@example.test"; a.provider=Provider::Gmail; return a; }
Draft richDraft()
{
    Draft d; d.account=1; d.messageId="<draft-suite@example.test>";
    d.to="\"Recipient, Team\" <recipient@example.test>"; d.cc="Copy <copy@example.test>"; d.bcc="Hidden 🐦 <hidden@example.test>";
    d.subject="Draft 🎉"; d.references={"<previous@example.test>"}; d.providerThread="thread-original";
    d.attachments={{"Photo 🐦.png","image/png","photo@example.test",std::string("\x89PNG\r\n\x1a\n\0\xff",10),true},
        {"résumé.txt","text/plain","",std::string("notes\0end",9),false}};
    const auto marker=imagePlaceholder(d.attachments[0]);
    d.text="Top 🎉\n"+marker+"\nLink and "+marker+"\nEnd";
    d.images={{std::string("Top 🎉\n").size(),marker.size(),"photo@example.test"},{d.text.rfind(marker),marker.size(),"photo@example.test"}};
    auto link=d.text.find("Link");
    d.runs={{0,true,false,false,""},{link,false,true,true,"https://example.test/a?b=1&c=2"},{link+4,false,false,false,""}};
    d.modified=1792542600; return d;
}
TextRun styleAt(const Draft& d,std::size_t offset)
{ TextRun result; for(const auto& run:d.runs) if(run.offset<=offset) result=run; return result; }
void content(const Draft& d,const Draft& original)
{
    expect(d.text==original.text,"draft text and image placeholders round trip");
    expect(d.subject==original.subject && d.references==original.references && d.messageId==original.messageId,"draft headers and reply references round trip");
    expect(parseAddresses(d.to)[0].name=="Recipient, Team" && parseAddresses(d.bcc)[0].name=="Hidden 🐦","encoded names and BCC remain editable");
    expect(d.attachments.size()==2 && d.images.size()==2,"repeated CID images share one owned attachment");
    for(std::size_t i=0;i<d.attachments.size();++i)
        expect(d.attachments[i].data==original.attachments[i].data && d.attachments[i].name==original.attachments[i].name,"attachment names and binary bytes survive import");
    for(std::size_t i=0;i<d.images.size();++i)
        expect(d.images[i].offset==original.images[i].offset && d.images[i].contentId==original.images[i].contentId,"CID image position survives import");
    for(auto offset:{std::size_t(0),d.text.find("Link"),d.text.find("End")}) {
        auto actual=styleAt(d,offset),expected=styleAt(original,offset);
        expect(actual.bold==expected.bold && actual.italic==expected.italic && actual.underline==expected.underline && actual.link==expected.link,"rich text semantics survive import");
    }
}
void mimeChecks()
{
    auto a=account(); auto d=richDraft(); auto raw=composeDraftMessage(d,a,d.modified);
    auto imported=parseDraftMessage(raw,a.id,d.providerThread); content(imported,d);
    expect(imported.providerThread==d.providerThread && imported.modified==d.modified,"provider thread and draft date are retained");
    expect(raw.find("Bcc: ")!=std::string::npos,"server draft contains BCC");
    auto wire=composeMessage(imported,a,d.modified);
    expect(wire.raw.find("Bcc: ")==std::string::npos && wire.recipients.size()==3,"delivery copy still omits BCC while retaining the envelope");
    Store store(":memory:"); a.id=store.saveAccount(a); imported.account=a.id;
    auto id=store.saveDraft(imported); auto reopened=store.draft(id); content(reopened,d);
    content(parseDraftMessage(composeDraftMessage(reopened,a,d.modified),a.id),d);
    Draft empty; empty.account=a.id; empty.messageId=newMessageId();
    auto unfinished=composeDraftMessage(empty,a,d.modified);
    expect(unfinished.find("\r\nTo:")==std::string::npos && parseDraftMessage(unfinished,a.id).text.empty(),"blank recipient and body can be saved as a draft");
    rejects([&]{composeMessage(empty,a,d.modified);},"empty draft cannot be delivered");
    auto partial=parseDraftMessage("Subject: In progress\r\nTo: unfinished@\r\n\r\nA body",a.id);
    expect(partial.to=="unfinished@" && !partial.messageId.empty(),"unfinished recipient text is retained for editing");
    rejects([&]{composeDraftMessage(partial,a,d.modified);},"unfinished recipient is not silently dropped during upload");
    auto encodedInput="=?UTF-8?B?ZmFrZUBleGFtcGxlLnRlc3QsIHJlYWxAZXhhbXBsZS50ZXN0?=";
    partial=parseDraftMessage(std::string("To: ")+encodedInput+"\r\nSubject: In progress\r\n\r\nBody",a.id);
    expect(partial.to==encodedInput,"invalid address text is not reinterpreted as additional decoded recipients");
    rejects([&]{composeDraftMessage(partial,a,d.modified);},"unparsed encoded address input cannot silently become deliverable");
    auto quoted=d; quoted.to="\"A \\\"quoted\\\" name, 🐦\" <recipient@example.test>";
    auto names=parseDraftMessage(composeDraftMessage(quoted,a,d.modified),a.id);
    expect(parseAddresses(names.to)[0].name=="A \"quoted\" name, 🐦","quoted Unicode display names round trip once");
    quoted.to="\""+std::string(512,'x')+"\" <recipient@example.test>";
    names=parseDraftMessage(composeDraftMessage(quoted,a,d.modified),a.id);
    expect(parseAddresses(names.to)[0].name.size()==512,"encoded headers retain the full allowed decoded name length");
    auto files=d.attachments; files[0].contentId="photo+1@example.test";
    auto html=importDraftHtml("A<img src='CID:photo%2B1%40example.test'>B<img src='cid:photo+1@example.test'>",files);
    expect(html.images.size()==2 && html.attachments.size()==2 && html.images[0].contentId==html.images[1].contentId && html.images[0].contentId!="photo+1@example.test","percent-encoded CID references share a normalized safe identifier");
    expect(html.attachments[0].data==d.attachments[0].data && !html.attachments[1].inlineImage,"CID normalization preserves bytes and unused file attachments");
    files.push_back(files[0]);
    html=importDraftHtml("<img src='cid:photo+1@example.test' alt='Ambiguous image'>",files);
    expect(html.images.empty() && html.attachments.size()==3 && html.text=="Ambiguous image","ambiguous CID reference keeps all files without choosing arbitrary content");
    expect(html.attachments[0].contentId!=html.attachments[2].contentId,"duplicate source identifiers become distinct editable attachments");
    html=importDraftHtml("<script>ignored</script><img src='https://example.test/secret' alt='Remote'><img src='file:///secret' alt='File'><img src='cid:photo%ZZ' alt='Invalid'>",d.attachments);
    expect(html.images.empty() && html.text=="RemoteFileInvalid" && html.attachments.size()==2,"untrusted draft resources never become fetched images");
    rejects([&]{parseDraftMessage("To: x@example.test\r\nBcc: =?UTF-8?B?DQpJbmplY3RlZA==?= <hidden@example.test>\r\n\r\n",a.id);},"encoded header controls cannot enter an editable draft");
    rejects([&]{parseDraftMessage("Subject: Duplicate\r\nBcc: one@example.test\r\nbCc: two@example.test\r\n\r\nBody",a.id);},"duplicate hidden-recipient headers cannot lose an address during import");
    rejects([&]{parseMessageAddresses("To: one@example.test\r\ntO: two@example.test\r\n\r\nBody","to");},"MIME address extraction rejects duplicate recipient headers");
    rejects([&]{importDraftHtml("",std::vector<Attachment>(101,d.attachments[0]));},"draft attachment count is bounded");
}
}
int main(int argc,char** argv)
{
    if(argc!=4) return 2;
    try {
        mimeChecks(); std::atomic<bool> cancel{false};
        auto base="https://"+std::string(argv[1])+":"+argv[3];
        GoogleEndpoints endpoints; endpoints.gmail=base+"/gmail"; endpoints.certificate=argv[2];
        GmailApi api([](bool){return "fixture-access";},cancel,endpoints);
        auto fixture=[&](const std::string& path,const Json& value=Json()) {
            HttpRequest request; request.url=base+"/fixture/"+path; request.certificate=argv[2];
            if(!value.is_null()) { request.method="POST"; request.contentType="application/json"; request.body=value.dump(); }
            auto response=requestHttps(request,cancel); requireHttpSuccess(response); return Json::parse(response.body);
        };
        auto page=api.drafts(); expect(page.drafts.size()==2 && page.next=="page+2 /","draft list returns stable and contained identities plus opaque next token");
        auto second=api.drafts(page.next); expect(second.drafts.size()==1 && second.next.empty(),"draft pagination encodes opaque tokens");
        auto seed=api.draft("server_1"); expect(seed.id=="server_1" && seed.message.id=="message_1" && !seed.message.raw.empty(),"raw draft fetch retains both identifiers");
        expect(api.draft("server_1",false).message.raw.empty(),"metadata draft fetch omits MIME");
        auto a=account(); auto d=richDraft(); auto raw=composeDraftMessage(d,a,d.modified);
        auto created=api.createDraft(raw,d.providerThread);
        expect(created.id!=created.message.id && created.message.thread==d.providerThread,"draft creation records stable ID separately from message ID");
        expect(api.draft(created.id).message.raw==raw,"server stores complete draft MIME including BCC");
        auto changed=d; changed.subject="Updated draft";
        auto updatedRaw=composeDraftMessage(changed,a,d.modified);
        auto updated=api.updateDraft(created.id,updatedRaw,d.providerThread);
        expect(updated.id==created.id && updated.message.id!=created.message.id,"replacement keeps draft ID and replaces message ID");
        expect(api.draft(created.id).message.raw==updatedRaw,"updated draft fetch returns latest content");
        auto query=api.drafts({},"rfc822msgid:<draft-suite@example.test>");
        expect(query.drafts.size()==1 && query.drafts[0].id==created.id,"draft search locates a saved Message-ID");
        auto blank=Draft{}; blank.account=a.id; blank.messageId="<blank@example.test>";
        auto blankSaved=api.createDraft(composeDraftMessage(blank,a,d.modified));
        expect(!blankSaved.id.empty(),"recipient-free draft upload works");
        api.deleteDraft(blankSaved.id);
        failure([&]{api.draft(blankSaved.id);},NetworkFailure::Protocol,"deleted draft is authoritatively absent");
        expect(fixture("stats")["sent"].empty(),"draft creation, edits and deletion never deliver mail");
        fixture("replace",Json{{"id",created.id}}); // Another client changes server recipients.
        auto final=d; final.subject="Final 🎉"; auto wire=composeMessage(final,a,d.modified);
        auto sent=api.sendDraft(created.id,wire.raw,wire.recipients,d.providerThread);
        expect(sent.id=="sent_1" && sent.thread==d.providerThread,"draft send returns new sent-message identity");
        failure([&]{api.draft(created.id);},NetworkFailure::Protocol,"sending removes the stable draft resource");
        expect(wire.raw.find("Bcc:")==std::string::npos,"draft submission does not mutate the outbox cache");
        int refreshed=0;
        GmailApi refresh([&](bool force){if(force) ++refreshed; return force?"fixture-access":"expired-access";},cancel,endpoints);
        d.subject="Refreshed draft"; auto refreshedDraft=refresh.createDraft(composeDraftMessage(d,a,d.modified));
        expect(refreshed==1 && !refreshedDraft.id.empty(),"explicit 401 refreshes authorization once before draft creation");
        for(const std::string kind:{"Create disconnect","Create malformed","Create 503"}) {
            d.subject=kind; d.messageId="<uncertain-"+std::to_string(checks)+"@example.test>"; raw=composeDraftMessage(d,a,d.modified);
            failure([&]{api.createDraft(raw);},NetworkFailure::CopyUnknown,"ambiguous creation reports draft uncertainty without retrying");
            auto found=api.drafts({},"rfc822msgid:"+d.messageId);
            expect(found.drafts.size()==1 && api.draft(found.drafts[0].id).message.raw==raw,"saved draft remains discoverable after a lost receipt");
        }
        for(const std::string kind:{"Update disconnect","Update malformed"}) {
            d.subject=kind; raw=composeDraftMessage(d,a,d.modified);
            failure([&]{api.updateDraft(refreshedDraft.id,raw);},NetworkFailure::CopyUnknown,"ambiguous replacement is held for reconciliation");
            expect(api.draft(refreshedDraft.id).message.raw==raw,"replacement can be reconciled through stable draft ID");
        }
        for(const std::string kind:{"Create denied","Rate limited","Redirect"}) {
            d.subject=kind; raw=composeDraftMessage(d,a,d.modified); bool rejected=false;
            try { api.createDraft(raw); } catch(const HttpError& error) { rejected=error.status==(kind=="Create denied"?403:kind=="Rate limited"?429:307) && std::string(error.what()).find("fixture-secret")==std::string::npos; }
            expect(rejected,"explicit draft rejection preserves status without exposing response content");
        }
        for(const std::string kind:{"Send disconnect","Send malformed","Send 503"}) {
            d.subject="Prepare "+kind; auto pending=api.createDraft(composeDraftMessage(d,a,d.modified));
            d.subject=kind; auto outgoing=composeMessage(d,a,d.modified);
            failure([&]{api.sendDraft(pending.id,outgoing.raw,outgoing.recipients);},NetworkFailure::DeliveryUnknown,"uncertain draft delivery never retries automatically");
            failure([&]{api.draft(pending.id);},NetworkFailure::Protocol,"accepted ambiguous send removed its server draft");
        }
        d.subject="Delete disconnect"; auto deleting=api.createDraft(composeDraftMessage(d,a,d.modified));
        failure([&]{api.deleteDraft(deleting.id);},NetworkFailure::Connection,"lost deletion response remains an error until checked");
        failure([&]{api.draft(deleting.id);},NetworkFailure::Protocol,"a later 404 can confirm an interrupted deletion");
        for(const auto* id:{"wrong_id","bad_message","bad_raw"})
            failure([&]{api.draft(id);},NetworkFailure::Protocol,"malformed draft identities or MIME are rejected");
        failure([&]{api.drafts("oversized");},NetworkFailure::Protocol,"draft list is bounded to the requested page size");
        rejects([&]{api.updateDraft("../escape",raw);},"draft path identifiers are validated before a request");
        rejects([&]{api.createDraft("not MIME");},"invalid MIME cannot be uploaded");
        rejects([&]{api.createDraft(raw,"thread/invalid");},"reply thread identifiers are validated");
        rejects([&]{api.sendDraft("server_3",composeDraftMessage(d,a,d.modified),wire.recipients);},"delivery only accepts an outbox copy without a cached BCC header");
        cancel=true; failure([&]{api.createDraft(raw);},NetworkFailure::Cancelled,"cancelled draft write never starts"); cancel=false;
        auto stats=fixture("stats"); expect(stats["sent"].size()==4,"only explicit send operations delivered messages in the fixture");
        expect(!stats["leaked"].get<bool>(),"draft redirects never forward credentials");
        for(const std::string outcome:{"accepted","disconnect","denied","missing"}) {
            Store store(":memory:"); auto sender=account(); sender.id=store.saveAccount(sender);
            auto local=richDraft(); local.account=sender.id; local.subject="Prepare journal "+outcome;
            local.id=store.saveDraft(local); store.prepareGmailDrafts(sender.id);
            auto claim=store.beginGmailDraftWrite(store.claimGmailDraft(store.gmailDraftStates()[0]));
            auto saved=api.createDraft(claim.attemptRaw,claim.attemptThread); store.finishGmailDraftWrite(claim,saved);
            // Sending an edited local draft uses the durable delivery snapshot,
            // even if another client replaced its stored server content.
            fixture("replace",Json{{"id",saved.id}});
            if(outcome=="missing") api.deleteDraft(saved.id);
            local.subject="Send journal "+outcome; store.saveDraft(local);
            store.enqueueDraft(local.id,local.modified,local.revision);
            auto connect=[&](const Account&) { return std::make_unique<GmailApi>([](bool){return "fixture-access";},cancel,endpoints); };
            auto delivered=deliverQueued(store,[](const Account&)->std::string {throw std::runtime_error("Gmail draft must not use SMTP");},cancel,connect);
            if(outcome=="accepted") {
                expect(delivered.sent==1 && store.gmailDraftStates().empty() && store.drafts().empty(),"outbox draft delivery consumes the journal and local draft after a confirmed receipt");
                Query sentQuery; sentQuery.folder="sent";
                expect(store.conversations(sentQuery,local.modified).size()==1,"journal delivery creates exactly one local Sent conversation");
            } else {
                const bool uncertain=outcome=="disconnect";
                expect(delivered.uncertain==int(uncertain) && delivered.failed==int(!uncertain),"draft delivery classifies a lost receipt separately from explicit rejection");
                expect(store.gmailDraftStates().size()==1 && store.draft(local.id).subject==local.subject,"unsuccessful delivery retains both editable content and stable server identity");
                expect(store.outbox()[0].state==(uncertain?"uncertain":"failed"),"outbox persists draft-delivery attention");
            }
            auto repeated=deliverQueued(store,{},cancel,connect);
            expect(!repeated.sent && !repeated.failed && !repeated.uncertain,"another background pass does not replay a failed or uncertain draft delivery");
        }
        expect(fixture("stats")["sent"].size()==6,"journal dispatch delivered only the accepted and lost-receipt cases once");
        std::cout<<checks<<" Gmail draft transport and MIME checks passed\n";
        return 0;
    } catch(const std::exception& error) { std::cerr<<"FAIL: "<<error.what()<<'\n'; return 1; }
}
