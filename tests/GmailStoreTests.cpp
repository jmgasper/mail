#include "mail/Store.h"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <unistd.h>

using namespace mail;
namespace {
int assertions = 0;
void expect(bool ok, const char* message) { ++assertions; if (!ok) throw std::runtime_error(message); }
template<class F> void rejects(F fn, const char* message)
{ bool failed = false; try { fn(); } catch (const std::exception&) { failed = true; } expect(failed, message); }
Account account(Store& store, const std::string& name)
{ Account value; value.name = name; value.email = name + "@example.test"; value.provider = Provider::Gmail; value.id = store.saveAccount(value); return value; }
const std::vector<GmailLabel> labels = {{"INBOX", "Inbox", true}, {"SENT", "Sent", true}, {"DRAFT", "Drafts", true},
    {"TRASH", "Trash", true}, {"SPAM", "Spam", true}, {"UNREAD", "Unread", true}, {"STARRED", "Starred", true}, {"Label_1", "Project 🐦", false}};
GmailMessage message(const std::string& id, const std::vector<std::string>& membership = {"INBOX", "SENT", "UNREAD", "Label_1"})
{
    return {id, "thread-" + id, "100", membership, 1792542600,
        "From: sender@example.test\r\nTo: user@example.test\r\nSubject: Gmail storage\r\nMessage-ID: <" + id + "@example.test>\r\n\r\nHello from Gmail\r\n"};
}
std::size_t count(Store& store, Id account, const std::string& folder, const std::string& tag = {})
{ Query q; q.account = account; q.folder = folder; q.tag = tag; return store.conversations(q, 2000000000).size(); }
PendingChange pending(Store& store, Id account, const std::string& kind)
{
    for (const auto& operation : store.pendingChanges(account)) if (operation.kind == kind) return operation;
    throw std::runtime_error("expected pending change missing");
}
bool has(const Message& message, const std::string& tag)
{ return std::find(message.tags.begin(), message.tags.end(), tag) != message.tags.end(); }
void storage(const std::string& path)
{
    Id aId = 0, messageId = 0;
    {
        Store store(path); auto a = account(store, "user"); aId = a.id;
        auto b = account(store, "other"); store.saveGmailLabels(a.id, labels); store.saveGmailLabels(b.id, labels);
        auto remote = message("m1"); messageId = store.ingestGmail(a.id, remote); auto id = messageId;
        expect(store.ingestGmail(a.id, remote) == id, "Gmail IDs are idempotent across duplicate ingestion");
        expect(store.ingestGmail(b.id, remote) != id, "identical Gmail IDs remain isolated between accounts");
        expect(count(store, a.id, "inbox") == 1 && count(store, a.id, "sent") == 1 && count(store, a.id, "remote:Label_1") == 1,
            "one message appears in every applicable folder without duplicate cache rows");
        expect(count(store, a.id, "archive") == 0 && store.thread(store.message(id).thread).size() == 1,
            "inbox message is not archived and label membership does not duplicate thread members");
        expect(has(store.message(id), "Project 🐦") && count(store, a.id, "", "Project 🐦") == 1, "Gmail user labels participate in existing tag and smart-folder queries");
        expect(store.remoteFolders(a.id).size() == 6, "sidebar exposes standard folders and user labels without unread/star duplicates");
        auto metadata = remote; metadata.raw.clear(); store.setRead(id, true); store.setStarred(id, true);
        store.ingestGmail(a.id, metadata);
        expect(!store.message(id).unread && store.message(id).starred, "old Gmail metadata cannot erase pending read/star edits");
        auto oldRead = pending(store, a.id, "read"); store.setRead(id, false); auto newRead = pending(store, a.id, "read");
        auto readServer = metadata; readServer.labels = {"INBOX", "SENT", "Label_1"};
        store.ingestGmail(a.id, readServer, oldRead.id);
        expect(store.message(id).unread && pending(store, a.id, "read").id == newRead.id, "stale Gmail acknowledgement preserves a newer opposite edit");
        store.ingestGmail(a.id, metadata, newRead.id);
        auto starredServer = metadata; starredServer.labels.push_back("STARRED");
        store.ingestGmail(a.id, starredServer, pending(store, a.id, "star").id);
        expect(store.pendingChanges(a.id).empty() && store.message(id).starred, "matching acknowledgements clear only their operations");
        store.move(id, "archive"); store.ingestGmail(a.id, starredServer);
        expect(count(store, a.id, "inbox") == 0 && count(store, a.id, "archive") == 1 && count(store, a.id, "sent") == 1
            && count(store, a.id, "remote:Label_1") == 1, "optimistic archive preserves Sent and user labels across an older snapshot");
        auto archived = starredServer; archived.labels.erase(archived.labels.begin());
        store.ingestGmail(a.id, archived, pending(store, a.id, "move").id);
        store.move(id, "trash");
        expect(count(store, a.id, "trash") == 1 && count(store, a.id, "sent") == 0 && count(store, a.id, "remote:Label_1") == 0,
            "trash membership suppresses ordinary folder and label views");
        auto trashed = archived; trashed.labels.push_back("TRASH"); store.ingestGmail(a.id, trashed, pending(store, a.id, "move").id);
        store.move(id, "inbox"); store.ingestGmail(a.id, trashed);
        expect(count(store, a.id, "inbox") == 1 && count(store, a.id, "trash") == 0, "pending restore survives a stale trash snapshot");
        store.ingestGmail(a.id, starredServer, pending(store, a.id, "move").id);

        store.tag(id, "Project 🐦", false); store.ingestGmail(a.id, starredServer);
        expect(!has(store.message(id), "Project 🐦") && count(store, a.id, "remote:Label_1") == 0, "pending tag removal suppresses an older provider membership");
        auto withoutTag = starredServer; withoutTag.labels.erase(std::find(withoutTag.labels.begin(), withoutTag.labels.end(), "Label_1"));
        store.ingestGmail(a.id, withoutTag, pending(store, a.id, "tag:Project 🐦").id);
        store.tag(id, "New label", true);
        expect(has(store.message(id), "New label"), "a new Gmail tag is visible before its server label exists");
        auto newLabels = labels; newLabels.push_back({"Label_2", "New label", false}); store.saveGmailLabels(a.id, newLabels);
        store.ingestGmail(a.id, withoutTag);
        expect(count(store, a.id, "remote:Label_2") == 1, "created label IDs inherit a pending local tag");
        auto withNew = withoutTag; withNew.labels.push_back("Label_2");
        store.ingestGmail(a.id, withNew, pending(store, a.id, "tag:New label").id);
        newLabels.back().name = "Renamed remotely"; store.saveGmailLabels(a.id, newLabels);
        expect(has(store.message(id), "Renamed remotely") && !has(store.message(id), "New label"), "provider label rename does not leave a stale local tag copy");
        store.tag(id, "Renamed remotely", false); auto oldTag = pending(store, a.id, "tag:Renamed remotely");
        store.tag(id, "Renamed remotely", true); auto newestTag = pending(store, a.id, "tag:Renamed remotely");
        store.ingestGmail(a.id, withoutTag, oldTag.id);
        expect(has(store.message(id), "Renamed remotely") && pending(store, a.id, "tag:Renamed remotely").id == newestTag.id,
            "a stale tag acknowledgement cannot discard a newer add");
        store.ingestGmail(a.id, withNew, newestTag.id);
        newLabels.pop_back(); store.saveGmailLabels(a.id, newLabels);
        expect(!has(store.message(id), "Renamed remotely"), "deleted provider labels disappear from tag views");
        store.saveGmailLabels(a.id, labels);
        rejects([&] { auto incomplete = message("missing"); incomplete.raw.clear(); store.ingestGmail(a.id, incomplete); }, "uncached messages require a raw body");
        expect(!store.findRemote(a.id, "gmail:missing"), "failed Gmail ingestion does not leave a partial row");
        store.finishGmailSync(a.id, "90071992547409931", {});
        store.setRead(id, false);
        rejects([&] { store.finishGmailSync(a.id, "90071992547409932", {"m1", "bad/id"}); }, "invalid deletion rolls back the entire checkpoint");
        expect(store.gmailHistory(a.id) == "90071992547409931" && count(store, a.id, "inbox") == 1 && !store.pendingChanges(a.id).empty(),
            "checkpoint rollback preserves cursor, visible cache, and pending edits");
        rejects([&] { store.finishGmailSync(a.id, "99", {}); }, "a stale sync cannot move the Gmail history cursor backwards");
        auto changed = a; changed.email = "wrong@example.test";
        rejects([&] { store.saveAccount(changed); }, "cached Google account identity cannot be changed to another mailbox");
        changed.email = "USER@example.test"; store.saveAccount(changed);
        expect(store.accounts()[0].email == changed.email, "case-only address edits preserve identity");
        store.finishGmailSync(a.id, "90071992547409932", {"m1"});
        expect(store.thread(store.message(id).thread).empty() && store.pendingChanges(a.id).empty(), "confirmed deletion hides the message and clears impossible mutations");
        expect(!store.message(id).raw.empty() && count(store, b.id, "inbox") == 1, "deletion retains recoverable raw content and leaves other accounts untouched");
        store.ingestGmail(a.id, remote);
        expect(store.findRemote(a.id, "gmail:m1") == id && count(store, a.id, "inbox") == 1, "restored provider identity reuses its cached row");
    }
    {
        Store store(path);
        expect(store.gmailHistory(aId) == "90071992547409932" && has(store.message(messageId), "Project 🐦"), "Gmail cursor and label membership survive restart");
        store.removeAccount(aId);
        expect(store.gmailHistory(aId).empty() && store.gmailLabels(aId).empty() && store.accounts().size() == 1, "account removal cascades Gmail state without touching another account");
        auto replacement = account(store, "replacement");
        expect(store.gmailHistory(replacement.id).empty() && store.gmailLabels(replacement.id).empty(), "new accounts never inherit old Gmail state");
    }
}
void filteringAndSending()
{
    Store store(":memory:"); auto a = account(store, "user"); store.saveGmailLabels(a.id, labels);
    Filter filter; filter.account = a.id; filter.name = "Gmail rule"; filter.subjectContains = "Gmail storage";
    filter.addTag = "Rule tag"; filter.moveTo = "archive"; filter.markRead = true; filter.star = true; store.saveFilter(filter);
    auto remote = message("filtered"); auto id = store.ingestGmail(a.id, remote);
    expect(store.pendingChanges(a.id).size() == 4 && !store.message(id).unread && store.message(id).starred && has(store.message(id), "Rule tag")
        && count(store, a.id, "inbox") == 0, "automatic rules queue Gmail read, star, archive and tag changes atomically");
    auto metadata = remote; metadata.raw.clear(); store.ingestGmail(a.id, metadata);
    expect(store.pendingChanges(a.id).size() == 4 && count(store, a.id, "inbox") == 0, "repeated metadata keeps filter effects without reapplying rules");
    auto updatedLabels = labels; updatedLabels.push_back({"Label_rule", "Rule tag", false}); store.saveGmailLabels(a.id, updatedLabels);
    metadata.labels = {"SENT", "STARRED", "Label_1", "Label_rule"};
    for (const auto& op : store.pendingChanges(a.id)) store.ingestGmail(a.id, metadata, op.id);
    metadata.labels = {"INBOX"}; store.ingestGmail(a.id, metadata);
    expect(store.pendingChanges(a.id).empty() && count(store, a.id, "inbox") == 1 && !has(store.message(id), "Rule tag"),
        "later remote changes replace acknowledged rules and do not trigger the filter again");
    for (const auto& folder : {"SENT", "DRAFT", "TRASH", "SPAM"}) {
        auto historic = message(std::string("historic-") + folder, {folder});
        auto historicId = store.ingestGmail(a.id, historic);
        expect(store.pendingChanges(a.id).empty() && !has(store.message(historicId), "Rule tag"),
            "full-sync downloads of historic non-inbox mail do not trigger receive rules or resurrect trash");
    }
    Draft draft; draft.account = a.id; draft.to = "recipient@example.test"; draft.subject = "Gmail storage"; draft.text = "Sent content";
    draft.providerThread = remote.thread; auto draftId = store.saveDraft(draft); auto outboxId = store.enqueueDraft(draftId, 1792542600);
    expect(store.claimOutgoing().id == outboxId, "Gmail outbox uses the same exclusive delivery claim");
    rejects([&] { store.finishOutgoing(outboxId, "sent", "", 1792542600); }, "Gmail acceptance requires a provider receipt");
    expect(store.outbox()[0].state == "sending", "missing receipt rolls back finalization instead of losing the draft");
    store.finishOutgoing(outboxId, "sent", "", 1792542600, {"sent1", remote.thread});
    auto sentId = store.findRemote(a.id, "gmail:sent1");
    expect(sentId && store.outbox().empty() && store.drafts(a.id).empty() && store.pendingChanges(a.id).empty(), "Gmail acceptance stores Sent and removes the draft without running receive filters");
    expect(store.message(sentId).thread == store.message(id).thread && !store.message(sentId).unread, "Gmail receipt attaches Sent to its provider thread");
    auto sent = message("sent1", {"SENT", "INBOX"}); sent.thread = remote.thread;
    expect(store.ingestGmail(a.id, sent) == sentId && store.thread(store.message(id).thread).size() == 2, "later synchronization reuses the provider Sent ID without a duplicate");

    draft.providerThread = "race-thread"; draft.subject = "Already received"; draftId = store.saveDraft(draft);
    outboxId = store.enqueueDraft(draftId, 1792542601); store.claimOutgoing();
    auto raced = message("raced", {"SENT", "INBOX", "Label_1"}); raced.thread = "race-thread";
    raced.raw.replace(raced.raw.find("Gmail storage"), std::string("Gmail storage").size(), "Already received");
    auto raceId = store.ingestGmail(a.id, raced);
    store.finishOutgoing(outboxId, "sent", "", 1792542601, {"raced", "race-thread"});
    expect(store.findRemote(a.id, "gmail:raced") == raceId && store.message(raceId).folder == "inbox" && has(store.message(raceId), "Project 🐦"),
        "send finalization preserves a copy and labels already downloaded by a concurrent sync");
}
}
int main()
{
    char directory[] = "mail-gmail-store-XXXXXX"; if (!mkdtemp(directory)) return 2;
    auto cleanup = [&] {
        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::directory_iterator(directory)) files.push_back(entry.path());
        for (const auto& file : files) std::filesystem::remove(file);
        std::filesystem::remove(directory);
    };
    try {
        storage(std::string(directory) + "/mail.db"); filteringAndSending(); cleanup();
        std::cout << "Gmail storage: " << assertions << " assertions passed\n"; return 0;
    } catch (const std::exception& error) { cleanup(); std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
