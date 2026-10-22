#include "mail/Sync.h"
#include <algorithm>
#include <set>

namespace mail {
namespace {
void cancelled(const std::atomic<bool>& value)
{ if (value) throw NetworkError(NetworkFailure::Cancelled,"Checking Gmail drafts was cancelled."); }
void pageToken(std::set<std::string>& seen,const std::string& token)
{
    if (!token.empty() && (!seen.insert(token).second || seen.size()>1000))
        throw NetworkError(NetworkFailure::Protocol,"Gmail returned repeated or excessive draft pages.");
}
bool stop(const NetworkError& e)
{
    const auto* http=dynamic_cast<const HttpError*>(&e);
    return e.reason==NetworkFailure::Cancelled || e.reason==NetworkFailure::Authentication || e.reason==NetworkFailure::Certificate
        || e.reason==NetworkFailure::Connection || (http && (http->status==401 || http->status==429 || http->status>=500));
}
}
SyncReport syncGmailDrafts(Store& store,const Account& account,GmailApi& api,const std::atomic<bool>& cancel)
{
    cancelled(cancel); verifyGmailIdentity(account,api.profile()); store.prepareGmailDrafts(account.id);
    SyncReport report;
    for (const auto& candidate:store.gmailDraftStates(account.id)) {
        cancelled(cancel); auto claim=store.claimGmailDraft(candidate); if (!claim.id) continue;
        bool held=true,mutation=false,deleting=false;
        auto release=[&](const std::string& error,bool rejected=false) {
            if (held) { store.releaseGmailDraft(claim,error,rejected); held=false; }
        };
        try {
            if (claim.attempt=="create") {
                store.recordGmailDraftMatches(claim,{});
                auto original=parseDraftMessage(claim.attemptRaw,account.id);
                std::string page; std::set<std::string> pages,ids; std::vector<GmailDraft> found;
                std::size_t bytes=0;
                do {
                    cancelled(cancel); auto list=api.drafts(page,"rfc822msgid:"+original.messageId);
                    for (const auto& item:list.drafts) if (ids.insert(item.id).second) {
                        if (ids.size()>100) throw NetworkError(NetworkFailure::Protocol,"Too many matching drafts to confirm this save. Review Gmail before retrying.");
                        cancelled(cancel);
                        try {
                            auto match=api.draft(item.id); bytes+=match.message.raw.size();
                            if (bytes>64*1024*1024) throw NetworkError(NetworkFailure::Protocol,"Matching Gmail drafts exceed the review size limit. Inspect Gmail before retrying.");
                            found.push_back(std::move(match));
                        }
                        catch (const HttpError& e) { if (e.status!=404) throw; }
                    }
                    page=list.next; pageToken(pages,page);
                } while (!page.empty());
                if (found.size()==1 && found[0].message.raw==claim.attemptRaw) {
                    store.finishGmailDraftWrite(claim,found[0]); held=false; ++report.applied;
                } else {
                    store.recordGmailDraftMatches(claim,found);
                    const std::string error="Gmail could not confirm the earlier draft save. Check Gmail before making another copy.";
                    release(error); report.errors.push_back(error);
                }
                continue;
            }
            if (!claim.remoteId.empty()) {
                GmailDraft remote;
                try { remote=api.draft(claim.remoteId); }
                catch (const HttpError& e) {
                    if (e.status!=404) throw;
                    store.missingGmailDraft(claim); held=false; ++report.applied; continue;
                }
                if (claim.attempt=="update" && remote.message.raw==claim.attemptRaw) {
                    store.finishGmailDraftWrite(claim,remote); held=false; ++report.applied; continue;
                }
                if (claim.attempt=="delete" && remote.message.id==claim.messageId) {
                    // The stable draft still exists unchanged. A later check
                    // can retry this idempotent deletion after this GET.
                    release("The discarded draft is still on Gmail. Check again to finish removing it.",true); continue;
                }
                if (!store.observeGmailDraft(claim,remote)) {
                    const auto error=store.gmailDraftState(claim.id,false).error;
                    release(error); report.errors.push_back(error); continue;
                }
            }
            auto current=store.gmailDraftState(claim.id,false);
            if (!current.draft) {
                if (current.remoteId.empty()) { release({}); continue; }
                store.beginGmailDraftDelete(claim); mutation=true; deleting=true; cancelled(cancel);
                try { api.deleteDraft(current.remoteId); }
                catch (const HttpError& e) {
                    if (e.status!=404) throw;
                    // A draft GET, rather than an arbitrary mutation failure,
                    // establishes the identity has gone.
                    try { api.draft(current.remoteId,false); throw std::runtime_error("Gmail could not remove this draft. Check again."); }
                    catch (const HttpError& confirm) { if (confirm.status!=404) throw; }
                }
                store.finishGmailDraftDelete(claim); held=false; ++report.applied; continue;
            }
            auto local=store.draft(current.draft);
            if (local.revision==current.savedRevision) { release({}); continue; }
            auto writing=store.beginGmailDraftWrite(claim); mutation=true;
            cancelled(cancel);
            auto receipt=writing.remoteId.empty()?api.createDraft(writing.attemptRaw,writing.attemptThread)
                :api.updateDraft(writing.remoteId,writing.attemptRaw,writing.attemptThread);
            store.finishGmailDraftWrite(claim,receipt); held=false; ++report.applied;
        } catch (const NetworkError& e) {
            // CopyUnknown is the transport's explicit acceptance ambiguity.
            // Other write failures occurred before bytes were accepted, or
            // carry an authoritative HTTP rejection. A DELETE has no body,
            // so a disconnected deletion must still be checked with GET.
            const auto* http=dynamic_cast<const HttpError*>(&e);
            const bool rejected=mutation && e.reason!=NetworkFailure::CopyUnknown && e.reason!=NetworkFailure::DeliveryUnknown
                && (!deleting || http);
            release(e.what(),rejected); if (stop(e)) throw; report.errors.push_back(e.what());
        } catch (const std::exception& e) { release(e.what()); report.errors.push_back(e.what()); }
    }
    std::set<std::string> known,seen,pages;
    for (const auto& state:store.gmailDraftStates(account.id)) if (!state.remoteId.empty()) known.insert(state.remoteId);
    std::string page;
    do {
        cancelled(cancel); auto list=api.drafts(page);
        for (const auto& item:list.drafts) if (seen.insert(item.id).second && !known.count(item.id)) {
            cancelled(cancel);
            try { if (store.importGmailDraft(account.id,api.draft(item.id))) ++report.downloaded; }
            catch (const HttpError& e) { if (e.status!=404) throw; }
            catch (const NetworkError& e) { if (stop(e)) throw; report.errors.push_back(e.what()); }
            catch (const std::exception& e) { report.errors.push_back(e.what()); }
        }
        if (seen.size()>100000) throw NetworkError(NetworkFailure::Protocol,"This account exceeds Mail's draft synchronization limit.");
        page=list.next; pageToken(pages,page);
    } while (!page.empty());
    return report;
}
}
