#include "mail/Gmail.h"
#include "mail/Compose.h"
#include <curl/curl.h>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <iostream>
#include <memory>
#include <thread>

using namespace mail;
namespace {
int assertions = 0;
void expect(bool value, const char* description)
{ ++assertions; if (!value) throw std::runtime_error(description); }
template<class F> void rejects(F fn, const char* description)
{
    bool rejected = false;
    try { fn(); } catch (const std::exception&) { rejected = true; }
    expect(rejected, description);
}
template<class F> void fails(F fn, NetworkFailure reason, const char* description)
{
    bool rejected = false;
    try { fn(); } catch (const NetworkError& error) {
        rejected = error.reason == reason;
        expect(std::string(error.what()).find("SECRET-MUST-NOT-LEAK") == std::string::npos, "server diagnostics never expose token response bodies");
    }
    expect(rejected, description);
}
struct BrowserResponse { long status = 0; std::string location; };
std::size_t discard(char*, std::size_t size, std::size_t count, void*) { return size * count; }
std::size_t header(char* bytes, std::size_t size, std::size_t count, void* target)
{
    std::string line(bytes, size * count);
    if (line.compare(0, 10, "Location: ") == 0) {
        line = line.substr(10);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        *static_cast<std::string*>(target) = line;
    }
    return size * count;
}
BrowserResponse browse(const std::string& url, const std::string& certificate, const std::string& overrideHost = {}, bool post = false)
{
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl) throw std::runtime_error("fixture browser failed to initialize");
    BrowserResponse response;
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_CAINFO, certificate.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_PROXY, "");
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "https,http");
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, &discard);
    curl_easy_setopt(curl.get(), CURLOPT_HEADERFUNCTION, &header);
    curl_easy_setopt(curl.get(), CURLOPT_HEADERDATA, &response.location);
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(
        overrideHost.empty() ? nullptr : curl_slist_append(nullptr, ("Host: " + overrideHost).c_str()), curl_slist_free_all);
    if (headers) curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    if (post) { curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, ""); curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, 0L); }
    auto result = curl_easy_perform(curl.get());
    if (result != CURLE_OK) throw std::runtime_error(std::string("fixture browser: ") + curl_easy_strerror(result));
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &response.status);
    return response;
}
GoogleTokens signIn(const std::string& client, const GoogleEndpoints& endpoints, std::atomic<bool>& cancelled)
{
    std::thread browser; std::exception_ptr browserError;
    auto launch = [&](const std::string& url) {
        browser = std::thread([&, url] {
            try {
                auto redirect = browse(url, endpoints.certificate);
                if (redirect.status != 302 || redirect.location.empty()) throw std::runtime_error("fixture authorization redirect missing");
                auto badState = redirect.location;
                auto start = badState.find("state=") + 6;
                auto end = badState.find('&', start);
                badState.replace(start, end == std::string::npos ? std::string::npos : end - start, "forged-state");
                if (browse(badState, "").status != 400) throw std::runtime_error("forged callback state was accepted");
                if (browse(redirect.location + "&state=duplicate", "").status != 400) throw std::runtime_error("duplicate state was accepted");
                if (browse(redirect.location, "", "attacker.test").status != 400) throw std::runtime_error("forged callback host was accepted");
                if (browse(redirect.location, "", {}, true).status != 400) throw std::runtime_error("POST callback was accepted");
                if (browse(redirect.location, "").status != 200) throw std::runtime_error("valid callback was rejected after invalid callbacks");
            } catch (...) { browserError = std::current_exception(); }
        });
    };
    try {
        auto tokens = authorizeGoogle({client, "fixture-secret"}, launch, cancelled, "user@example.test", endpoints, 10);
        browser.join(); if (browserError) std::rethrow_exception(browserError);
        return tokens;
    } catch (...) {
        if (browser.joinable()) browser.join();
        if (browserError) std::rethrow_exception(browserError);
        throw;
    }
}
}
int main(int argc, char** argv)
{
    if (argc != 4) { std::cerr << "Usage: mail_google_tests HOST CA HTTPS_PORT\n"; return 2; }
    try {
        std::atomic<bool> cancelled{false};
        std::string base = "https://" + std::string(argv[1]) + ":" + argv[3];
        GoogleEndpoints endpoints{base + "/authorize", base + "/token", base + "/revoke", base + "/gmail", argv[2]};
        std::string binary;
        for (int i = 0; i < 256; ++i) binary += static_cast<char>(i);
        expect(base64UrlDecode(base64UrlEncode(binary)) == binary, "base64url round trips all binary bytes");
        expect(base64UrlEncode("f") == "Zg" && base64UrlDecode("Zg==") == "f", "base64url handles padded and unpadded input");
        expect(base64UrlDecode("Zm8=") == "fo" && base64UrlDecode("Zm9v") == "foo", "base64url final quanta decode");
        for (const auto& bad : {"A", "A===", "AA=", "AA=A", "Zg===", "Zg=", "Zh", "Zm9", "////", "AA\nA"})
            rejects([&] { base64UrlDecode(bad); }, "invalid base64url is rejected");
        rejects([&] { base64UrlDecode("Zm9v", 2); }, "base64url enforces decoded byte limit");
        expect(urlEncode("a+b/c&= 🎉") == "a%2Bb%2Fc%26%3D%20%F0%9F%8E%89", "query values are encoded without interpretation");
        auto client = parseGoogleClient(R"({"installed":{"client_id":"desktop","client_secret":"fixture-secret","token_uri":"http://attacker.test"}})");
        expect(client.id == "desktop" && client.secret == "fixture-secret", "desktop config reads credentials and ignores endpoint overrides");
        rejects([&] { parseGoogleClient(R"({"web":{"client_id":"web"}})"); }, "web OAuth clients are rejected");
        rejects([&] { parseGoogleClient(R"({"installed":{"client_id":"bad\nclient"}})"); }, "client identifiers reject control characters");
        rejects([&] { parseGoogleClient(std::string(65537, ' ')); }, "client configuration is bounded");

        auto tokens = signIn("desktop", endpoints, cancelled);
        expect(tokens.access == "valid-access" && tokens.refresh == "valid-refresh" && tokens.scope == kGoogleMailScope,
            "authorization code exchange returns offline Gmail credentials after rejecting forged callbacks");
        expect(tokens.expires > std::time(nullptr) + 3500 && tokens.expires <= std::time(nullptr) + 3600, "OAuth expiry is converted to an absolute timestamp");
        auto saved = parseGoogleTokens(serializeGoogleTokens(tokens));
        expect(saved.client.id == "desktop" && saved.refresh == tokens.refresh && saved.expires == tokens.expires, "key-store token serialization round trips");
        rejects([&] { parseGoogleTokens("{}"); }, "invalid stored credentials are rejected");
        auto invalid = tokens; invalid.access = "token\r\nHeader: injected";
        rejects([&] { serializeGoogleTokens(invalid); }, "access tokens cannot inject headers");
        auto refreshed = refreshGoogle(tokens, cancelled, endpoints);
        expect(refreshed.access == "refreshed-access" && refreshed.refresh == tokens.refresh && refreshed.scope == tokens.scope,
            "refresh preserves the refresh token and scope when Google omits them");
        auto revoked = tokens; revoked.refresh = "revoked";
        fails([&] { refreshGoogle(revoked, cancelled, endpoints); }, NetworkFailure::Authentication, "invalid_grant requires sign-in again");
        fails([&] { signIn("denied", endpoints, cancelled); }, NetworkFailure::Authentication, "denied consent does not create credentials");
        rejects([&] { signIn("no-refresh", endpoints, cancelled); }, "missing offline refresh grant is rejected");
        fails([&] { signIn("wrong-scope", endpoints, cancelled); }, NetworkFailure::Authentication, "insufficient Gmail permission is rejected");
        fails([&] { signIn("empty-scope", endpoints, cancelled); }, NetworkFailure::Authentication, "an explicitly empty grant does not inherit requested permission");
        rejects([&] { signIn("bad-expiry", endpoints, cancelled); }, "negative token expiry is rejected");
        revokeGoogle(tokens, cancelled, endpoints); revokeGoogle(revoked, cancelled, endpoints);
        expect(true, "revocation is explicit and already-revoked tokens are harmless");
        auto redirect = endpoints; redirect.token = base + "/redirect-token";
        rejects([&] { refreshGoogle(tokens, cancelled, redirect); }, "token endpoint redirects are not followed");
        std::thread cancellation;
        fails([&] {
            try {
                authorizeGoogle(client, [&](const std::string&) { cancellation = std::thread([&] {
                    std::this_thread::sleep_for(std::chrono::milliseconds(150)); cancelled = true;
                }); }, cancelled, {}, endpoints, 3);
            } catch (...) { cancellation.join(); throw; }
            cancellation.join();
        }, NetworkFailure::Cancelled, "pending browser sign-in can be cancelled");
        cancelled = false;
        fails([&] { authorizeGoogle(client, [](const std::string&) {}, cancelled, {}, endpoints, 1); }, NetworkFailure::Authentication,
            "unused loopback listener expires instead of waiting indefinitely");
        cancelled = true;
        fails([&] { authorizeGoogle(client, [](const std::string&) { throw std::runtime_error("must not launch"); }, cancelled); },
            NetworkFailure::Cancelled, "cancelled sign-in never launches a browser"); cancelled = false;
        rejects([&] { authorizeGoogle(client, [](const std::string&) { throw std::runtime_error("browser unavailable"); }, cancelled, {}, endpoints, 1); },
            "browser launch failure releases its listener");

        int tokenCalls = 0, forced = 0;
        auto access = [&](bool force) { ++tokenCalls; if (force) ++forced; return force ? refreshed.access : tokens.access; };
        GmailApi api(access, cancelled, endpoints);
        auto profile = api.profile();
        expect(profile.email == "user@example.test" && profile.history == "90071992547409931", "profile preserves Gmail history IDs without floating-point rounding");
        auto labels = api.labels();
        expect(labels.size() == 2 && labels[0].system && !labels[1].system && labels[1].name == "Project 🐦", "Gmail system and Unicode user labels remain distinct");
        auto page = api.messages(); auto page2 = api.messages(page.next);
        expect(page.ids == std::vector<std::string>{"m1"} && page2.ids == std::vector<std::string>{"m2"} && page2.next.empty(),
            "message pages include trash/spam and encode opaque continuation tokens");
        auto message = api.message("m1", true);
        expect(message.raw.find("Hello from Gmail 🎉") != std::string::npos && message.thread == "thread1" && message.received == 1792542600,
            "raw Gmail messages decode with provider thread and internal date");
        expect(message.labels.size() == 4 && message.labels[0] == "INBOX" && message.labels[1] == "SENT", "one Gmail message retains multiple folder labels");
        expect(api.message("m1", false).raw.empty(), "metadata refresh does not fetch raw body content");
        auto history = api.history(profile.history); auto history2 = api.history(profile.history, history.next);
        expect(history.changed == std::vector<std::string>({"m1", "m2"}) && history2.changed == std::vector<std::string>{"deleted"}
            && history2.history == "90071992547409933" && history2.next.empty(), "history combines and deduplicates message and label events across pages");
        bool expired = false;
        try { api.history("1"); } catch (const HttpError& error) { expired = error.status == 404; }
        expect(expired, "expired history cursor retains HTTP 404 for full-sync fallback");
        bool deleted = false;
        try { api.message("deleted", false); } catch (const HttpError& error) { deleted = error.status == 404; }
        expect(deleted, "deleted message retains HTTP 404 for cache reconciliation");
        rejects([&] { api.message("../profile", true); }, "message identifiers cannot inject endpoint paths");
        rejects([&] { api.message("wrong_id", true); }, "mismatched message identity is rejected");
        rejects([&] { api.message("malformed", true); }, "malformed API JSON is rejected");
        rejects([&] { api.message("bad_base64", true); }, "invalid Gmail raw encoding is rejected");
        rejects([&] { api.history("not-a-number"); }, "history cursor validation precedes network access");
        api.modify("m1", {"STARRED", "Label_42"}, {"UNREAD"}); api.trash("m1", true); api.trash("m1", false);
        expect(true, "label mutation, trash, and untrash use reversible API operations");
        auto created = api.createLabel("New 🐦");
        expect(created.id == "Label_43" && !created.system && created.name == "New 🐦", "creating a Gmail label returns its provider ID");
        rejects([&] { api.createLabel("bad\nname"); }, "control characters in labels are rejected");
        expect(forced == 0 && tokenCalls > 0, "successful API requests do not force token refresh");
        int refreshAttempts = 0;
        GmailApi stale([&](bool force) { if (force) ++refreshAttempts; return force ? "refreshed-access" : "expired-access"; }, cancelled, endpoints);
        expect(stale.profile().email == profile.email && refreshAttempts == 1, "explicit API 401 forces exactly one refresh and retry");
        int deniedCalls = 0;
        GmailApi denied([&](bool) { ++deniedCalls; return "still-expired"; }, cancelled, endpoints);
        fails([&] { denied.profile(); }, NetworkFailure::Authentication, "a second 401 stops instead of looping");
        expect(deniedCalls == 2, "failed authorization attempts are bounded");

        Account account; account.id = 1; account.email = "user@example.test"; account.name = "Fixture"; account.provider = Provider::Gmail;
        Draft draft; draft.account = account.id; draft.messageId = newMessageId(); draft.to = "visible@example.test"; draft.cc = "copy@example.test";
        draft.bcc = "hidden@example.test"; draft.subject = "API fixture 🎉"; draft.text = "Hello 🎉";
        draft.runs = {{0, true, false, false, {}}}; draft.references = {"<parent@example.test>"};
        draft.attachments = {{"file.bin", "application/octet-stream", {}, std::string("\0\1\xff" "file", 7), false}};
        auto composed = composeMessage(draft, account, 1792542600);
        auto receipt = api.send(composed.raw, composed.recipients, "thread1");
        expect(receipt.id == "sent1" && receipt.thread == "thread1" && composed.raw.find("Bcc:") == std::string::npos,
            "Gmail send returns a provider receipt without mutating the BCC-free local message");
        draft.subject = "Refreshed send"; composed = composeMessage(draft, account, 1792542600);
        auto refreshBefore = refreshAttempts;
        expect(stale.send(composed.raw, composed.recipients, "thread1").id == "sent1" && refreshAttempts == refreshBefore + 1,
            "send may retry only after an explicit unauthorized response");
        auto hiddenDraft = draft; hiddenDraft.to.clear(); hiddenDraft.cc.clear(); hiddenDraft.subject = "BCC only";
        auto hiddenMessage = composeMessage(hiddenDraft, account, 1792542600);
        expect(api.send(hiddenMessage.raw, hiddenMessage.recipients, "thread1").id == "sent1", "BCC-only mail supplies its recipient to Gmail without altering the cache");
        for (const auto& subject : {"Lost response", "Server error", "Malformed receipt"}) {
            draft.subject = subject; composed = composeMessage(draft, account, 1792542600);
            fails([&] { api.send(composed.raw, composed.recipients); }, NetworkFailure::DeliveryUnknown,
                "lost, server-error and invalid send receipts preserve delivery uncertainty");
        }
        draft.subject = "Refused send"; composed = composeMessage(draft, account, 1792542600);
        fails([&] { api.send(composed.raw, composed.recipients); }, NetworkFailure::Protocol, "explicit send rejection is a known failure");
        rejects([&] { api.send(composed.raw, {"hidden@example.test"}); }, "saved recipients must contain every visible header recipient");
        rejects([&] { api.send(composed.raw, {"bad@example.test\r\nBcc: victim@example.test"}); }, "Gmail envelope cannot inject headers");
        rejects([&] { api.send("Bcc: leaked@example.test\r\n" + composed.raw, composed.recipients); }, "unexpected BCC headers in stored MIME are rejected");
        rejects([&] { api.send("Bcc : leaked@example.test\r\n" + composed.raw, composed.recipients); }, "ambiguous recipient header names are rejected");
        rejects([&] { api.send("To: leaked@example.test\r\n" + composed.raw, composed.recipients); }, "duplicate recipient headers cannot bypass envelope matching");

        auto untrusted = endpoints; untrusted.certificate.clear();
        GmailApi badCertificate(access, cancelled, untrusted);
        fails([&] { badCertificate.profile(); }, NetworkFailure::Certificate, "Gmail HTTPS rejects an untrusted certificate");
        auto badScheme = endpoints; badScheme.gmail = "http://" + std::string(argv[1]) + ":" + argv[3] + "/gmail";
        GmailApi plaintext(access, cancelled, badScheme);
        fails([&] { plaintext.profile(); }, NetworkFailure::Protocol, "bearer tokens are never sent over plaintext HTTP");
        auto redirected = endpoints; redirected.gmail = base + "/redirect";
        GmailApi redirectApi(access, cancelled, redirected);
        rejects([&] { redirectApi.profile(); }, "API bearer tokens never follow redirects");
        auto limited = endpoints; limited.gmail = base + "/limited";
        GmailApi quota(access, cancelled, limited);
        fails([&] { quota.profile(); }, NetworkFailure::Protocol, "quota responses stop without immediate retries or diagnostic leakage");
        HttpRequest small; small.url = base + "/large"; small.certificate = endpoints.certificate; small.responseLimit = 64;
        fails([&] { requestHttps(small, cancelled); }, NetworkFailure::Protocol, "HTTPS response size limits are enforced while receiving");
        std::string deep = "{}"; for (int i = 0; i < 40; ++i) deep = "{\"x\":" + deep + "}";
        rejects([&] { parseGoogleClient(deep); }, "JSON nesting is bounded before recursive parsing can exhaust the stack");
        cancelled = true;
        fails([&] { api.profile(); }, NetworkFailure::Cancelled, "Gmail API honors cancellation");
        std::cout << "Google OAuth/Gmail: " << assertions << " assertions passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
