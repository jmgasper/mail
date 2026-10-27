// Manual performance harness for large-mailbox measurements. Not part of
// CTest: build with `cmake --build build-perf --target mail_perf_tests` and
// run `build-perf/mail_perf_tests [messages] [threads-per-conversation]`.
// It seeds synthetic mail through the real Store and reports timings for the
// operations the UI and sync workers perform.
#include "mail/Store.h"
#include "mail/Mime.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

using namespace mail;
namespace {
using Clock = std::chrono::steady_clock;
double seconds(Clock::time_point start)
{ return std::chrono::duration<double>(Clock::now() - start).count(); }
const std::vector<GmailLabel> kLabels = {{"INBOX", "Inbox", true}, {"SENT", "Sent", true}, {"DRAFT", "Drafts", true},
    {"TRASH", "Trash", true}, {"SPAM", "Spam", true}, {"UNREAD", "Unread", true}, {"STARRED", "Starred", true}};
GmailMessage makeMessage(int index, int thread, const std::vector<std::string>& labels)
{
    std::string body = "Hello from message " + std::to_string(index) + " in thread " + std::to_string(thread) + ".\n";
    while (body.size() < 2048) body += "The quick brown fox jumps over the lazy dog. Pack my box with five dozen liquor jugs.\n";
    std::string raw = "From: sender" + std::to_string(index % 50) + "@example.test\r\nTo: user@example.test\r\n"
        "Subject: Conversation " + std::to_string(thread) + "\r\nMessage-ID: <m" + std::to_string(index) + "@example.test>\r\n\r\n" + body;
    return {"m" + std::to_string(index), "t" + std::to_string(thread), "100", labels, 1700000000 + index, raw};
}
std::vector<GmailMessage> makeMail(int count, int perThread)
{
    std::vector<GmailMessage> out; out.reserve(count);
    for (int i = 0; i < count; ++i) {
        std::vector<std::string> labels = {"INBOX"};
        if (i % 5 == 0) labels.push_back("UNREAD");
        if (i % 17 == 0) labels.push_back("STARRED");
        if (i % 23 == 0) labels.push_back("Label_1");
        out.push_back(makeMessage(i, i / perThread, labels));
    }
    return out;
}
void seed(Store& store, Id account, const std::vector<GmailMessage>& mail, bool batch)
{
    if (batch) {
        for (std::size_t at = 0; at < mail.size(); at += 500) {
            auto end = std::min(mail.size(), at + 500);
            store.ingestGmailBatch(account, std::vector<GmailMessage>(mail.begin() + at, mail.begin() + end));
        }
        return;
    }
    for (const auto& message : mail) store.ingestGmail(account, message);
}
double runQueries(Store& store, Id account, const std::string& label)
{
    Query inbox; inbox.account = account; inbox.folder = "inbox";
    auto start = Clock::now();
    auto rows = store.conversations(inbox, 1800000000);
    double list = seconds(start);
    std::cout << label << " conversations(inbox): " << rows.size() << " rows in " << list << " s\n";
    start = Clock::now();
    auto unread = store.unreadConversations(inbox, 1800000000);
    double count = seconds(start);
    std::cout << label << " unreadConversations(inbox): " << unread << " in " << count << " s\n";
    Query all; all.account = account; all.folder = "";
    start = Clock::now();
    auto everywhere = store.conversations(all, 1800000000);
    double allTime = seconds(start);
    std::cout << label << " conversations(all folders): " << everywhere.size() << " rows in " << allTime << " s\n";
    inbox.text = "quick";
    start = Clock::now();
    auto found = store.conversations(inbox, 1800000000);
    double search = seconds(start);
    std::cout << label << " conversations(search 'quick'): " << found.size() << " rows in " << search << " s\n";
    // Opening the largest conversation: full thread() with per-message reads.
    Id largest = 0; int members = 0;
    for (const auto& c : everywhere) if (c.count > members) { members = c.count; largest = c.id; }
    start = Clock::now();
    auto thread = store.thread(largest);
    double open = seconds(start);
    std::cout << label << " thread(" << thread.size() << " messages): " << open << " s\n";
    return list + count + allTime + search + open;
}
}
int main(int argc, char** argv)
{
    try {
        const int count = argc > 1 ? std::atoi(argv[1]) : 100000;
        const int perThread = argc > 2 ? std::atoi(argv[2]) : 25;
        if (count <= 0 || perThread <= 0) throw std::runtime_error("usage: mail_perf_tests [messages] [messages-per-conversation]");
        auto mail = makeMail(count, perThread);
        { // Micro-profile the MIME parser share of ingest cost.
            const int samples = 20000;
            auto start = Clock::now();
            std::size_t parsed = 0;
            for (int i = 0; i < samples; ++i) parsed += parseMessage(mail[i % mail.size()].raw).subject.size();
            double elapsed = seconds(start);
            std::cout << "[parse] " << samples << " MIME parses in " << elapsed << " s (" << samples / std::max(elapsed, 1e-9)
                      << " msg/s, check " << parsed << ")\n";
        }
        std::filesystem::path dir = std::filesystem::temp_directory_path() / "mail-perf";
        const bool keep = std::getenv("MAIL_PERF_KEEP_DB") != nullptr;
        std::filesystem::remove_all(dir); std::filesystem::create_directories(dir);

        // Baseline path: one transaction per message (the old sync behavior).
        if (!keep && !std::getenv("MAIL_PERF_BATCH_ONLY")) {
            std::string path = (dir / "serial.db").string();
            std::filesystem::remove(path);
            Store store(path);
            Account account; account.name = "Perf"; account.email = "perf@example.test"; account.provider = Provider::Gmail;
            account.id = store.saveAccount(account);
            store.saveGmailLabels(account.id, kLabels);
            store.saveGmailLabels(account.id, std::vector<GmailLabel>{{"Label_1", "Project", false}});
            auto start = Clock::now();
            seed(store, account.id, mail, false);
            double ingest = seconds(start);
            std::cout << "[serial ingest] " << count << " messages: " << ingest << " s (" << count / std::max(ingest, 1e-9)
                      << " msg/s)\n";
            runQueries(store, account.id, "[serial]");
        }
        // Optimized path: batched transactions.
        {
            std::string path = (dir / "batch.db").string();
            std::filesystem::remove(path);
            Store store(path);
            Account account; account.name = "Perf"; account.email = "perf@example.test"; account.provider = Provider::Gmail;
            account.id = store.saveAccount(account);
            store.saveGmailLabels(account.id, kLabels);
            store.saveGmailLabels(account.id, std::vector<GmailLabel>{{"Label_1", "Project", false}});
            auto start = Clock::now();
            seed(store, account.id, mail, true);
            double ingest = seconds(start);
            std::cout << "[batch ingest]  " << count << " messages: " << ingest << " s (" << count / std::max(ingest, 1e-9)
                      << " msg/s)\n";
            runQueries(store, account.id, "[batch]");
            if (keep) std::cout << "kept database at " << path << "\n";
        }
        if (!keep) std::filesystem::remove_all(dir);
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
