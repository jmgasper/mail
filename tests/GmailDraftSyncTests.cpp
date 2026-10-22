#include "mail/Sync.h"
#include <nlohmann/json.hpp>
#include <iostream>
using namespace mail;
using Json=nlohmann::json;
namespace {
int checks=0;
void expect(bool value,const char* why) { ++checks; if (!value) throw std::runtime_error(why); }
template<class F> void rejects(F action,const char* why)
{ bool rejected=false; try { action(); } catch (const std::exception&) { rejected=true; } expect(rejected,why); }
Account account(Store& store)
{ Account a; a.name="Draft sync"; a.email="user@example.test"; a.provider=Provider::Gmail; a.id=store.saveAccount(a); return a; }
Draft local(Store& store,const Account& a,const std::string& subject="Local")
{ Draft d; d.account=a.id; d.subject=subject; d.to="recipient@example.test"; d.bcc="hidden@example.test"; d.text="Local body 🎉"; d.modified=1792542600; d.id=store.saveDraft(d); return d; }
std::string remote(const std::string& title="Server",const std::string& id="server")
{ return "From: user@example.test\r\nTo: recipient@example.test\r\nBcc: hidden@example.test\r\nSubject: "+title+"\r\nMessage-ID: <"+id+"@example.test>\r\nContent-Type: text/html; charset=utf-8\r\n\r\n<div style='color: purple'><b>Remote 🎉</b></div>"; }
}
int main(int argc,char** argv)
{
    if (argc!=4) return 2;
    try {
        std::atomic<bool> cancel{false}; const auto base="https://"+std::string(argv[1])+":"+argv[3];
        GoogleEndpoints endpoints; endpoints.gmail=base+"/gmail"; endpoints.certificate=argv[2];
        GmailApi api([](bool){return "fixture-access";},cancel,endpoints);
        auto fixture=[&](const std::string& path,const Json& body=Json()) {
            HttpRequest r; r.url=base+"/fixture/"+path; r.certificate=argv[2];
            if (!body.is_null()) { r.method="POST"; r.contentType="application/json"; r.body=body.dump(); }
            auto result=requestHttps(r,cancel); requireHttpSuccess(result); return Json::parse(result.body);
        };
        auto put=[&](const std::string& id,const std::string& raw) { fixture("put",{{"id",id},{"raw",base64UrlEncode(raw)}}); };
        {
            Store store(":memory:"); auto a=account(store);
            for (int i=0;i<3;++i) put("server"+std::to_string(i),remote("Server "+std::to_string(i),std::to_string(i)));
            auto report=syncGmailAccount(store,a,api,cancel);
            expect(report.downloaded==3 && store.drafts().size()==3,"full mail check imports every page of editable remote drafts");
            expect(fixture("stats")["create"]==0 && fixture("stats")["update"]==0,"import never rewrites unsupported original formatting");
            auto states=store.gmailDraftStates(a.id,true); auto s=states[0]; auto d=store.draft(s.draft);
            expect(s.raw==remote("Server 0","0") && d.bcc=="hidden@example.test" && d.text.find("Remote 🎉")!=std::string::npos,"original MIME and editable hidden recipients survive import");
            fixture("settings",{{"omit",s.remoteId}}); syncGmailDrafts(store,a,api,cancel);
            expect(store.drafts().size()==3,"absence from a listing does not delete a cached draft");
            put(s.remoteId,remote("Remote edit","0")); syncGmailDrafts(store,a,api,cancel);
            expect(store.draft(s.draft).subject=="Remote edit" && !store.gmailDraftState(s.id).conflict,"a closed clean draft adopts an external edit");
            auto editor=store.openDraft(s.draft,"editor");
            rejects([&]{store.openDraft(s.draft,"other");},"only one editor can own a draft snapshot");
            store.closeDraft(s.draft,"wrong-editor");
            put(s.remoteId,remote("While open","0")); syncGmailDrafts(store,a,api,cancel);
            expect(store.gmailDraftState(s.id).conflict && store.draft(s.draft).subject=="Remote edit","remote update preserves a live editor, including unsaved typing");
            store.closeDraft(s.draft,"editor"); syncGmailDrafts(store,a,api,cancel);
            expect(store.draft(s.draft).subject=="While open" && !store.gmailDraftState(s.id).conflict,"closing without local edits lets the pending remote version apply");
            editor=store.draft(s.draft); editor.text="Local competing body"; store.saveDraft(editor);
            put(s.remoteId,remote("Competing remote","0")); syncGmailDrafts(store,a,api,cancel);
            auto conflict=store.gmailDraftState(s.id);
            expect(conflict.conflict && conflict.observedRaw==remote("Competing remote","0") && store.draft(editor.id).text==editor.text,"both conflicting versions persist without an overwrite");
            rejects([&]{store.enqueueDraft(editor.id,1792542700,editor.revision);},"conflicting drafts cannot bypass review by sending");
            store.keepBothGmailDrafts(conflict);
            expect(store.drafts().size()==4 && store.draft(editor.id).subject=="Competing remote","keep both restores the Gmail version and makes a separate local draft");
            rejects([&]{store.keepBothGmailDrafts(conflict);},"stale review selection cannot create another copy");
            syncGmailDrafts(store,a,api,cancel);
            expect(fixture("stats")["create"]==1 && fixture("stats")["update"]==0,"keep both uploads exactly one new copy without replacing the remote edit");
            auto updated=store.draft(editor.id); updated.subject="Edited locally"; store.saveDraft(updated); syncGmailDrafts(store,a,api,cancel);
            expect(parseDraftMessage(api.draft(s.remoteId).message.raw,a.id).subject=="Edited locally" && fixture("stats")["update"]==1,"local editing updates the original stable server draft");
            fixture("drop",{{"id",s.remoteId}}); syncGmailDrafts(store,a,api,cancel);
            rejects([&]{store.draft(editor.id);},"authoritative deletion removes a closed unedited draft");
        }
        fixture("reset",Json::object());
        {
            Store store(":memory:"); auto a=account(store); auto d=local(store,a);
            fixture("settings",{{"behavior","disconnect"}}); auto first=syncGmailDrafts(store,a,api,cancel);
            expect(!first.errors.empty() && store.gmailDraftStates()[0].attempt=="create","lost creation response remains an unresolved saved attempt");
            expect(store.drafts().size()==1 && fixture("stats")["create"]==1,"listing the accepted uncertain create does not import a duplicate local draft");
            syncGmailDrafts(store,a,api,cancel); auto state=store.gmailDraftStates()[0];
            expect(state.attempt.empty() && !state.remoteId.empty() && fixture("stats")["create"]==1,"Message-ID search and exact raw MIME reconcile a create without replay");
            d.subject="Update after recovery"; store.saveDraft(d);
            fixture("settings",{{"behavior","disconnect"}}); syncGmailDrafts(store,a,api,cancel);
            expect(store.gmailDraftState(state.id).attempt=="update","lost replacement response retains its immutable attempt");
            syncGmailDrafts(store,a,api,cancel);
            expect(store.gmailDraftState(state.id).attempt.empty() && fixture("stats")["update"]==1,"stable draft GET confirms an interrupted update without replacing it twice");
            d.text="Keep despite remote removal"; store.saveDraft(d); fixture("drop",{{"id",state.remoteId}});
            syncGmailDrafts(store,a,api,cancel); state=store.gmailDraftState(state.id);
            expect(state.conflict && state.missing && store.draft(d.id).text==d.text,"remote deletion cannot erase newer local edits");
            auto oldMessage=d.messageId; store.keepBothGmailDrafts(state); syncGmailDrafts(store,a,api,cancel);
            expect(store.draft(d.id).messageId!=oldMessage && fixture("stats")["create"]==2,"explicit keep-local after deletion creates a fresh recoverable server draft");
            state=store.gmailDraftStates()[0]; store.removeDraft(d.id,store.draft(d.id).revision); syncGmailDrafts(store,a,api,cancel);
            expect(store.gmailDraftStates().empty() && fixture("stats")["drafts"].empty() && fixture("stats")["delete"]==1,"local discard removes its server draft once and retires the tombstone");
        }
        fixture("reset",Json::object());
        {
            Store store(":memory:"); auto a=account(store); auto d=local(store,a);
            fixture("settings",{{"behavior","denied"}}); syncGmailDrafts(store,a,api,cancel);
            expect(store.gmailDraftStates()[0].attempt.empty() && !store.gmailDraftStates()[0].error.empty(),"authoritative rejection allows a later new attempt");
            syncGmailDrafts(store,a,api,cancel);
            expect(fixture("stats")["create"]==2 && !store.gmailDraftStates()[0].remoteId.empty(),"a rejected upload can succeed on a later check");
            auto state=store.gmailDraftStates()[0]; store.removeDraft(d.id); put(state.remoteId,remote("Other device after discard"));
            syncGmailDrafts(store,a,api,cancel); state=store.gmailDraftState(state.id);
            expect(state.conflict && !state.draft && fixture("stats")["delete"]==0,"discard does not erase an intervening edit from another device");
            store.keepBothGmailDrafts(state);
            expect(store.drafts().size()==1 && store.drafts()[0].subject=="Other device after discard","review can retain an externally changed discarded draft");
        }
        fixture("reset",Json::object());
        {
            Store store(":memory:"); auto a=account(store); local(store,a);
            fixture("settings",{{"behavior","rewrite-disconnect"}}); syncGmailDrafts(store,a,api,cancel); syncGmailDrafts(store,a,api,cancel);
            expect(fixture("stats")["create"]==1 && store.drafts().size()==1 && store.gmailDraftStates()[0].attempt=="create",
                "rewritten uncertain content is held for review without duplicate upload or import");
            auto held=store.gmailDraftStates()[0]; auto matches=store.gmailDraftMatches(held.id);
            expect(matches.size()==1 && matches[0].subject=="Local","uncertain search exposes the rewritten copy for explicit review");
            store.useGmailDraftMatch(held,matches[0].id); syncGmailDrafts(store,a,api,cancel);
            expect(store.gmailDraftStates()[0].attempt.empty() && store.drafts().size()==1 && fixture("stats")["create"]==1
                && fixture("stats")["update"]==0,"choosing a header-rewritten copy resolves uncertainty without another upload");
            fixture("settings",{{"identity","wrong@example.test"}});
            rejects([&]{syncGmailDrafts(store,a,api,cancel);},"draft work verifies Google identity before any mutation");
            expect(fixture("stats")["create"]==1,"wrong account cannot cause a draft upload");
            cancel=true; rejects([&]{syncGmailDrafts(store,a,api,cancel);},"cancelled draft synchronization never starts"); cancel=false;
        }
        fixture("reset",Json::object());
        {
            Store store(":memory:"); auto a=account(store); local(store,a);
            fixture("settings",{{"behavior","disconnect"}}); syncGmailDrafts(store,a,api,cancel);
            auto held=store.gmailDraftStates(a.id,true)[0]; put("another-copy",held.attemptRaw);
            syncGmailDrafts(store,a,api,cancel); held=store.gmailDraftState(held.id);
            auto matches=store.gmailDraftMatches(held.id);
            expect(matches.size()==2 && held.attempt=="create" && store.drafts().size()==1,"multiple exact results remain separate review choices");
            store.useGmailDraftMatch(held,matches[1].id); syncGmailDrafts(store,a,api,cancel);
            expect(store.gmailDraftState(held.id).remoteId==matches[1].id && store.drafts().size()==2
                && fixture("stats")["create"]==1 && fixture("stats")["delete"]==0,"choosing one exact copy leaves the other server copy intact and discoverable");
        }
        fixture("reset",Json::object());
        {
            Store store(":memory:"); auto a=account(store); auto d=local(store,a);
            fixture("settings",{{"behavior","disconnect"}}); syncGmailDrafts(store,a,api,cancel);
            auto held=store.gmailDraftStates(a.id,true)[0]; auto remoteId=fixture("stats")["drafts"][0].get<std::string>();
            auto changed=d; changed.subject="Edited on another device";
            auto changedRaw=composeDraftMessage(changed,a,1792542600); put(remoteId,changedRaw);
            syncGmailDrafts(store,a,api,cancel); store.useGmailDraftMatch(store.gmailDraftState(held.id),remoteId);
            auto copies=store.drafts(); bool kept=false;
            for (const auto& copy:copies) if (copy.id!=d.id) kept=copy.subject==d.subject && copy.messageId!=d.messageId;
            expect(copies.size()==2 && kept && store.draft(d.id).subject==changed.subject,"choosing an externally edited copy preserves local content with its own identity");
            syncGmailDrafts(store,a,api,cancel);
            expect(api.draft(remoteId).message.raw==changedRaw && fixture("stats")["create"]==2 && fixture("stats")["update"]==0,
                "preserved local version creates one new draft without overwriting the chosen remote version");
        }
        fixture("reset",Json::object());
        {
            Store store(":memory:"); auto a=account(store); auto d=local(store,a);
            fixture("settings",{{"behavior","disconnect"}}); syncGmailDrafts(store,a,api,cancel);
            auto held=store.gmailDraftStates(a.id,true)[0]; auto previousRaw=held.attemptRaw;
            auto oldId=fixture("stats")["drafts"][0].get<std::string>(); fixture("drop",{{"id",oldId}});
            syncGmailDrafts(store,a,api,cancel);
            expect(store.gmailDraftMatches(held.id).empty() && store.gmailDraftState(held.id).attempt=="create","an empty search still holds the previous attempt until a decision");
            store.retryGmailDraftCreate(store.gmailDraftState(held.id)); syncGmailDrafts(store,a,api,cancel);
            auto retried=store.gmailDraftState(held.id); put(oldId,previousRaw); syncGmailDrafts(store,a,api,cancel);
            expect(retried.remoteId!=oldId && store.gmailDraftState(held.id).remoteId==retried.remoteId && store.draft(d.id).messageId!=d.messageId
                && store.drafts().size()==2 && fixture("stats")["create"]==2,"explicit retry has a fresh identity and a late old copy cannot take over its journal");
        }
        fixture("reset",Json::object());
        {
            Store store(":memory:"); auto a=account(store); auto d=local(store,a);
            fixture("settings",{{"behavior","rewrite-disconnect"}}); syncGmailDrafts(store,a,api,cancel);
            store.removeDraft(d.id); syncGmailDrafts(store,a,api,cancel);
            auto held=store.gmailDraftStates()[0]; store.forgetDiscardedGmailDraft(held); syncGmailDrafts(store,a,api,cancel);
            expect(store.drafts().size()==1 && fixture("stats")["create"]==1 && fixture("stats")["delete"]==0,
                "stopping an uncertain discarded save keeps its server copy for later import");
        }
        std::cout<<checks<<" Gmail draft synchronization checks passed\n"; return 0;
    } catch (const std::exception& e) { std::cerr<<"FAIL: "<<e.what()<<'\n'; return 1; }
}
