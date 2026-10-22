#include "mail/Sync.h"
#include <algorithm>
#include <cctype>
#include <set>

namespace mail {
namespace {
std::string lower(std::string value)
{
    for (auto& c : value) if (static_cast<unsigned char>(c) < 128) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}
void checkCancelled(const std::atomic<bool>& cancelled)
{ if (cancelled) throw NetworkError(NetworkFailure::Cancelled, "Checking Gmail was cancelled."); }
void nextPage(std::set<std::string>& visited, const std::string& token)
{
    if (!token.empty() && (!visited.insert(token).second || visited.size() > 10000))
        throw NetworkError(NetworkFailure::Protocol, "Gmail returned repeated or excessive pages. The saved sync cursor was not advanced.");
}
}
void verifyGmailIdentity(const Account& account, const GmailProfile& profile)
{
    if (account.provider != Provider::Gmail || lower(account.email) != lower(profile.email))
        throw NetworkError(NetworkFailure::Authentication, "This Google sign-in belongs to a different account. Sign in with " + account.email + ".");
}
SyncReport syncGmailAccount(Store& store, const Account& account, GmailApi& api, const std::atomic<bool>& cancelled,
    const SyncProgressFunction& progress)
{
    SyncReport report; checkCancelled(cancelled);
    if (progress) progress({"Connecting to " + account.name + "…"});
    auto profile = api.profile(); verifyGmailIdentity(account, profile);
    auto labels = api.labels(); store.saveGmailLabels(account.id, labels);
    auto labelChanges=store.labelChanges(account.id);
    for (const auto& change:labelChanges) {
        checkCancelled(cancelled);
        try {
            if (change.kind=="rename") api.renameLabel(change.label,change.name);
            else if (change.kind=="delete") {
                try { api.deleteLabel(change.label); }
                catch (const HttpError& error) { if (error.status!=404) throw; }
            } else throw NetworkError(NetworkFailure::Protocol,"This Gmail label change is unsupported.");
            store.finishLabelChange(change.id); ++report.applied;
        } catch (const std::exception& error) {
            store.finishLabelChange(change.id,error.what());
            // Stop this account's check: membership changes must not create a
            // second label while its rename/removal has not been confirmed.
            throw;
        }
    }
    if (!labelChanges.empty()) { labels=api.labels(); store.saveGmailLabels(account.id,labels); }
    auto apply = [&] {
        auto pending = store.pendingChanges(account.id);
        std::stable_sort(pending.begin(), pending.end(), [](const PendingChange& a, const PendingChange& b) { return (a.kind == "move") < (b.kind == "move"); });
        for (const auto& change : pending) {
            checkCancelled(cancelled);
            const auto& id = change.location.locator;
            try {
                if (change.location.folder != "gmail") throw NetworkError(NetworkFailure::Protocol, "The cached Gmail identity is invalid.");
                if (change.kind == "read") api.modify(id, change.value == "1" ? std::vector<std::string>{} : std::vector<std::string>{"UNREAD"},
                    change.value == "1" ? std::vector<std::string>{"UNREAD"} : std::vector<std::string>{});
                else if (change.kind == "star") api.modify(id, change.value == "1" ? std::vector<std::string>{"STARRED"} : std::vector<std::string>{},
                    change.value == "1" ? std::vector<std::string>{} : std::vector<std::string>{"STARRED"});
                else if (change.kind.compare(0, 4, "tag:") == 0) {
                    auto name = change.kind.substr(4);
                    auto labelEdits=store.labelChanges(account.id);
                    if (std::any_of(labelEdits.begin(),labelEdits.end(),[&](const LabelChange& edit){return edit.name==name;})) continue;
                    auto find = [&] { return std::find_if(labels.begin(), labels.end(), [&](const GmailLabel& label) { return !label.system && label.name == name; }); };
                    auto label = find();
                    if (label == labels.end() && change.value == "1") {
                        try { api.createLabel(name); }
                        catch (const HttpError& error) { if (error.status != 409) throw; } // Another client may have created it.
                        labels = api.labels(); store.saveGmailLabels(account.id, labels); label = find();
                        if (label == labels.end()) throw NetworkError(NetworkFailure::Protocol, "The new Gmail label was not returned by the server.");
                    }
                    if (label != labels.end()) api.modify(id, change.value == "1" ? std::vector<std::string>{label->id} : std::vector<std::string>{},
                        change.value == "1" ? std::vector<std::string>{} : std::vector<std::string>{label->id});
                } else if (change.kind == "move") {
                    if (change.value == "trash") api.trash(id, true);
                    else {
                        std::string target;
                        if (change.value == "inbox") target = "INBOX";
                        else if (change.value == "junk") target = "SPAM";
                        else if (change.value.compare(0, 7, "remote:") == 0) {
                            auto label = std::find_if(labels.begin(), labels.end(), [&](const GmailLabel& l) { return !l.system && l.id == change.value.substr(7); });
                            if (label != labels.end()) target = label->id;
                        }
                        if (target.empty() && change.value != "archive")
                            throw NetworkError(NetworkFailure::Unsupported, "Gmail cannot move a message to “" + change.value + "”. The change remains pending.");
                        auto remote = api.message(id, false);
                        if (std::find(remote.labels.begin(), remote.labels.end(), "TRASH") != remote.labels.end()) api.trash(id, false);
                        std::vector<std::string> remove{"INBOX", "SPAM"};
                        remove.erase(std::remove(remove.begin(), remove.end(), target), remove.end());
                        api.modify(id, target.empty() ? std::vector<std::string>{} : std::vector<std::string>{target}, remove);
                    }
                } else throw NetworkError(NetworkFailure::Unsupported, "An unsupported Gmail change remains pending.");
                // Persist the latest provider state and acknowledge this exact
                // journal ID atomically. Newer UI edits are reapplied afterward.
                store.ingestGmail(account.id, api.message(id, false), change.id); ++report.applied;
            } catch (const HttpError& error) {
                if (error.status == 404) {
                    // A mutation can also report 404 for a removed label. Only
                    // a message GET can establish that the message itself is gone.
                    try {
                        store.ingestGmail(account.id, api.message(id, false));
                        report.errors.push_back("A Gmail change could not be applied and remains pending.");
                    } catch (const HttpError& confirm) {
                        if (confirm.status != 404) throw;
                        store.removeGmailMessage(account.id, id); ++report.applied;
                    }
                }
                else if (error.status == 401 || error.status == 429 || error.status >= 500) throw;
                else report.errors.push_back(error.what());
            } catch (const NetworkError& error) {
                if (error.reason == NetworkFailure::Cancelled || error.reason == NetworkFailure::Connection
                    || error.reason == NetworkFailure::Authentication || error.reason == NetworkFailure::Certificate) throw;
                report.errors.push_back(error.what());
            }
        }
    };
    apply();
    auto cursor = store.gmailHistory(account.id); bool full = cursor.empty();
    std::set<std::string> changed; std::string checkpoint;
    if (!full) {
        std::string page; std::set<std::string> pages;
        try {
            do {
                checkCancelled(cancelled); auto history = api.history(cursor, page);
                changed.insert(history.changed.begin(), history.changed.end());
                if (changed.size() > 5000000) throw NetworkError(NetworkFailure::Protocol, "This Gmail history exceeds Mail's sync limit.");
                checkpoint = history.history; page = history.next; nextPage(pages, page);
            } while (!page.empty());
        } catch (const HttpError& error) { if (error.status != 404) throw; full = true; changed.clear(); }
    }
    std::vector<std::string> deleted;
    std::size_t done = 0, total = full ? 0 : changed.size();
    auto receive = [&](const std::string& id) {
        checkCancelled(cancelled);
        if (progress) progress({(full ? "Downloading " : "Updating ") + account.name + " — Gmail", 0, 0, done++, total});
        bool known = store.findRemote(account.id, "gmail:" + id) != 0;
        try {
            auto remote=api.message(id,!known);
            // Editable drafts are synchronized through the stable draft ID.
            // Old message-cache entries are hidden to avoid read-only twins.
            if (std::find(remote.labels.begin(),remote.labels.end(),"DRAFT")!=remote.labels.end()) store.removeGmailMessage(account.id,id);
            else { store.ingestGmail(account.id,remote); if (!known) ++report.downloaded; }
        }
        catch (const HttpError& error) { if (error.status != 404) throw; deleted.push_back(id); }
    };
    if (full) {
        // The initial profile precedes all list/fetch requests. Changes made
        // during this full check remain discoverable by the following history check.
        checkpoint = profile.history;
        auto baseline = store.cachedGmailIds(account.id); std::set<std::string> seen, pages; std::string page;
        do {
            checkCancelled(cancelled); auto messages = api.messages(page);
            for (const auto& id : messages.ids) if (seen.insert(id).second) receive(id);
            if (seen.size() > 5000000) throw NetworkError(NetworkFailure::Protocol, "This Gmail mailbox exceeds Mail's sync limit.");
            page = messages.next; nextPage(pages, page);
        } while (!page.empty());
        // List pagination is not a transaction. Confirm missing baseline IDs
        // individually so concurrent changes cannot silently hide cached mail.
        for (const auto& id : baseline) if (!seen.count(id)) receive(id);
    } else for (const auto& id : changed) receive(id);
    checkCancelled(cancelled); store.finishGmailSync(account.id, checkpoint, deleted);
    apply(); // Includes actions created by automatic filters on newly received mail.
    auto drafts=syncGmailDrafts(store,account,api,cancelled);
    report.downloaded+=drafts.downloaded; report.applied+=drafts.applied;
    report.errors.insert(report.errors.end(),drafts.errors.begin(),drafts.errors.end());
    return report;
}
}
