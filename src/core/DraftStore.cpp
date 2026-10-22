#include "mail/Store.h"
#include "StoreSql.h"
#include <algorithm>
#include <ctime>
#include <tuple>

namespace mail {
using namespace storage;
namespace {
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
bool sameContent(const Draft& a,const Draft& b)
{
    if (std::tie(a.account,a.messageId,a.to,a.cc,a.bcc,a.subject,a.text,a.references,a.providerThread)
        !=std::tie(b.account,b.messageId,b.to,b.cc,b.bcc,b.subject,b.text,b.references,b.providerThread)) return false;
    return a.runs.size()==b.runs.size() && a.attachments.size()==b.attachments.size() && a.images.size()==b.images.size()
        && std::equal(a.runs.begin(),a.runs.end(),b.runs.begin(),[](const TextRun& x,const TextRun& y) {
            return std::tie(x.offset,x.bold,x.italic,x.underline,x.link)==std::tie(y.offset,y.bold,y.italic,y.underline,y.link); })
        && std::equal(a.attachments.begin(),a.attachments.end(),b.attachments.begin(),[](const Attachment& x,const Attachment& y) {
            return std::tie(x.name,x.mediaType,x.contentId,x.data,x.inlineImage)==std::tie(y.name,y.mediaType,y.contentId,y.data,y.inlineImage); })
        && std::equal(a.images.begin(),a.images.end(),b.images.begin(),[](const InlineImage& x,const InlineImage& y) {
            return std::tie(x.offset,x.length,x.contentId)==std::tie(y.offset,y.length,y.contentId); });
}
void gmailAccount(sqlite3* db,Id id)
{
    Statement s(db,"SELECT provider FROM accounts WHERE id=?"); s.bind(1,id);
    require(s.step() && s.number(0)==static_cast<Id>(Provider::Gmail),"Choose a Gmail account for server drafts.");
}
void identifier(const std::string& value)
{
    require(!value.empty() && value.size()<=256 && value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")==std::string::npos,
        "Invalid Gmail draft identifier.");
}
GmailDraftState readState(sqlite3* db,Id id,bool raw=true)
{
    Statement s(db,R"SQL(SELECT id,COALESCE(draft_id,0),account_id,revision,active,remote_id,message_id,thread_id,
 CASE WHEN ?2 THEN raw ELSE X'' END,saved_revision,attempt,attempt_revision,
 CASE WHEN ?2 THEN attempt_raw ELSE X'' END,attempt_thread,error,conflict,missing,observed_id,observed_thread,
 CASE WHEN ?2 THEN observed_raw ELSE X'' END FROM gmail_drafts WHERE id=?1)SQL");
    s.bind(1,id); s.bind(2,raw); require(s.step(),"This draft's server record no longer exists.");
    GmailDraftState d;
    d.id=s.number(0); d.draft=s.number(1); d.account=s.number(2); d.revision=s.number(3); d.active=s.number(4);
    d.remoteId=s.text(5); d.messageId=s.text(6); d.thread=s.text(7); d.raw=s.text(8); d.savedRevision=s.number(9);
    d.attempt=s.text(10); d.attemptRevision=s.number(11); d.attemptRaw=s.text(12); d.attemptThread=s.text(13); d.error=s.text(14);
    d.conflict=s.number(15); d.missing=s.number(16); d.observedId=s.text(17); d.observedThread=s.text(18); d.observedRaw=s.text(19);
    return d;
}
GmailDraftState claimed(sqlite3* db,const GmailDraftState& claim)
{
    auto current=readState(db,claim.id);
    require(current.active && current.account==claim.account && current.revision==claim.revision,
        "This draft check has finished or been replaced. Check its current status.");
    return current;
}
void detachDraft(sqlite3* db,Id id)
{
    // Keep the old account's server identity and any in-flight snapshot even if
    // the composer changes From or the user discards its local content.
    Statement remove(db,"DELETE FROM gmail_drafts WHERE draft_id=? AND remote_id='' AND attempt='' AND active=0");
    remove.bind(1,id); remove.step();
    Statement detach(db,"UPDATE gmail_drafts SET draft_id=NULL WHERE draft_id=?"); detach.bind(1,id); detach.step();
}
bool protectedDraft(sqlite3* db,Id id)
{
    Statement s(db,"SELECT 1 FROM draft_editors WHERE draft_id=?1 UNION ALL SELECT 1 FROM outbox WHERE draft_id=?1");
    s.bind(1,id); return s.step();
}
void validateRemote(const GmailDraft& remote)
{
    identifier(remote.id); identifier(remote.message.id); identifier(remote.message.thread);
    require(!remote.message.raw.empty() && remote.message.raw.size()<=32*1024*1024,"This Gmail draft needs a complete supported message.");
}
void clearMatches(sqlite3* db,Id id)
{ Statement s(db,"DELETE FROM gmail_draft_matches WHERE journal_id=?"); s.bind(1,id); s.step(); }
GmailDraft readMatch(sqlite3* db,Id id,const std::string& remoteId)
{
    Statement s(db,"SELECT remote_id,message_id,thread_id,raw FROM gmail_draft_matches WHERE journal_id=? AND remote_id=?");
    s.bind(1,id); s.bind(2,remoteId); require(s.step(),"This Gmail copy is no longer in the review. Check Gmail again.");
    GmailDraft d; d.id=s.text(0); d.message.id=s.text(1); d.message.thread=s.text(2); d.message.raw=s.text(3); return d;
}
GmailDraftState reviewedCreate(sqlite3* db,const GmailDraftState& candidate)
{
    auto state=readState(db,candidate.id);
    require(!state.active && state.revision==candidate.revision && state.account==candidate.account
        && state.attempt=="create" && state.remoteId.empty(),"This draft save changed or is being checked. Review its current state.");
    require(!state.draft || !protectedDraft(db,state.draft),"Close the composer and return queued mail to Drafts before resolving this save.");
    return state;
}
}

Id Store::saveDraft(Draft& value)
{
    validateDraft(value); std::lock_guard<std::mutex> guard(mutex_); Transaction transaction(db_);
    Draft staged=value; auto id=saveDraftLocked(staged); transaction.commit();
    value.revision=staged.revision; value.messageId=std::move(staged.messageId); value.modified=staged.modified; return id;
}
Id Store::saveDraftLocked(Draft& value)
{
    validateDraft(value);
    Draft d=value; if (d.messageId.empty()) d.messageId=newMessageId();
    if (d.id) {
        Statement locked(db_,"SELECT 1 FROM outbox WHERE draft_id=?"); locked.bind(1,d.id);
        require(!locked.step(),"Return this message to Drafts from the Outbox before editing it.");
        auto previous=readDraft(d.id);
        require(previous.revision==d.revision,"This draft changed in another window or on the server. Your edits have not been saved over the newer version.");
        if (sameContent(previous,d)) {
            value.modified=previous.modified; return d.id;
        }
        if (previous.account!=d.account) detachDraft(db_,d.id);
    }
    Statement s(db_,R"SQL(INSERT INTO drafts(id,account_id,message_id,recipient,cc,bcc,subject,body,provider_thread,modified)
VALUES(NULLIF(?1,0),?2,?3,?4,?5,?6,?7,?8,?9,?10)
ON CONFLICT(id) DO UPDATE SET account_id=excluded.account_id,message_id=excluded.message_id,recipient=excluded.recipient,
cc=excluded.cc,bcc=excluded.bcc,subject=excluded.subject,body=excluded.body,provider_thread=excluded.provider_thread,modified=excluded.modified,
revision=drafts.revision+1)SQL");
    s.bind(1,d.id); s.bind(2,d.account); s.bind(3,d.messageId); s.bind(4,d.to); s.bind(5,d.cc); s.bind(6,d.bcc);
    s.bind(7,d.subject); s.bind(8,d.text); s.bind(9,d.providerThread); s.bind(10,d.modified); s.step();
    if (!d.id) { d.id=sqlite3_last_insert_rowid(db_); d.revision=1; } else ++d.revision;
    for (const char* sql:{"DELETE FROM draft_runs WHERE draft_id=?","DELETE FROM draft_refs WHERE draft_id=?","DELETE FROM draft_attachments WHERE draft_id=?","DELETE FROM draft_images WHERE draft_id=?"}) {
        Statement remove(db_,sql); remove.bind(1,d.id); remove.step();
    }
    for (const auto& run:d.runs) {
        Statement add(db_,"INSERT INTO draft_runs VALUES(?,?,?,?,?,?)"); add.bind(1,d.id); add.bind(2,static_cast<Id>(run.offset));
        add.bind(3,run.bold); add.bind(4,run.italic); add.bind(5,run.underline); add.bind(6,run.link); add.step();
    }
    for (std::size_t i=0;i<d.references.size();++i) {
        Statement add(db_,"INSERT INTO draft_refs VALUES(?,?,?)"); add.bind(1,d.id); add.bind(2,static_cast<Id>(i)); add.bind(3,d.references[i]); add.step();
    }
    for (const auto& a:d.attachments) {
        Statement add(db_,"INSERT INTO draft_attachments VALUES(NULL,?,?,?,?,?,?)"); add.bind(1,d.id); add.bind(2,a.name);
        add.bind(3,a.mediaType); add.bind(4,a.contentId); add.blob(5,a.data); add.bind(6,a.inlineImage); add.step();
    }
    for(const auto& image:d.images) {
        Statement add(db_,"INSERT INTO draft_images VALUES(?,?,?,?)"); add.bind(1,d.id); add.bind(2,static_cast<Id>(image.offset));
        add.bind(3,static_cast<Id>(image.length)); add.bind(4,image.contentId); add.step();
    }
    value.revision=d.revision; value.messageId=d.messageId; return d.id;
}
Draft Store::readDraft(Id id)
{
    Statement s(db_,"SELECT * FROM drafts WHERE id=?"); s.bind(1,id); require(s.step(),"This draft no longer exists.");
    Draft d; d.id=id; d.account=s.number(1); d.messageId=s.text(2); d.to=s.text(3); d.cc=s.text(4); d.bcc=s.text(5);
    d.subject=s.text(6); d.text=s.text(7); d.providerThread=s.text(8); d.modified=s.number(9); d.revision=s.number(10);
    Statement runs(db_,"SELECT offset,bold,italic,underline,link FROM draft_runs WHERE draft_id=? ORDER BY offset"); runs.bind(1,id);
    while (runs.step()) d.runs.push_back({static_cast<std::size_t>(runs.number(0)),runs.number(1)!=0,runs.number(2)!=0,runs.number(3)!=0,runs.text(4)});
    Statement refs(db_,"SELECT reference FROM draft_refs WHERE draft_id=? ORDER BY position"); refs.bind(1,id);
    while (refs.step()) d.references.push_back(refs.text(0));
    Statement attachments(db_,"SELECT name,media_type,content_id,data,inline_image FROM draft_attachments WHERE draft_id=? ORDER BY id"); attachments.bind(1,id);
    while (attachments.step()) d.attachments.push_back({attachments.text(0),attachments.text(1),attachments.text(2),attachments.text(3),attachments.number(4)!=0});
    Statement images(db_,"SELECT offset,length,content_id FROM draft_images WHERE draft_id=? ORDER BY offset"); images.bind(1,id);
    while(images.step()) d.images.push_back({static_cast<std::size_t>(images.number(0)),static_cast<std::size_t>(images.number(1)),images.text(2)});
    return d;
}
Draft Store::draft(Id id) { std::lock_guard<std::mutex> guard(mutex_); return readDraft(id); }
Draft Store::openDraft(Id id,const std::string& editor)
{
    require(!editor.empty() && editor.size()<=256,"Invalid draft editor.");
    std::lock_guard<std::mutex> guard(mutex_); Transaction transaction(db_);
    require(!protectedDraft(db_,id),"This draft is already open or queued for sending.");
    auto result=readDraft(id);
    Statement s(db_,"INSERT INTO draft_editors VALUES(?,?)"); s.bind(1,id); s.bind(2,editor); s.step();
    transaction.commit(); return result;
}
void Store::closeDraft(Id id,const std::string& editor)
{
    std::lock_guard<std::mutex> guard(mutex_);
    Statement s(db_,"DELETE FROM draft_editors WHERE draft_id=? AND owner=?"); s.bind(1,id); s.bind(2,editor); s.step();
}
std::vector<Draft> Store::drafts(Id account)
{
    std::lock_guard<std::mutex> guard(mutex_);
    Statement s(db_,"SELECT id FROM drafts WHERE (?=0 OR account_id=?) AND NOT EXISTS(SELECT 1 FROM outbox WHERE draft_id=drafts.id) ORDER BY modified DESC,id DESC");
    s.bind(1,account); s.bind(2,account); std::vector<Draft> out;
    while (s.step()) out.push_back(readDraft(s.number(0)));
    return out;
}
void Store::removeDraft(Id id,Id expectedRevision)
{
    std::lock_guard<std::mutex> guard(mutex_); Transaction transaction(db_);
    if (expectedRevision) require(readDraft(id).revision==expectedRevision,"This draft changed. Review the current version before discarding it.");
    Statement queued(db_,"SELECT 1 FROM outbox WHERE draft_id=?"); queued.bind(1,id);
    require(!queued.step(),"This draft is queued for delivery.");
    detachDraft(db_,id);
    Statement remove(db_,"DELETE FROM drafts WHERE id=? AND NOT EXISTS(SELECT 1 FROM outbox WHERE draft_id=drafts.id)"); remove.bind(1,id); remove.step();
    require(sqlite3_changes(db_)==1,"This draft is queued for delivery or no longer exists.");
    transaction.commit();
}
Id Store::enqueueDraft(Id id,Timestamp now,Id expectedRevision)
{
    std::lock_guard<std::mutex> guard(mutex_); Transaction transaction(db_); auto d=readDraft(id);
    require(!expectedRevision || d.revision==expectedRevision,"This draft changed. Review the current version before sending it.");
    Statement conflict(db_,"SELECT 1 FROM gmail_drafts WHERE draft_id=? AND conflict=1"); conflict.bind(1,id);
    require(!conflict.step(),"This draft changed on Gmail. Close the composer and review its versions before sending.");
    Statement account(db_,"SELECT email,provider FROM accounts WHERE id=?"); account.bind(1,d.account); require(account.step(),"The sending account no longer exists.");
    Account a; a.id=d.account; a.email=account.text(0); a.provider=static_cast<Provider>(account.number(1));
    auto wire=composeMessage(d,a,now);
    Statement add(db_,"INSERT INTO outbox VALUES(NULL,?,?,'queued','',?,?,?)");
    add.bind(1,id); add.bind(2,d.account); add.bind(3,wire.from); add.blob(4,wire.raw); add.bind(5,now); add.step(); auto result=sqlite3_last_insert_rowid(db_);
    for (std::size_t i=0;i<wire.recipients.size();++i) {
        Statement r(db_,"INSERT INTO outbox_recipients VALUES(?,?,?)"); r.bind(1,result); r.bind(2,static_cast<Id>(i)); r.bind(3,wire.recipients[i]); r.step();
    }
    transaction.commit(); return result;
}

void Store::prepareGmailDrafts(Id account)
{
    std::lock_guard<std::mutex> guard(mutex_); Transaction transaction(db_); gmailAccount(db_,account);
    Statement s(db_,R"SQL(INSERT INTO gmail_drafts(draft_id,account_id)
 SELECT id,account_id FROM drafts WHERE account_id=?
 AND NOT EXISTS(SELECT 1 FROM outbox WHERE draft_id=drafts.id)
 AND NOT EXISTS(SELECT 1 FROM gmail_drafts WHERE draft_id=drafts.id))SQL");
    s.bind(1,account); s.step(); transaction.commit();
}

std::vector<GmailDraftState> Store::gmailDraftStates(Id account,bool includeRaw)
{
    std::lock_guard<std::mutex> guard(mutex_);
    Statement s(db_,"SELECT id FROM gmail_drafts WHERE (?1=0 OR account_id=?1) ORDER BY id"); s.bind(1,account);
    std::vector<GmailDraftState> result;
    while (s.step()) result.push_back(readState(db_,s.number(0),includeRaw));
    return result;
}
GmailDraftState Store::gmailDraftState(Id id,bool includeRaw)
{ std::lock_guard<std::mutex> guard(mutex_); return readState(db_,id,includeRaw); }

void Store::recordGmailDraftMatches(const GmailDraftState& claim,const std::vector<GmailDraft>& matches)
{
    require(matches.size()<=100,"Too many Gmail copies to review.");
    std::size_t bytes=0;
    for (const auto& match:matches) { validateRemote(match); bytes+=match.message.raw.size(); }
    require(bytes<=64*1024*1024,"These Gmail copies exceed the review size limit.");
    std::lock_guard<std::mutex> guard(mutex_); Transaction transaction(db_); auto state=claimed(db_,claim);
    require(state.attempt=="create" && state.remoteId.empty(),"This save is not awaiting a Gmail copy.");
    auto messageId=parseDraftMessage(state.attemptRaw,state.account).messageId;
    clearMatches(db_,state.id);
    for (const auto& match:matches) {
        auto draft=parseDraftMessage(match.message.raw,state.account,match.message.thread);
        require(draft.messageId==messageId,"Gmail returned a copy with a different Message-ID. Check its saved drafts.");
        Statement s(db_,"INSERT INTO gmail_draft_matches VALUES(?,?,?,?,?,?)");
        s.bind(1,state.id); s.bind(2,match.id); s.bind(3,match.message.id); s.bind(4,match.message.thread);
        s.bind(5,draft.subject); s.blob(6,match.message.raw); s.step();
    }
    transaction.commit();
}
std::vector<GmailDraftMatch> Store::gmailDraftMatches(Id id)
{
    std::lock_guard<std::mutex> guard(mutex_);
    Statement s(db_,"SELECT remote_id,subject FROM gmail_draft_matches WHERE journal_id=? ORDER BY remote_id"); s.bind(1,id);
    std::vector<GmailDraftMatch> result; while (s.step()) result.push_back({s.text(0),s.text(1)}); return result;
}
GmailDraft Store::gmailDraftMatch(Id id,const std::string& remoteId)
{ std::lock_guard<std::mutex> guard(mutex_); return readMatch(db_,id,remoteId); }

void Store::useGmailDraftMatch(const GmailDraftState& candidate,const std::string& remoteId)
{
    std::lock_guard<std::mutex> guard(mutex_); Transaction transaction(db_); auto state=reviewedCreate(db_,candidate);
    auto match=readMatch(db_,state.id,remoteId);
    Statement existing(db_,"SELECT 1 FROM gmail_drafts WHERE account_id=? AND remote_id=? AND id!=?");
    existing.bind(1,state.account); existing.bind(2,remoteId); existing.bind(3,state.id);
    require(!existing.step(),"This Gmail copy is already linked to another draft. Choose a different copy or retry the local save.");
    auto server=parseDraftMessage(match.message.raw,state.account,match.message.thread);
    auto attempted=parseDraftMessage(state.attemptRaw,state.account,match.message.thread);
    Id saved=state.attemptRevision,localId=state.draft;
    // Header rewrites need not create a duplicate. Selecting this copy accepts
    // its original MIME while newer local revisions still need their own save.
    if (!localId) { localId=saveDraftLocked(server); saved=server.revision; }
    else if (!sameContent(server,attempted)) {
        auto local=readDraft(localId); local.id=0; local.revision=0; local.messageId=newMessageId(); saveDraftLocked(local);
        server.id=localId; server.revision=readDraft(localId).revision; saveDraftLocked(server); saved=server.revision;
    }
    Statement s(db_,R"SQL(UPDATE gmail_drafts SET draft_id=?,remote_id=?,message_id=?,thread_id=?,raw=?,saved_revision=?,
 attempt='',attempt_revision=0,attempt_raw=X'',attempt_thread='',active=0,revision=revision+1,error='',conflict=0,missing=0,
 observed_id='',observed_thread='',observed_raw=X'' WHERE id=?)SQL");
    s.bind(1,localId); s.bind(2,match.id); s.bind(3,match.message.id); s.bind(4,match.message.thread);
    s.blob(5,match.message.raw); s.bind(6,saved); s.bind(7,state.id); s.step();
    clearMatches(db_,state.id); removeGmailMessageLocked(state.account,match.message.id); transaction.commit();
}
void Store::retryGmailDraftCreate(const GmailDraftState& candidate)
{
    std::lock_guard<std::mutex> guard(mutex_); Transaction transaction(db_); auto state=reviewedCreate(db_,candidate);
    require(state.draft,"There is no local draft to save again.");
    auto local=readDraft(state.draft); local.messageId=newMessageId(); saveDraftLocked(local);
    Statement s(db_,R"SQL(UPDATE gmail_drafts SET attempt='',attempt_revision=0,attempt_raw=X'',attempt_thread='',
 saved_revision=0,error='',revision=revision+1,conflict=0,missing=0,observed_id='',observed_thread='',observed_raw=X'' WHERE id=?)SQL");
    s.bind(1,state.id); s.step(); clearMatches(db_,state.id); transaction.commit();
}
void Store::forgetDiscardedGmailDraft(const GmailDraftState& candidate)
{
    std::lock_guard<std::mutex> guard(mutex_); Transaction transaction(db_); auto state=reviewedCreate(db_,candidate);
    require(!state.draft,"Keep or discard the local draft before stopping this check.");
    Statement s(db_,"DELETE FROM gmail_drafts WHERE id=?"); s.bind(1,state.id); s.step(); transaction.commit();
}

Id Store::importGmailDraft(Id account,const GmailDraft& remote)
{
    validateRemote(remote); auto draft=parseDraftMessage(remote.message.raw,account,remote.message.thread);
    if (remote.message.received) draft.modified=remote.message.received;
    std::lock_guard<std::mutex> guard(mutex_); Transaction transaction(db_); gmailAccount(db_,account);
    Statement exists(db_,"SELECT COALESCE(draft_id,0) FROM gmail_drafts WHERE account_id=? AND remote_id=?");
    exists.bind(1,account); exists.bind(2,remote.id); if (exists.step()) return exists.number(0);
    // A lost create receipt may already refer to this remote resource. Leave
    // it for reconciliation rather than importing a second editable copy.
    Statement pending(db_,"SELECT attempt_raw FROM gmail_drafts WHERE account_id=? AND attempt='create'"); pending.bind(1,account);
    while (pending.step()) if (parseDraftMessage(pending.text(0),account).messageId==draft.messageId) return 0;
    draft.id=saveDraftLocked(draft);
    Statement s(db_,"INSERT INTO gmail_drafts(draft_id,account_id,remote_id,message_id,thread_id,raw,saved_revision) VALUES(?,?,?,?,?,?,?)");
    s.bind(1,draft.id); s.bind(2,account); s.bind(3,remote.id); s.bind(4,remote.message.id); s.bind(5,remote.message.thread);
    s.blob(6,remote.message.raw); s.bind(7,draft.revision); s.step();
    removeGmailMessageLocked(account,remote.message.id);
    transaction.commit(); return draft.id;
}

bool Store::observeGmailDraft(const GmailDraftState& claim,const GmailDraft& remote)
{
    validateRemote(remote);
    std::lock_guard<std::mutex> guard(mutex_); Transaction transaction(db_); auto state=claimed(db_,claim);
    require(state.remoteId==remote.id,"This is a different server draft.");
    // Only an acknowledged update or an exact MIME match can settle a pending
    // write. A different response remains available for explicit review.
    bool changed=remote.message.id!=state.messageId && remote.message.raw!=state.raw;
    if (!state.attempt.empty() || (changed && (!state.draft || protectedDraft(db_,state.draft)
        || readDraft(state.draft).revision!=state.savedRevision))) {
        Statement s(db_,R"SQL(UPDATE gmail_drafts SET conflict=1,missing=0,observed_id=?,observed_thread=?,observed_raw=?,
 error='This draft changed on Gmail. Close its composer and review both versions.' WHERE id=?)SQL");
        s.bind(1,remote.message.id); s.bind(2,remote.message.thread); s.blob(3,remote.message.raw); s.bind(4,state.id); s.step();
        transaction.commit(); return false;
    }
    Id saved=state.savedRevision;
    if (changed && state.draft) {
        auto replacement=parseDraftMessage(remote.message.raw,state.account,remote.message.thread);
        replacement.id=state.draft; replacement.revision=readDraft(state.draft).revision;
        if (remote.message.received) replacement.modified=remote.message.received;
        saveDraftLocked(replacement); saved=replacement.revision;
    }
    Statement s(db_,R"SQL(UPDATE gmail_drafts SET message_id=?,thread_id=?,raw=?,saved_revision=?,conflict=0,missing=0,
 observed_id='',observed_thread='',observed_raw=X'',error='' WHERE id=?)SQL");
    s.bind(1,remote.message.id); s.bind(2,remote.message.thread); s.blob(3,remote.message.raw); s.bind(4,saved); s.bind(5,state.id); s.step();
    removeGmailMessageLocked(state.account,state.messageId);
    removeGmailMessageLocked(state.account,remote.message.id);
    transaction.commit(); return true;
}

void Store::missingGmailDraft(const GmailDraftState& claim)
{
    std::lock_guard<std::mutex> guard(mutex_); Transaction transaction(db_); auto state=claimed(db_,claim);
    require(!state.remoteId.empty(),"A missing search result cannot prove an uncertain create was not saved.");
    if (!state.draft || (state.attempt.empty() && !protectedDraft(db_,state.draft) && readDraft(state.draft).revision==state.savedRevision)) {
        Statement remove(db_,"DELETE FROM gmail_drafts WHERE id=?"); remove.bind(1,state.id); remove.step();
        if (state.draft) { Statement d(db_,"DELETE FROM drafts WHERE id=?"); d.bind(1,state.draft); d.step(); }
    } else {
        Statement s(db_,"UPDATE gmail_drafts SET conflict=1,missing=1,active=0,revision=revision+1,error='This draft was removed from Gmail. Your local version is retained for review.' WHERE id=?");
        s.bind(1,state.id); s.step();
    }
    removeGmailMessageLocked(state.account,state.messageId); transaction.commit();
}

void Store::keepBothGmailDrafts(const GmailDraftState& candidate)
{
    std::lock_guard<std::mutex> guard(mutex_); Transaction transaction(db_); auto state=readState(db_,candidate.id);
    require(!state.active && state.revision==candidate.revision && state.account==candidate.account && state.conflict,
        "This draft's versions changed or are being checked. Review their current state.");
    require(!state.draft || !protectedDraft(db_,state.draft),"Close the composer and return queued mail to Drafts before reviewing its versions.");
    require(state.attempt!="create","Check the uncertain server save before making another copy.");
    if (!state.draft) {
        require(!state.observedRaw.empty() && !state.missing,"Check Gmail to retrieve the remaining draft first.");
        auto server=parseDraftMessage(state.observedRaw,state.account,state.observedThread); server.id=saveDraftLocked(server);
        Statement s(db_,R"SQL(UPDATE gmail_drafts SET draft_id=?,saved_revision=?,message_id=observed_id,thread_id=observed_thread,
 raw=observed_raw,observed_id='',observed_thread='',observed_raw=X'',attempt='',attempt_revision=0,attempt_raw=X'',attempt_thread='',
 conflict=0,missing=0,error='',revision=revision+1 WHERE id=?)SQL");
        s.bind(1,server.id); s.bind(2,server.revision); s.bind(3,state.id); s.step(); transaction.commit(); return;
    }
    auto local=readDraft(state.draft);
    if (state.missing) {
        // Explicitly retaining the local version after an authoritative 404
        // gives any new server copy its own Message-ID for future recovery.
        local.messageId=newMessageId(); local.providerThread.clear(); saveDraftLocked(local);
        Statement remove(db_,"DELETE FROM gmail_drafts WHERE id=?"); remove.bind(1,state.id); remove.step();
    } else {
        require(!state.observedRaw.empty(),"Check Gmail to retrieve the other version first.");
        auto server=parseDraftMessage(state.observedRaw,state.account,state.observedThread);
        local.id=0; local.revision=0; local.messageId=newMessageId(); saveDraftLocked(local);
        server.id=state.draft; server.revision=readDraft(state.draft).revision; saveDraftLocked(server);
        Statement s(db_,R"SQL(UPDATE gmail_drafts SET message_id=observed_id,thread_id=observed_thread,raw=observed_raw,
 saved_revision=?,conflict=0,missing=0,observed_id='',observed_thread='',observed_raw=X'',attempt='',attempt_revision=0,
 attempt_raw=X'',attempt_thread='',error='',revision=revision+1 WHERE id=?)SQL");
        s.bind(1,server.revision); s.bind(2,state.id); s.step();
    }
    transaction.commit();
}

std::string Store::draftStatus(Id id)
{
    std::lock_guard<std::mutex> guard(mutex_);
    Statement s(db_,R"SQL(SELECT a.provider,d.revision,g.saved_revision,g.active,g.attempt,g.conflict,g.missing,g.error
 FROM drafts d JOIN accounts a ON a.id=d.account_id LEFT JOIN gmail_drafts g ON g.draft_id=d.id WHERE d.id=?)SQL");
    s.bind(1,id); if (!s.step()) return "Draft no longer available";
    if (s.number(0)!=static_cast<Id>(Provider::Gmail)) return "Draft saved on this computer";
    if (s.number(3)) return "Checking draft on Gmail…";
    if (s.number(5)) return s.number(6)?"Draft removed on Gmail — review saved versions":"Draft changed on Gmail — review saved versions";
    if (!s.text(7).empty()) return "Saved locally — review Gmail save";
    if (!s.text(4).empty()) return "Saved locally — Gmail save needs checking";
    return s.number(1)==s.number(2)?"Draft saved on Gmail":"Draft saved on this computer — waiting for Gmail";
}

GmailDraftState Store::claimGmailDraft(const GmailDraftState& candidate)
{
    std::lock_guard<std::mutex> guard(mutex_); Transaction transaction(db_);
    // A queued delivery can wait for an unresolved save to be reconciled, but
    // cannot permit a new upload. Sending and uncertain delivery exclude all
    // draft work until the outbox has resolved their disposition.
    Statement s(db_,R"SQL(UPDATE gmail_drafts SET active=1,revision=revision+1
 WHERE id=? AND account_id=? AND revision=? AND active=0
 AND NOT EXISTS(SELECT 1 FROM outbox o WHERE o.draft_id=gmail_drafts.draft_id
   AND (o.state IN('sending','uncertain') OR gmail_drafts.attempt='')))SQL");
    s.bind(1,candidate.id); s.bind(2,candidate.account); s.bind(3,candidate.revision); s.step();
    if (sqlite3_changes(db_)!=1) return {};
    auto result=readState(db_,candidate.id); transaction.commit(); return result;
}

GmailDraftState Store::beginGmailDraftWrite(const GmailDraftState& claim)
{
    std::lock_guard<std::mutex> guard(mutex_); Transaction transaction(db_); auto state=claimed(db_,claim);
    require(state.draft && state.attempt.empty(),"Check this draft's previous save before starting another upload.");
    require(!state.conflict,"Review this draft's conflicting versions before uploading.");
    Statement queued(db_,"SELECT 1 FROM outbox WHERE draft_id=?"); queued.bind(1,state.draft);
    require(!queued.step(),"This draft is queued for sending. Finish its current check without uploading again.");
    auto draft=readDraft(state.draft);
    require(draft.account==state.account,"This draft's sending account changed.");
    require(draft.revision!=state.savedRevision,"This draft is already saved on Gmail.");
    gmailAccount(db_,state.account);
    Statement account(db_,"SELECT email FROM accounts WHERE id=?"); account.bind(1,state.account); account.step();
    Account a; a.id=state.account; a.provider=Provider::Gmail; a.email=account.text(0);
    const auto raw=composeDraftMessage(draft,a,draft.modified?draft.modified:std::time(nullptr));
    Statement s(db_,"UPDATE gmail_drafts SET attempt=?,attempt_revision=?,attempt_raw=?,attempt_thread=?,error='' WHERE id=?");
    s.bind(1,state.remoteId.empty()?std::string("create"):std::string("update")); s.bind(2,draft.revision); s.blob(3,raw);
    s.bind(4,draft.providerThread); s.bind(5,state.id); s.step();
    auto result=readState(db_,state.id); transaction.commit(); return result;
}

GmailDraftState Store::beginGmailDraftDelete(const GmailDraftState& claim)
{
    std::lock_guard<std::mutex> guard(mutex_); Transaction transaction(db_); auto state=claimed(db_,claim);
    require(!state.draft && !state.remoteId.empty() && state.attempt.empty() && !state.conflict,"Resolve this discarded draft's previous save before deleting its server copy.");
    Statement s(db_,"UPDATE gmail_drafts SET attempt='delete',error='' WHERE id=?"); s.bind(1,state.id); s.step();
    auto result=readState(db_,state.id); transaction.commit(); return result;
}

void Store::finishGmailDraftWrite(const GmailDraftState& claim,const GmailDraft& remote)
{
    identifier(remote.id); identifier(remote.message.id); identifier(remote.message.thread);
    std::lock_guard<std::mutex> guard(mutex_); Transaction transaction(db_); auto state=claimed(db_,claim);
    require((state.attempt=="create" || state.attempt=="update") && !state.attemptRaw.empty(),"This draft has no pending upload to confirm.");
    require(state.remoteId.empty() || state.remoteId==remote.id,"Gmail returned a different draft. Check the server before saving again.");
    // This acknowledges only the snapshot sent, never a newer local revision.
    // A detached row remains available for deletion even after local discard.
    Statement s(db_,R"SQL(UPDATE gmail_drafts SET remote_id=?,message_id=?,thread_id=?,raw=attempt_raw,
 saved_revision=attempt_revision,attempt='',attempt_revision=0,attempt_raw=X'',attempt_thread='',
 active=0,revision=revision+1,error='',conflict=0,missing=0,observed_id='',observed_thread='',observed_raw=X'' WHERE id=?)SQL");
    s.bind(1,remote.id); s.bind(2,remote.message.id); s.bind(3,remote.message.thread); s.bind(4,state.id); s.step();
    clearMatches(db_,state.id);
    transaction.commit();
}

void Store::finishGmailDraftDelete(const GmailDraftState& claim)
{
    std::lock_guard<std::mutex> guard(mutex_); Transaction transaction(db_); auto state=claimed(db_,claim);
    require(!state.draft && !state.remoteId.empty() && state.attempt!="create", "This draft cannot be removed before its server identity is resolved.");
    Statement s(db_,"DELETE FROM gmail_drafts WHERE id=?"); s.bind(1,state.id); s.step(); transaction.commit();
}

void Store::releaseGmailDraft(const GmailDraftState& claim,const std::string& error,bool clearAttempt)
{
    require(error.size()<=8192,"The draft error is too long.");
    std::lock_guard<std::mutex> guard(mutex_); Transaction transaction(db_); auto state=claimed(db_,claim);
    Statement s(db_,R"SQL(UPDATE gmail_drafts SET active=0,revision=revision+1,error=?1,
 attempt=CASE WHEN ?2 THEN '' ELSE attempt END,attempt_revision=CASE WHEN ?2 THEN 0 ELSE attempt_revision END,
 attempt_raw=CASE WHEN ?2 THEN X'' ELSE attempt_raw END,attempt_thread=CASE WHEN ?2 THEN '' ELSE attempt_thread END WHERE id=?3)SQL");
    s.bind(1,error); s.bind(2,clearAttempt); s.bind(3,state.id); s.step();
    if (clearAttempt) clearMatches(db_,state.id);
    exec(db_,"DELETE FROM gmail_drafts WHERE draft_id IS NULL AND remote_id='' AND attempt='' AND active=0");
    transaction.commit();
}

bool Store::readyOutgoing()
{
    std::lock_guard<std::mutex> guard(mutex_);
    Statement s(db_,"SELECT 1 FROM outbox WHERE state='queued' AND NOT EXISTS(SELECT 1 FROM gmail_drafts g WHERE g.draft_id=outbox.draft_id AND (g.active=1 OR g.attempt!='' OR g.conflict=1)) LIMIT 1");
    return s.step();
}
}
