#include "mail/GoogleOAuth.h"
#include "GoogleJson.h"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <array>
#include <chrono>
#include <ctime>
#include <cerrno>
#include <map>
#include <sstream>
#include <algorithm>

namespace mail {
namespace {
using google::Json;
using Clock = std::chrono::steady_clock;
struct Socket {
    int fd = -1;
    explicit Socket(int value) : fd(value) {}
    ~Socket() { if (fd >= 0) close(fd); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
};
void validateClient(const GoogleClient& client)
{
    google::opaque(client.id, "Google desktop client ID", 512);
    if (!client.secret.empty()) google::opaque(client.secret, "Google desktop client secret", 512);
}
bool hasScope(const std::string& scope)
{
    std::istringstream stream(scope); std::string entry;
    while (stream >> entry) if (entry == kGoogleMailScope) return true;
    return false;
}
void validateTokens(const GoogleTokens& tokens)
{
    validateClient(tokens.client);
    google::opaque(tokens.access, "Google access token");
    google::opaque(tokens.refresh, "Google refresh token");
    if (tokens.expires <= 0 || !hasScope(tokens.scope))
        throw NetworkError(NetworkFailure::Authentication, "Google mail permission was not granted. Sign in again.");
}
std::string randomValue()
{
    std::array<unsigned char, 32> bytes{};
    if (RAND_bytes(bytes.data(), bytes.size()) != 1)
        throw NetworkError(NetworkFailure::Authentication, "Could not create a secure Google sign-in request.");
    return base64UrlEncode(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}
std::string challenge(const std::string& verifier)
{
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{}; unsigned int size = 0;
    if (EVP_Digest(verifier.data(), verifier.size(), digest.data(), &size, EVP_sha256(), nullptr) != 1)
        throw NetworkError(NetworkFailure::Authentication, "Could not create the Google sign-in challenge.");
    return base64UrlEncode(std::string(reinterpret_cast<const char*>(digest.data()), size));
}
void nonblocking(int fd)
{
    auto flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        throw NetworkError(NetworkFailure::Connection, "Could not prepare the Google sign-in listener.");
    fcntl(fd, F_SETFD, FD_CLOEXEC);
}
bool waitReadable(int fd, Clock::time_point deadline, const std::atomic<bool>& cancelled)
{
    while (Clock::now() < deadline) {
        if (cancelled) throw NetworkError(NetworkFailure::Cancelled, "Google sign-in was cancelled.");
        pollfd entry{fd, POLLIN, 0}; auto status = poll(&entry, 1, 100);
        if (status < 0 && errno == EINTR) continue;
        if (status < 0) throw NetworkError(NetworkFailure::Connection, "The Google sign-in listener failed.");
        if (status > 0) return (entry.revents & POLLIN) != 0;
    }
    return false;
}
std::string decodeQuery(const std::string& input)
{
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    for (std::size_t i = 0; i < input.size(); ++i) {
        unsigned char c = input[i];
        if (c == '%') {
            if (i + 2 >= input.size() || digit(input[i+1]) < 0 || digit(input[i+2]) < 0) throw std::invalid_argument("query");
            c = digit(input[i+1]) * 16 + digit(input[i+2]); i += 2;
        } else if (c == '+') c = ' ';
        if (c < 32 || c == 127) throw std::invalid_argument("query");
        out += c;
    }
    return out;
}
std::map<std::string, std::string> callbackQuery(const std::string& request, const std::string& host)
{
    auto first = request.find("\r\n");
    if (first == std::string::npos || request.compare(0, 4, "GET ") != 0) throw std::invalid_argument("request");
    auto space = request.find(' ', 4);
    if (space == std::string::npos || request.substr(space, first - space) != " HTTP/1.1") throw std::invalid_argument("request");
    auto target = request.substr(4, space - 4);
    constexpr const char* path = "/oauth/callback?";
    if (target.compare(0, std::char_traits<char>::length(path), path) != 0) throw std::invalid_argument("path");
    bool foundHost = false;
    for (std::size_t start = first + 2; start < request.size();) {
        auto end = request.find("\r\n", start); if (end == std::string::npos) throw std::invalid_argument("header");
        if (end == start) break;
        auto colon = request.find(':', start); if (colon >= end) throw std::invalid_argument("header");
        auto name = request.substr(start, colon - start);
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        auto value = request.substr(colon + 1, end - colon - 1);
        auto begin = value.find_first_not_of(" \t"), last = value.find_last_not_of(" \t");
        value = begin == std::string::npos ? "" : value.substr(begin, last - begin + 1);
        if (name == "host") { if (foundHost || value != host) throw std::invalid_argument("host"); foundHost = true; }
        if (name == "transfer-encoding" || (name == "content-length" && value != "0")) throw std::invalid_argument("body");
        start = end + 2;
    }
    if (!foundHost) throw std::invalid_argument("host");
    auto query = target.substr(std::char_traits<char>::length(path));
    std::map<std::string, std::string> result;
    for (std::size_t start = 0; start < query.size();) {
        auto end = query.find('&', start); if (end == std::string::npos) end = query.size();
        auto equal = query.find('=', start); if (equal >= end) throw std::invalid_argument("query");
        if (!result.emplace(decodeQuery(query.substr(start, equal - start)), decodeQuery(query.substr(equal + 1, end - equal - 1))).second)
            throw std::invalid_argument("duplicate");
        start = end + 1;
    }
    return result;
}
void respond(int fd, bool accepted)
{
    std::string body = accepted ? "<!doctype html><title>Mail</title><p>Return to Mail to finish signing in. You can close this tab.</p>"
        : "<!doctype html><title>Mail</title><p>This sign-in response was not accepted. Return to the Google sign-in tab.</p>";
    std::string wire = std::string("HTTP/1.1 ") + (accepted ? "200 OK" : "400 Bad Request")
        + "\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\nReferrer-Policy: no-referrer\r\nContent-Security-Policy: default-src 'none'\r\nConnection: close\r\nContent-Length: "
        + std::to_string(body.size()) + "\r\n\r\n" + body;
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL;
#endif
    // A tiny best-effort response; the authorization outcome does not depend on
    // a browser retaining its connection. No code/state/token is reflected.
    std::size_t sent = 0;
    while (sent < wire.size()) {
        auto count = send(fd, wire.data() + sent, wire.size() - sent, flags);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        sent += count;
    }
}
GoogleTokens tokenRequest(const GoogleTokens& previous, const std::string& form,
    const std::atomic<bool>& cancelled, const GoogleEndpoints& endpoints)
{
    HttpRequest request; request.url = endpoints.token; request.method = "POST";
    request.contentType = "application/x-www-form-urlencoded"; request.body = form;
    request.certificate = endpoints.certificate; request.responseLimit = 128 * 1024;
    auto response = requestHttps(request, cancelled);
    if (response.status == 400) {
        auto value = google::parse(response.body);
        if (google::string(value, "error", 256, false) == "invalid_grant")
            throw NetworkError(NetworkFailure::Authentication, "Google sign-in has expired or been revoked. Sign in again.");
    }
    requireHttpSuccess(response);
    auto value = google::parse(response.body); auto tokens = previous;
    auto type = google::string(value, "token_type", 32);
    std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (type != "bearer") throw NetworkError(NetworkFailure::Authentication, "Google returned an unsupported token type.");
    tokens.access = google::string(value, "access_token");
    auto refresh = google::string(value, "refresh_token", 16384, false);
    if (!refresh.empty()) tokens.refresh = refresh;
    if (value.contains("scope")) tokens.scope = google::string(value, "scope", 16384, false);
    auto expires = value.find("expires_in");
    if (expires == value.end() || !expires->is_number_integer() || *expires <= 0 || *expires > 86400)
        throw NetworkError(NetworkFailure::Protocol, "Google returned an invalid token lifetime.");
    tokens.expires = std::time(nullptr) + expires->get<Timestamp>();
    validateTokens(tokens); return tokens;
}
std::string clientForm(const GoogleClient& client)
{
    std::string form = "client_id=" + urlEncode(client.id);
    if (!client.secret.empty()) form += "&client_secret=" + urlEncode(client.secret);
    return form;
}
}
GoogleClient parseGoogleClient(const std::string& text)
{
    if (text.size() > 65536) throw NetworkError(NetworkFailure::Protocol, "The Google desktop client file is too large.");
    auto value = google::parse(text); auto installed = value.find("installed");
    if (installed == value.end() || !installed->is_object())
        throw NetworkError(NetworkFailure::Protocol, "Mail requires a Google desktop application client configuration.");
    GoogleClient client{google::string(*installed, "client_id", 512), google::string(*installed, "client_secret", 512, false)};
    validateClient(client); return client;
}
std::string serializeGoogleTokens(const GoogleTokens& tokens)
{
    validateTokens(tokens);
    return Json{{"version", 1}, {"client_id", tokens.client.id}, {"client_secret", tokens.client.secret},
        {"access_token", tokens.access}, {"refresh_token", tokens.refresh}, {"expires", tokens.expires}, {"scope", tokens.scope}}.dump();
}
GoogleTokens parseGoogleTokens(const std::string& text)
{
    if (text.size() > 128 * 1024) throw NetworkError(NetworkFailure::Authentication, "The stored Google credentials are invalid. Sign in again.");
    auto value = google::parse(text);
    if (!value.contains("version") || value["version"] != 1 || !value.contains("expires") || !value["expires"].is_number_integer()
        || value["expires"] <= 0 || value["expires"] > INT64_MAX)
        throw NetworkError(NetworkFailure::Authentication, "The stored Google credentials are invalid. Sign in again.");
    GoogleTokens tokens{{google::string(value, "client_id", 512), google::string(value, "client_secret", 512, false)},
        google::string(value, "access_token"), google::string(value, "refresh_token"), value["expires"].get<Timestamp>(), google::string(value, "scope")};
    validateTokens(tokens); return tokens;
}
GoogleTokens authorizeGoogle(const GoogleClient& client, const std::function<void(const std::string&)>& launchBrowser,
    const std::atomic<bool>& cancelled, const std::string& loginHint, const GoogleEndpoints& endpoints, int timeoutSeconds)
{
    validateClient(client);
    if (cancelled) throw NetworkError(NetworkFailure::Cancelled, "Google sign-in was cancelled.");
    if (timeoutSeconds < 1 || timeoutSeconds > 600) throw std::invalid_argument("Invalid OAuth timeout");
    if (endpoints.authorize.compare(0, 8, "https://") != 0 || endpoints.authorize.find_first_of("?#\r\n") != std::string::npos)
        throw NetworkError(NetworkFailure::Protocol, "Google authorization requires a valid HTTPS endpoint.");
    Socket listener(socket(AF_INET, SOCK_STREAM, 0));
    if (listener.fd < 0) throw NetworkError(NetworkFailure::Connection, "Could not start the Google sign-in listener.");
    nonblocking(listener.fd);
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = 0;
#ifdef __HAIKU__
    address.sin_len = sizeof(address);
#endif
    if (bind(listener.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 || listen(listener.fd, 8) < 0)
        throw NetworkError(NetworkFailure::Connection, "Could not bind the Google sign-in listener on localhost.");
    socklen_t length = sizeof(address);
    if (getsockname(listener.fd, reinterpret_cast<sockaddr*>(&address), &length) < 0)
        throw NetworkError(NetworkFailure::Connection, "Could not find the Google sign-in listener port.");
    const auto host = "127.0.0.1:" + std::to_string(ntohs(address.sin_port));
    const auto redirect = "http://" + host + "/oauth/callback";
    const auto state = randomValue(), verifier = randomValue();
    auto url = endpoints.authorize + "?client_id=" + urlEncode(client.id) + "&redirect_uri=" + urlEncode(redirect)
        + "&response_type=code&scope=" + urlEncode(kGoogleMailScope) + "&access_type=offline&prompt=consent%20select_account"
        + "&code_challenge_method=S256&code_challenge=" + urlEncode(challenge(verifier)) + "&state=" + urlEncode(state);
    if (!loginHint.empty()) url += "&login_hint=" + urlEncode(loginHint);
    const auto deadline = Clock::now() + std::chrono::seconds(timeoutSeconds);
    launchBrowser(url);
    while (waitReadable(listener.fd, deadline, cancelled)) {
        Socket connection(accept(listener.fd, nullptr, nullptr));
        if (connection.fd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            throw NetworkError(NetworkFailure::Connection, "Could not accept the Google sign-in response.");
        }
        nonblocking(connection.fd); std::string request;
        const auto connectionDeadline = std::min(deadline, Clock::now() + std::chrono::seconds(2));
        while (request.size() < 8192 && request.find("\r\n\r\n") == std::string::npos
            && waitReadable(connection.fd, connectionDeadline, cancelled)) {
            char bytes[1024]; auto size = recv(connection.fd, bytes, std::min(sizeof(bytes), 8192 - request.size()), 0);
            if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
            if (size <= 0) break;
            request.append(bytes, size);
        }
        std::map<std::string, std::string> query;
        try {
            if (request.find("\r\n\r\n") == std::string::npos) throw std::invalid_argument("incomplete");
            query = callbackQuery(request, host);
            if (query["state"].size() != state.size() || CRYPTO_memcmp(query["state"].data(), state.data(), state.size()) != 0
                || (query.count("code") && query.count("error")) || (!query.count("code") && !query.count("error")))
                throw std::invalid_argument("state");
            if (query.count("code")) google::opaque(query["code"], "authorization code", 4096);
        } catch (const std::invalid_argument&) { respond(connection.fd, false); continue; }
          catch (const NetworkError&) { respond(connection.fd, false); continue; }
        respond(connection.fd, true);
        if (query.count("error")) throw NetworkError(NetworkFailure::Authentication, "Google sign-in was not authorized. No account was added.");
        GoogleTokens initial; initial.client = client; initial.scope = kGoogleMailScope;
        return tokenRequest(initial, clientForm(client) + "&grant_type=authorization_code&code=" + urlEncode(query["code"])
            + "&redirect_uri=" + urlEncode(redirect) + "&code_verifier=" + urlEncode(verifier), cancelled, endpoints);
    }
    if (cancelled) throw NetworkError(NetworkFailure::Cancelled, "Google sign-in was cancelled.");
    throw NetworkError(NetworkFailure::Authentication, "Google sign-in timed out. Try signing in again.");
}
GoogleTokens refreshGoogle(const GoogleTokens& tokens, const std::atomic<bool>& cancelled, const GoogleEndpoints& endpoints)
{
    validateTokens(tokens);
    return tokenRequest(tokens, clientForm(tokens.client) + "&grant_type=refresh_token&refresh_token=" + urlEncode(tokens.refresh), cancelled, endpoints);
}
void revokeGoogle(const GoogleTokens& tokens, const std::atomic<bool>& cancelled, const GoogleEndpoints& endpoints)
{
    validateTokens(tokens); HttpRequest request;
    request.url = endpoints.revoke; request.method = "POST"; request.contentType = "application/x-www-form-urlencoded";
    request.body = "token=" + urlEncode(tokens.refresh); request.certificate = endpoints.certificate; request.responseLimit = 128 * 1024;
    auto response = requestHttps(request, cancelled);
    if (response.status == 400 && google::string(google::parse(response.body), "error", 256, false) == "invalid_token") return;
    requireHttpSuccess(response);
}
}
