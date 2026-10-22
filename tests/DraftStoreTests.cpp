#include "mail/Store.h"
#include <filesystem>
#include <iostream>
#include <sqlite3.h>
#include <unistd.h>

using namespace mail;
namespace {
int checks=0;
void expect(bool value,const char* text) { ++checks; if (!value) throw std::runtime_error(text); }
template<class F> void rejects(F action,const char* text)
{ bool failed=false; try { action(); } catch (const std::exception&) { failed=true; } expect(failed,text); }
Account account(Store& store,const std::string& name)
{ Account a; a.name=name; a.email=name+"@example.test"; a.provider=Provider::Gmail; a.id=store.saveAccount(a); return a; }
Draft draft(Store& store,Id account)
{
    Draft d; d.account=account; d.to="recipient@example.test"; d.bcc="hidden@example.test";
    d.subject="Draft 🎉"; d.text="Local text"; d.modified=1792542600;
    d.attachments={{"binary.dat","application/octet-stream","",std::string("a\0z",3),false}};
    d.id=store.saveDraft(d); return d;
}
GmailDraft receipt(const std::string& id="remote",const std::string& message="message")
{ return {id,{message,"thread",{}, {"DRAFT"},1792542600,{}}}; }
GmailDraftState state(Store& store,Id account=0)
{ auto values=store.gmailDraftStates(account); expect(values.size()==1,"one draft journal entry expected"); return values[0]; }
void edits(const std::string& path)
{
    Store one(path),two(path); auto a=account(one,"edits"); auto d=draft(one,a.id);
    expect(d.revision==1 && !d.messageId.empty(),"saving returns revision and generated Message-ID to the editor");
    auto stale=two.draft(d.id); const auto originalModified=d.modified;
    d.modified+=20; one.saveDraft(d);
    expect(d.revision==1 && d.modified==originalModified,"unchanged close/save preserves version and modification time");
    d.text+=" — newer typing"; d.modified+=30; one.saveDraft(d);
    expect(d.revision==2 && two.draft(d.id).text==d.text,"edits increment the persistent version across connections");
    stale.subject="Stale edit";
    rejects([&]{two.saveDraft(stale);},"a stale editor cannot overwrite another save");
    rejects([&]{two.removeDraft(stale.id,stale.revision);},"a stale discard cannot remove newer content");
    rejects([&]{two.enqueueDraft(stale.id,1792542700,stale.revision);},"a stale send cannot submit unseen newer content");
    expect(stale.revision==1 && two.draft(d.id).revision==2,"rejected changes leave editor and database versions intact");
    d.runs={{0,true,false,false,{}}}; one.saveDraft(d);
    expect(d.revision==3,"formatting-only edits need a new server save");
    d.attachments[0].data[1]='x'; one.saveDraft(d);
    expect(d.revision==4,"binary attachment-only edits need a new server save");
    auto bad=d; bad.to="bad\nheader";
    rejects([&]{one.saveDraft(bad);},"invalid local draft does not update a revision");
    expect(two.draft(d.id).revision==4,"failed validation leaves committed revision unchanged");
    auto queued=one.enqueueDraft(d.id,1792542700,d.revision);
    rejects([&]{one.saveDraft(d);},"queued immutable content cannot be edited");
    one.prepareGmailDrafts(a.id);
    expect(one.gmailDraftStates().empty(),"preparation does not create server copies of queued messages");
    one.returnToDraft(queued); one.prepareGmailDrafts(a.id); one.prepareGmailDrafts(a.id);
    expect(one.gmailDraftStates().size()==1,"preparation is idempotent after returning to Drafts");
    Account local; local.name="Local"; local.email="local@example.test"; local.id=one.saveAccount(local);
    rejects([&]{one.prepareGmailDrafts(local.id);},"local-only drafts are never enrolled for Gmail uploads");
}
void writes(const std::string& path)
{
    Store one(path),two(path); auto a=account(one,"writes"); auto d=draft(one,a.id);
    one.prepareGmailDrafts(a.id); auto candidate=state(two); auto first=one.claimGmailDraft(candidate);
    expect(first.active && first.revision>candidate.revision,"claim advances a durable worker token");
    expect(!two.claimGmailDraft(candidate).id,"another connection cannot claim the same draft");
    rejects([&]{two.removeAccount(a.id);},"account removal waits for a running draft operation");
    auto changedAccount=a; changedAccount.email="wrong@example.test";
    rejects([&]{two.saveAccount(changedAccount);},"tracked draft cannot be retargeted to another Google identity");
    auto writing=one.beginGmailDraftWrite(first);
    expect(writing.attempt=="create" && writing.attemptRevision==1 && writing.attemptRaw.find("Bcc:")!=std::string::npos,"create snapshot durably includes the hidden recipients");
    auto snapshot=writing.attemptRaw;
    expect(state(two).attemptRaw.empty() && two.gmailDraftStates(a.id,true)[0].attemptRaw==snapshot,"status reads omit MIME while recovery reads retain its exact bytes");
    d.text="New text during upload"; two.saveDraft(d);
    expect(two.gmailDraftStates(a.id,true)[0].attemptRaw==snapshot,"editing during an upload cannot change the uploaded snapshot");
    rejects([&]{one.beginGmailDraftWrite(first);},"a pending create cannot be blindly repeated");
    one.finishGmailDraftWrite(writing,receipt());
    auto confirmed=state(two);
    expect(confirmed.remoteId=="remote" && confirmed.messageId=="message" && confirmed.savedRevision==1 && !confirmed.active,
        "receipt binds stable draft ID and acknowledges only the uploaded revision");
    expect(two.draft(d.id).revision==2 && two.draft(d.id).text=="New text during upload","late receipt preserves newer typing");
    rejects([&]{one.releaseGmailDraft(first,"late error");},"late callback cannot alter a finished claim");
    auto update=two.beginGmailDraftWrite(two.claimGmailDraft(confirmed));
    expect(update.attempt=="update" && update.attemptRevision==2 && update.attemptRaw!=snapshot,"next upload captures the new local version");
    rejects([&]{two.finishGmailDraftWrite(update,receipt("wrong"));},"update cannot switch its stable server identity");
    rejects([&]{two.finishGmailDraftWrite(update,receipt("remote","bad/id"));},"malformed receipt cannot acknowledge a write");
    two.releaseGmailDraft(update,"Lost response");
    auto uncertain=state(one);
    expect(uncertain.attempt=="update" && uncertain.error=="Lost response" && !uncertain.active,"uncertain upload retains its state after release");
    auto checking=one.claimGmailDraft(uncertain);
    rejects([&]{one.beginGmailDraftWrite(checking);},"uncertain update must be reconciled before another mutation");
    one.finishGmailDraftWrite(checking,receipt("remote","message-new"));
    auto clean=one.claimGmailDraft(state(one));
    rejects([&]{one.beginGmailDraftWrite(clean);},"a fully synchronized revision is not uploaded again");
    one.releaseGmailDraft(clean,{});
    expect(state(one).messageId=="message-new" && one.gmailDraftStates(a.id,true)[0].raw==update.attemptRaw,"changing message ID retains the same draft and acknowledged bytes");
}
void discardAndAccounts(const std::string& path)
{
    Store one(path),two(path); auto a=account(one,"discard"),b=account(one,"other"); auto d=draft(one,a.id);
    one.prepareGmailDrafts(a.id); auto writing=one.beginGmailDraftWrite(one.claimGmailDraft(state(one,a.id)));
    two.removeDraft(d.id,d.revision);
    expect(one.drafts().empty() && !state(two,a.id).draft,"discard removes local content and retains remote cleanup state");
    rejects([&]{one.beginGmailDraftDelete(writing);},"discard cannot delete a server draft whose identity is still unknown");
    one.finishGmailDraftWrite(writing,receipt());
    auto deleting=one.beginGmailDraftDelete(one.claimGmailDraft(state(one,a.id)));
    expect(!deleting.draft && deleting.remoteId=="remote" && deleting.attempt=="delete","late create receipt can still be cleaned up after discard");
    one.releaseGmailDraft(deleting,"Deletion response lost");
    auto retry=two.claimGmailDraft(state(two,a.id));
    rejects([&]{two.beginGmailDraftWrite(retry);},"discarded draft cannot be uploaded again");
    two.finishGmailDraftDelete(retry);
    expect(one.gmailDraftStates().empty(),"confirmed deletion removes the durable tombstone");
    d=draft(one,a.id); one.prepareGmailDrafts(a.id);
    writing=one.beginGmailDraftWrite(one.claimGmailDraft(state(one,a.id)));
    d.account=b.id; d.providerThread.clear(); two.saveDraft(d); two.prepareGmailDrafts(b.id);
    expect(!state(one,a.id).draft && state(one,b.id).draft==d.id,"changing From detaches cleanup from the new account's draft");
    one.finishGmailDraftWrite(writing,receipt());
    auto newWrite=two.beginGmailDraftWrite(two.claimGmailDraft(state(two,b.id)));
    expect(newWrite.account==b.id && newWrite.attempt=="create" && newWrite.attemptRaw.find("From: other@example.test")!=std::string::npos,
        "new account creates its own draft using its own identity");
    two.finishGmailDraftWrite(newWrite,receipt());
    expect(one.gmailDraftStates().size()==2,"identical opaque server IDs remain isolated by account");
    auto old=one.claimGmailDraft(state(one,a.id)); one.finishGmailDraftDelete(old);
    expect(two.draft(d.id).account==b.id && two.gmailDraftStates().size()==1,"old-account cleanup cannot remove new-account content");
    two.removeAccount(b.id);
    expect(one.gmailDraftStates().empty() && one.drafts().empty(),"local account removal cascades only its cached draft records");
    d=draft(one,a.id); one.prepareGmailDrafts(a.id); one.removeDraft(d.id);
    expect(one.gmailDraftStates().empty(),"discard before any upload leaves no unnecessary cleanup job");
    d=draft(one,a.id); one.prepareGmailDrafts(a.id); auto checking=one.claimGmailDraft(state(one)); two.removeDraft(d.id);
    one.releaseGmailDraft(checking,{});
    expect(one.gmailDraftStates().empty(),"discard during a read-only claim cleans up when that claim ends");
}
void outbox(const std::string& path)
{
    Store one(path),two(path); auto a=account(one,"send"); auto d=draft(one,a.id); d.providerThread="saved-thread"; one.saveDraft(d);
    one.prepareGmailDrafts(a.id); auto checking=one.claimGmailDraft(state(one));
    auto queued=two.enqueueDraft(d.id,1792542700,d.revision);
    expect(!two.readyOutgoing() && !two.claimOutgoing().id,"sending waits for an active read-only draft claim");
    rejects([&]{one.beginGmailDraftWrite(checking);},"queueing prevents a checked draft from starting an upload");
    one.releaseGmailDraft(checking,{});
    expect(two.readyOutgoing(),"release wakes a send with no pending server mutation");
    expect(!one.claimGmailDraft(state(one)).id,"draft work cannot claim a queued message without an unresolved save");
    two.returnToDraft(queued);
    auto writing=one.beginGmailDraftWrite(one.claimGmailDraft(state(one)));
    queued=two.enqueueDraft(d.id,1792542700,d.revision);
    one.releaseGmailDraft(writing,"Lost create response");
    expect(!two.readyOutgoing() && !two.claimOutgoing().id,"uncertain create continues to block sending after worker release");
    checking=one.claimGmailDraft(state(one));
    expect(checking.id && checking.attempt=="create","reconciliation can claim an unresolved save despite queued sending");
    one.finishGmailDraftWrite(checking,receipt());
    auto outgoing=two.claimOutgoing();
    expect(outgoing.id==queued && outgoing.gmailDraft=="remote" && outgoing.providerThread=="saved-thread","delivery claim captures the stable server ID and reply thread");
    expect(outgoing.raw.find("Bcc:")==std::string::npos && outgoing.recipients.size()==2,"immutable delivery keeps BCC only in its envelope");
    expect(!one.claimGmailDraft(state(one)).id,"a sending message excludes server draft work");
    two.finishOutgoing(queued,"uncertain","Lost send response",1792542700);
    expect(!one.claimGmailDraft(state(one)).id,"uncertain delivery cannot recreate its possibly consumed draft");
    two.returnToDraft(queued,true);
    expect(one.claimGmailDraft(state(one)).id!=0,"explicit return to Drafts unlocks server reconciliation");
    one.recoverOutgoing(); // Simulate restart to release the just-claimed reader.
    queued=two.enqueueDraft(d.id,1792542700,d.revision); outgoing=two.claimOutgoing();
    two.finishOutgoing(outgoing.id,"sent",{},1792542700,{"sent-message","sent-thread"});
    expect(one.gmailDraftStates().empty() && one.drafts().empty() && one.outbox().empty(),"confirmed delivery atomically retires its draft and journal");
    expect(one.findRemote(a.id,"gmail:sent-message")!=0,"confirmed Gmail delivery still records its Sent message");
}
void recovery(const std::string& path)
{
    Id aId=0,dId=0; GmailDraftState old; std::string raw;
    {
        Store store(path); auto a=account(store,"recovery"); aId=a.id; auto d=draft(store,a.id); dId=d.id;
        store.prepareGmailDrafts(a.id); old=store.beginGmailDraftWrite(store.claimGmailDraft(state(store))); raw=old.attemptRaw;
    }
    Store reopened(path);
    expect(state(reopened).active,"opening a connection does not steal an active worker's claim");
    reopened.recoverOutgoing(); auto recovered=state(reopened);
    expect(!recovered.active && recovered.attempt=="create" && recovered.revision>old.revision,"startup releases the claim without replaying the upload");
    expect(reopened.gmailDraftStates(aId,true)[0].attemptRaw==raw,"exact recovery MIME survives close and reopen");
    rejects([&]{reopened.finishGmailDraftWrite(old,receipt());},"pre-crash callback cannot confirm a new worker's attempt");
    auto check=reopened.claimGmailDraft(recovered);
    rejects([&]{reopened.beginGmailDraftWrite(check);},"restart does not authorize a duplicate create");
    reopened.releaseGmailDraft(check,"Rejected before acceptance",true);
    expect(state(reopened).attempt.empty() && reopened.gmailDraftStates(aId,true)[0].attemptRaw.empty(),"authoritative rejection clears only the failed attempt");
    auto d=reopened.draft(dId); d.to="unfinished@"; reopened.saveDraft(d);
    check=reopened.claimGmailDraft(state(reopened));
    rejects([&]{reopened.beginGmailDraftWrite(check);},"unfinished addresses stay local without a misleading upload attempt");
    reopened.releaseGmailDraft(check,"Complete the recipient");
    expect(reopened.draft(dId).to=="unfinished@" && state(reopened).attempt.empty(),"failed serialization retains editable partial input");
    d.to="recipient@example.test"; reopened.saveDraft(d); check=reopened.beginGmailDraftWrite(reopened.claimGmailDraft(state(reopened)));
    reopened.removeDraft(dId); reopened.releaseGmailDraft(check,"Rejected",true);
    expect(reopened.gmailDraftStates().empty(),"discarded rejected create does not leave an unrecoverable job");
}
void migration(const std::string& path)
{
    Id dId=0,outId=0;
    {
        Store store(path); auto a=account(store,"migration"); auto d=draft(store,a.id); dId=d.id; outId=store.enqueueDraft(d.id,1792542700);
    }
    sqlite3* db=nullptr; expect(sqlite3_open(path.c_str(),&db)==SQLITE_OK,"open schema fixture");
    const auto rc=sqlite3_exec(db,"DROP TABLE gmail_draft_matches; DROP TABLE draft_editors; DROP TABLE gmail_drafts; ALTER TABLE drafts DROP COLUMN revision; PRAGMA user_version=11",nullptr,nullptr,nullptr); sqlite3_close(db);
    expect(rc==SQLITE_OK,"restore a schema-11 draft and queued delivery");
    Store store(path); auto d=store.draft(dId);
    expect(d.revision==1 && d.attachments[0].data==std::string("a\0z",3),"schema-12 migration preserves binary draft data and initializes its version");
    expect(store.readyOutgoing() && store.outbox()[0].id==outId,"schema migration preserves immutable pending delivery");
    expect(store.gmailDraftStates().empty(),"migration itself does not enroll or upload existing drafts");
}
void review(const std::string& path)
{
    Id journal=0,localId=0; std::string raw;
    {
        Store store(path); auto a=account(store,"review"); auto d=draft(store,a.id); localId=d.id;
        store.prepareGmailDrafts(a.id); auto claim=store.claimGmailDraft(state(store));
        auto writing=store.beginGmailDraftWrite(claim); journal=claim.id; raw=writing.attemptRaw;
        auto match=receipt("chosen"); match.message.raw="X-Rewritten: yes\r\n"+raw;
        store.recordGmailDraftMatches(claim,{match});
        rejects([&]{store.useGmailDraftMatch(claim,"chosen");},"review cannot change an active worker");
        rejects([&]{store.retryGmailDraftCreate(claim);},"retry cannot overlap an active save");
        store.releaseGmailDraft(claim,"Review copies");
    }
    Store store(path); auto candidate=store.gmailDraftState(journal);
    expect(store.gmailDraftMatches(journal).size()==1 && store.gmailDraftMatch(journal,"chosen").message.raw=="X-Rewritten: yes\r\n"+raw,
        "candidate MIME and identity survive reopening the database");
    store.openDraft(localId,"editor");
    rejects([&]{store.useGmailDraftMatch(candidate,"chosen");},"candidate selection cannot replace an open composer");
    rejects([&]{store.retryGmailDraftCreate(candidate);},"explicit retry cannot change an open composer identity");
    store.closeDraft(localId,"editor"); auto queued=store.enqueueDraft(localId,1792542700);
    rejects([&]{store.useGmailDraftMatch(candidate,"chosen");},"candidate selection cannot change an immutable queued message");
    store.returnToDraft(queued);
    rejects([&]{store.useGmailDraftMatch(candidate,"unknown");},"review accepts only a recorded candidate identity");
    auto newer=store.draft(localId); newer.subject="New typing after uncertain upload"; store.saveDraft(newer);
    store.useGmailDraftMatch(candidate,"chosen"); auto bound=store.gmailDraftState(journal);
    expect(bound.remoteId=="chosen" && bound.raw=="X-Rewritten: yes\r\n"+raw && bound.savedRevision<newer.revision
        && store.draft(localId).subject==newer.subject && store.drafts().size()==1,"choosing a header-rewritten copy retains new local edits without duplicating the draft");
    expect(store.gmailDraftMatches(journal).empty(),"resolved candidates are retired atomically");
    rejects([&]{store.useGmailDraftMatch(candidate,"chosen");},"stale confirmation cannot rebind a resolved draft");
    rejects([&]{store.retryGmailDraftCreate(candidate);},"stale confirmation cannot retry a resolved save");

    auto d=draft(store,bound.account); store.prepareGmailDrafts(bound.account);
    auto claim=store.claimGmailDraft(store.gmailDraftStates().back()); auto writing=store.beginGmailDraftWrite(claim);
    auto match=receipt("chosen"); match.message.raw=writing.attemptRaw;
    store.recordGmailDraftMatches(claim,{match}); store.releaseGmailDraft(claim,"Review"); candidate=store.gmailDraftState(claim.id);
    rejects([&]{store.useGmailDraftMatch(candidate,"chosen");},"one remote identity cannot belong to two local journal entries");
    claim=store.claimGmailDraft(candidate); auto wrong=match; wrong.message.raw=raw;
    rejects([&]{store.recordGmailDraftMatches(claim,{wrong});},"a candidate with a different Message-ID is rejected");
    expect(store.gmailDraftMatches(claim.id).size()==1,"failed candidate replacement rolls back the prior review");
    store.releaseGmailDraft(claim,"Review"); candidate=store.gmailDraftState(claim.id);
    rejects([&]{store.forgetDiscardedGmailDraft(candidate);},"stopping a discarded save cannot detach existing local work");
    store.retryGmailDraftCreate(candidate); auto retry=store.gmailDraftState(claim.id);
    expect(store.draft(d.id).messageId!=d.messageId && retry.attempt.empty() && retry.attemptRaw.empty()
        && store.gmailDraftMatches(claim.id).empty(),"explicit retry changes Message-ID and retires the previous attempt and candidates");
    rejects([&]{store.recordGmailDraftMatches(claim,{match});},"late worker cannot restore candidates after a retry decision");
    claim=store.claimGmailDraft(retry); store.beginGmailDraftWrite(claim); store.removeDraft(d.id); store.releaseGmailDraft(claim,"Discarded");
    store.forgetDiscardedGmailDraft(store.gmailDraftState(claim.id));
    rejects([&]{store.gmailDraftState(claim.id);},"explicitly stopping an uncertain discarded save retires its journal");
}
void reviewMigration(const std::string& path)
{
    Id id=0; std::string raw;
    { Store store(path); auto a=account(store,"oldreview"); draft(store,a.id); store.prepareGmailDrafts(a.id);
      auto claim=store.claimGmailDraft(state(store)); id=claim.id; raw=store.beginGmailDraftWrite(claim).attemptRaw;
      store.releaseGmailDraft(claim,"Uncertain schema-13 save"); }
    sqlite3* db=nullptr; expect(sqlite3_open(path.c_str(),&db)==SQLITE_OK,"open schema-13 fixture");
    const auto rc=sqlite3_exec(db,"DROP TABLE gmail_draft_matches; PRAGMA user_version=13",nullptr,nullptr,nullptr); sqlite3_close(db);
    expect(rc==SQLITE_OK,"restore schema-13 unresolved draft journal");
    Store store(path); auto saved=store.gmailDraftState(id);
    expect(saved.attempt=="create" && saved.attemptRaw==raw && saved.error=="Uncertain schema-13 save" && store.gmailDraftMatches(id).empty(),
        "schema-14 migration preserves unresolved MIME and starts with no guessed candidates");
}
}
int main()
{
    char directory[]="mail-drafts-XXXXXX"; if (!mkdtemp(directory)) return 2;
    const auto path=std::string(directory)+"/";
    try {
        edits(path+"edits.db"); writes(path+"writes.db"); discardAndAccounts(path+"discard.db");
        outbox(path+"outbox.db"); recovery(path+"recovery.db"); migration(path+"migration.db");
        review(path+"review.db"); reviewMigration(path+"review-migration.db");
        std::filesystem::remove_all(directory);
        std::cout << "Draft storage: " << checks << " assertions passed\n"; return 0;
    } catch (const std::exception& e) {
        std::filesystem::remove_all(directory); std::cerr << "FAIL: " << e.what() << '\n'; return 1;
    }
}
