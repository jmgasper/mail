#include "mail/Sync.h"
#include "mail/Outbox.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <iostream>

using namespace mail;
using Json = nlohmann::json;
namespace {
int assertions = 0;
void expect(bool ok, const char* message) { ++assertions; if (!ok) throw std::runtime_error(message); }
template<class F> void rejects(F fn, const char* message)
{ bool failed = false; try { fn(); } catch (const std::exception&) { failed = true; } expect(failed, message); }
GmailMessage cached(const std::string& id)
{
    return {id, "t-" + id, "100", {"INBOX"}, 1792542600,
        "From: sender@example.test\r\nTo: user@example.test\r\nSubject: Cached\r\nMessage-ID: <" + id + "@example.test>\r\n\r\nCached body\r\n"};
}
bool visible(Store& store, Id id) { return !store.thread(store.message(id).thread).empty(); }
bool label(const Json& stats, const std::string& id, const std::string& name)
{
    const auto& list = stats["labels"][id];
    return std::any_of(list.begin(), list.end(), [&](const Json& value) {
        return value.is_string() && value.get_ref<const std::string&>() == name;
    });
}
}
int main(int argc, char** argv)
{
    if (argc != 4) { std::cerr << "Usage: mail_gmail_sync_tests HOST CA HTTPS_PORT\n"; return 2; }
    try {
        std::atomic<bool> cancel{false}; std::string base = "https://" + std::string(argv[1]) + ":" + argv[3];
        GoogleEndpoints endpoints; endpoints.gmail = base + "/gmail"; endpoints.certificate = argv[2];
        auto fixture = [&](const std::string& path, const Json& value = Json()) {
            HttpRequest request; request.url = base + "/fixture/" + path; request.certificate = argv[2];
            if (!value.is_null()) { request.method = "POST"; request.contentType = "application/json"; request.body = value.dump(); }
            auto response = requestHttps(request, cancel); requireHttpSuccess(response); return Json::parse(response.body);
        };
        auto stage = [&](const char* name) { fixture("stage", Json{{"phase", name}}); };
        Store store(":memory:"); Account account; account.provider = Provider::Gmail; account.name = "Gmail fixture"; account.email = "user@example.test";
        account.id = store.saveAccount(account);
        auto vanished = store.ingestGmail(account.id, cached("vanished"));
        auto omitted = store.ingestGmail(account.id, cached("omitted"));
        GmailApi api([](bool) { return "fixture-access"; }, cancel, endpoints);
        bool injected = false;
        auto full = syncGmailAccount(store, account, api, cancel, [&](const SyncProgress& progress) {
            if (injected || progress.text.rfind("Downloading", 0) != 0) return;
            injected = true;
            Draft draft; draft.account = account.id; draft.to = "recipient@example.test"; draft.subject = "Concurrent send"; draft.text = "Sent during receive";
            auto id = store.saveDraft(draft); auto queued = store.enqueueDraft(id, 1792542600); store.claimOutgoing();
            store.finishOutgoing(queued, "sent", "", 1792542600, {"concurrent", "t-concurrent"});
        });
        expect(full.errors.empty() && full.downloaded == 2 && store.gmailHistory(account.id) == "100", "full sync downloads uncached bodies and checkpoints only after all pages");
        expect(!visible(store, vanished) && visible(store, omitted), "missing page entries require individual deletion confirmation");
        auto concurrent = store.findRemote(account.id, "gmail:concurrent");
        expect(concurrent && visible(store, concurrent), "a Sent copy created during a full check is not hidden by its earlier baseline");
        auto a = store.findRemote(account.id, "gmail:a"), b = store.findRemote(account.id, "gmail:b");
        expect(a && b && store.message(a).thread == store.message(b).thread && store.thread(store.message(a).thread).size() == 2,
            "Gmail provider threads span Inbox and Sent without duplicating members");
        auto stats = fixture("stats");
        expect(stats["raw_fetches"].size() == 2 && stats["raw_fetches"]["a"] == 1 && stats["raw_fetches"]["b"] == 1,
            "cached baseline messages use metadata instead of downloading their bodies again");
        expect(std::find(stats["gets"].begin(), stats["gets"].end(), "concurrent") == stats["gets"].end(), "full reconciliation excludes a concurrent new Sent ID from missing-message probes");

        Filter filter; filter.account = account.id; filter.name = "News"; filter.senderContains = "news@";
        filter.addTag = "Rule"; filter.moveTo = "archive"; filter.markRead = true; filter.star = true; store.saveFilter(filter);
        stage("partial"); auto partial = syncGmailAccount(store, account, api, cancel);
        auto c = store.findRemote(account.id, "gmail:c");
        expect(partial.errors.empty() && partial.downloaded == 1 && partial.applied == 4 && store.gmailHistory(account.id) == "200",
            "history sync fetches additions and sends all automatic filter actions");
        expect(c && !store.message(a).unread && store.message(a).starred && store.thread(store.message(a).thread).size() == 1,
            "history reconciles remote read/star changes and deletion within a thread");
        expect(!store.message(c).unread && store.message(c).starred && store.message(c).folder == "archive" && store.pendingChanges(account.id).empty(),
            "Gmail filter acknowledgements leave no pending work");
        stats = fixture("stats");
        expect(label(stats, "c", "Label_rule") && label(stats, "c", "STARRED") && !label(stats, "c", "INBOX") && !label(stats, "c", "UNREAD"),
            "the HTTPS provider independently observes tag, star, archive and read changes");
        expect(stats["label_creates"] == 1 && stats["raw_fetches"].size() == 3 && !stats["raw_fetches"].contains("concurrent"),
            "label-creation races recover through re-listing and known Sent bodies are not fetched again");

        store.setRead(a, false); store.tag(a, "Project", false); store.move(a, "junk");
        stage("local"); auto local = syncGmailAccount(store, account, api, cancel); stats = fixture("stats");
        expect(local.applied == 3 && local.errors.empty() && store.gmailHistory(account.id) == "300" && store.pendingChanges(account.id).empty(),
            "queued local Gmail edits are applied even when history contains no events");
        expect(label(stats, "a", "UNREAD") && label(stats, "a", "SPAM") && !label(stats, "a", "INBOX") && !label(stats, "a", "Label_1"),
            "read, user label removal and junk move preserve unrelated provider state");
        store.move(a, "trash"); syncGmailAccount(store, account, api, cancel);
        expect(store.message(a).folder == "trash" && label(fixture("stats"), "a", "TRASH"), "trash is performed through Gmail's reversible endpoint");
        store.move(a, "inbox"); syncGmailAccount(store, account, api, cancel); stats = fixture("stats");
        expect(store.message(a).folder == "inbox" && label(stats, "a", "INBOX") && !label(stats, "a", "TRASH") && !label(stats, "a", "SPAM"),
            "restore untrashes before applying the requested destination");
        store.tag(a, "Project", true); stage("label-missing"); auto missingLabel = syncGmailAccount(store, account, api, cancel);
        expect(!missingLabel.errors.empty() && visible(store, a) && store.pendingChanges(account.id).size() == 1,
            "a mutation's missing-label 404 never hides an existing message or acknowledges the failed edit");
        stage("label-restored"); syncGmailAccount(store, account, api, cancel);
        expect(store.pendingChanges(account.id).empty() && label(fixture("stats"), "a", "Label_1"), "failed label mutation remains retryable after server recovery");

        auto doomed = store.ingestGmail(account.id, cached("doomed"));
        stage("broken-full"); rejects([&] { syncGmailAccount(store, account, api, cancel); }, "expired cursor fallback stops on a failed list page");
        expect(store.gmailHistory(account.id) == "300" && visible(store, doomed), "interrupted full fallback neither advances history nor hides unconfirmed missing mail");
        stage("repeated-full"); rejects([&] { syncGmailAccount(store, account, api, cancel); }, "a repeated list continuation token fails instead of looping");
        expect(store.gmailHistory(account.id) == "300" && visible(store, omitted), "invalid pagination leaves the prior checkpoint usable");
        stage("complete-full"); auto recovered = syncGmailAccount(store, account, api, cancel);
        expect(recovered.downloaded == 0 && store.gmailHistory(account.id) == "500" && !visible(store, doomed) && visible(store, omitted) && visible(store, concurrent),
            "successful full recovery reuses bodies and hides only confirmed deletions");
        stage("broken-history"); rejects([&] { syncGmailAccount(store, account, api, cancel); }, "incomplete history pages are not committed");
        expect(store.gmailHistory(account.id) == "500", "history errors preserve the last complete checkpoint");
        stage("complete-history"); syncGmailAccount(store, account, api, cancel);
        expect(store.gmailHistory(account.id) == "600" && store.message(a).unread && !store.message(a).starred,
            "restarting history from its old cursor applies previously missed updates");
        stage("wrong-account"); rejects([&] { syncGmailAccount(store, account, api, cancel); }, "wrong Google profile cannot sync into another account's cache");
        expect(store.gmailHistory(account.id) == "600", "identity rejection leaves the mailbox checkpoint intact");
        stage("complete-history");

        auto factory = [&](const Account&) { return std::make_unique<GmailApi>([](bool) { return "fixture-access"; }, cancel, endpoints); };
        int smtpCalls = 0;
        auto noSmtp = [&](const Account&) -> std::string { ++smtpCalls; throw std::runtime_error("Gmail must not use SMTP credentials"); };
        auto queue = [&](const std::string& subject) {
            Draft draft; draft.account = account.id; draft.to = "recipient@example.test"; draft.bcc = "hidden@example.test";
            draft.subject = subject; draft.text = "Queued through Gmail"; draft.providerThread = "t1";
            draft.references = {"<a@example.test>"}; auto id = store.saveDraft(draft); return store.enqueueDraft(id, 1792542700);
        };
        queue("Delivered"); auto sent = deliverQueued(store, noSmtp, cancel, factory); auto sentId = store.findRemote(account.id, "gmail:sent1");
        expect(sent.sent == 1 && sentId && store.outbox().empty() && store.drafts(account.id).empty(), "Gmail outbox uses its API and atomically creates the provider Sent copy");
        expect(store.message(sentId).providerThread == "t1" && store.message(sentId).raw.find("Bcc:") == std::string::npos,
            "Gmail delivery retains reply threading and the BCC-free local cache");
        stage("sent"); syncGmailAccount(store, account, api, cancel);
        expect(store.findRemote(account.id, "gmail:sent1") == sentId && store.thread(store.message(a).thread).size() == 2,
            "subsequent Gmail history does not duplicate the outgoing copy");
        queue("Lost"); auto lost = deliverQueued(store, noSmtp, cancel, factory);
        expect(lost.uncertain == 1 && store.outbox().size() == 1 && store.outbox()[0].state == "uncertain", "an accepted upload with a lost response remains an uncertain outbox entry");
        auto again = deliverQueued(store, noSmtp, cancel, factory);
        expect(again.sent == 0 && again.failed == 0 && again.uncertain == 0, "later delivery passes never automatically resend uncertain Gmail mail");
        queue("Refused"); auto refused = deliverQueued(store, noSmtp, cancel, factory);
        expect(refused.failed == 1 && store.outbox().back().state == "failed", "explicit API rejection retains a failed draft for review");
        queue("Wrong identity"); stage("wrong-account"); auto wrong = deliverQueued(store, noSmtp, cancel, factory);
        expect(wrong.failed == 1 && fixture("stats")["send_attempts"].size() == 3, "outbox verifies Google identity before any send attempt");
        queue("No session"); auto missing = deliverQueued(store, noSmtp, cancel);
        expect(missing.failed == 1 && fixture("stats")["send_attempts"].size() == 3 && smtpCalls == 0, "missing Gmail authorization never falls back to SMTP");
        auto cancelledId = queue("Cancelled"); cancel = true;
        auto stopped = deliverQueued(store, noSmtp, cancel, factory);
        expect(stopped.sent == 0 && store.outbox().back().id == cancelledId && store.outbox().back().state == "queued", "cancelled delivery leaves unclaimed Gmail messages queued");
        std::cout << "Gmail sync/outbox: " << assertions << " assertions passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
