#include "mail/Sync.h"
#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

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
// Fetches Gmail message contents on worker threads (the network-bound part of
// a check) while the caller drains completed fetches and ingests them in
// batches. The store stays on the caller's thread. Any failure other than a
// confirmed 404 is fatal: workers stop accepting new work and the caller
// rethrows the original exception once the pool has wound down.
class FetchPool {
public:
    struct FetchResult {
        std::string id;
        GmailMessage message; // Valid unless notFound or error is set.
        bool notFound = false;
        std::exception_ptr error;
    };
    FetchPool(GmailApi& api, const std::atomic<bool>& cancelled) : api_(api)
    {
        (void)cancelled; // Cancellation aborts in-flight requests through the API's progress callback.
        const auto available = std::thread::hardware_concurrency();
        const auto count = std::min<unsigned>(8, std::max<unsigned>(2, available ? available : 4));
        workers_.reserve(count);
        for (unsigned i = 0; i < count; ++i) workers_.emplace_back([this] { run(); });
    }
    FetchPool(const FetchPool&) = delete;
    FetchPool& operator=(const FetchPool&) = delete;
    ~FetchPool()
    {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            stopped_ = true;
            tasks_.clear();
        }
        cv_.notify_all();
        for (auto& worker : workers_) worker.join();
    }

    void submit(std::string id, bool raw)
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (stopped_ || fatal_) return;
        tasks_.push_back({std::move(id), raw});
        cv_.notify_one();
    }
    void finishSubmitting()
    {
        std::lock_guard<std::mutex> guard(mutex_);
        submitting_ = false;
        cv_.notify_all();
    }
    // Completed results, without waiting.
    std::vector<FetchResult> poll(std::size_t limit) { return take(limit, false); }
    // Completed results, waiting when none are available. `finished` reports
    // that no further results can arrive.
    std::vector<FetchResult> wait(std::size_t limit, bool& finished) { return take(limit, true, &finished); }

private:
    struct Task { std::string id; bool raw; };
    std::vector<FetchResult> take(std::size_t limit, bool block, bool* finished = nullptr)
    {
        std::unique_lock<std::mutex> guard(mutex_);
        if (block) {
            cv_.wait(guard, [&] {
                return !results_.empty() || (tasks_.empty() && active_ == 0 && (fatal_ || !submitting_));
            });
        }
        std::vector<FetchResult> out;
        out.reserve(std::min(limit, results_.size()));
        while (!results_.empty() && out.size() < limit) {
            out.push_back(std::move(results_.front()));
            results_.pop_front();
        }
        if (finished)
            *finished = tasks_.empty() && active_ == 0 && results_.empty() && (fatal_ || !submitting_);
        return out;
    }
    void run()
    {
        // Workers live until the destructor stops them: submission phases can
        // resume after a drain (baseline confirmation), and idle workers cost
        // nothing against the network wait they exist to hide.
        for (;;) {
            Task task;
            {
                std::unique_lock<std::mutex> guard(mutex_);
                cv_.wait(guard, [&] { return stopped_ || fatal_ || !tasks_.empty(); });
                if (stopped_ || fatal_ || tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop_front();
                ++active_;
            }
            FetchResult result;
            result.id = task.id;
            try {
                result.message = api_.message(task.id, task.raw);
            } catch (const HttpError& error) {
                if (error.status == 404) result.notFound = true;
                else {
                    result.error = std::current_exception();
                    std::lock_guard<std::mutex> guard(mutex_);
                    fatal_ = true;
                    tasks_.clear(); // Drop pending work so the drain can finish.
                }
            } catch (const std::exception&) {
                result.error = std::current_exception();
                std::lock_guard<std::mutex> guard(mutex_);
                fatal_ = true;
                tasks_.clear();
            }
            {
                std::lock_guard<std::mutex> guard(mutex_);
                --active_;
                results_.push_back(std::move(result));
            }
            cv_.notify_all();
        }
    }

    GmailApi& api_;
    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Task> tasks_;
    std::deque<FetchResult> results_;
    std::size_t active_ = 0;
    bool submitting_ = true;
    bool fatal_ = false;
    bool stopped_ = false;
};
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
    // "Already cached" is decided from one snapshot, not a store query per
    // message: the store lock stays available to the UI during a long check.
    // A message first cached mid-check is at most fetched once more with its
    // body, and ingestion preserves local edits on conflict.
    const auto baseline = store.cachedGmailIds(account.id);
    const std::unordered_set<std::string> known(baseline.begin(), baseline.end());
    FetchPool pool(api, cancelled);
    std::exception_ptr failure;
    auto process = [&](std::vector<FetchPool::FetchResult> batch) {
        if (batch.empty()) return;
        std::vector<GmailMessage> fresh;
        for (auto& result : batch) {
            ++done;
            if (result.error) { if (!failure) failure = result.error; continue; }
            if (result.notFound) { deleted.push_back(std::move(result.id)); continue; }
            // Editable drafts are synchronized through the stable draft ID.
            // Old message-cache entries are hidden to avoid read-only twins.
            const bool draft = std::find(result.message.labels.begin(), result.message.labels.end(), "DRAFT") != result.message.labels.end();
            if (!draft && !known.count(result.id)) ++report.downloaded;
            fresh.push_back(std::move(result.message));
        }
        if (!fresh.empty()) store.ingestGmailBatch(account.id, fresh);
        if (progress) progress({(full ? "Downloading " : "Updating ") + account.name + " — Gmail", 0, 0, done, total});
    };
    auto drain = [&] {
        for (bool finished = false; !finished;) process(pool.wait(400, finished));
    };
    if (full) {
        // The initial profile precedes all list/fetch requests. Changes made
        // during this full check remain discoverable by the following history check.
        checkpoint = profile.history;
        std::set<std::string> seen, pages; std::string page;
        do {
            checkCancelled(cancelled); auto messages = api.messages(page);
            for (const auto& id : messages.ids) if (seen.insert(id).second) pool.submit(id, !known.count(id));
            // Keep the pending queue bounded: ingest what has arrived so far.
            process(pool.poll(400));
            if (seen.size() > 5000000) throw NetworkError(NetworkFailure::Protocol, "This Gmail mailbox exceeds Mail's sync limit.");
            page = messages.next; nextPage(pages, page);
        } while (!page.empty());
        pool.finishSubmitting();
        drain();
        checkCancelled(cancelled);
        // List pagination is not a transaction. Confirm missing baseline IDs
        // individually so concurrent changes cannot silently hide cached mail.
        for (const auto& id : baseline) if (!seen.count(id)) pool.submit(id, !known.count(id));
        pool.finishSubmitting();
        drain();
    } else {
        for (const auto& id : changed) pool.submit(id, !known.count(id));
        pool.finishSubmitting();
        drain();
    }
    // A failed fetch aborts the check before the cursor advances, matching a
    // serial check; everything already ingested is idempotent.
    if (failure) std::rethrow_exception(failure);
    checkCancelled(cancelled); store.finishGmailSync(account.id, checkpoint, deleted);
    apply(); // Includes actions created by automatic filters on newly received mail.
    auto drafts=syncGmailDrafts(store,account,api,cancelled);
    report.downloaded+=drafts.downloaded; report.applied+=drafts.applied;
    report.errors.insert(report.errors.end(),drafts.errors.begin(),drafts.errors.end());
    return report;
}
}
