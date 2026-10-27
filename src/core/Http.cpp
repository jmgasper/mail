#include "mail/Http.h"
#include <curl/curl.h>
#include <memory>
#include <limits>

namespace mail {
namespace {
struct ResponseBuffer { std::string body; std::size_t limit; bool exceeded = false; bool failed = false; };
std::size_t receive(char* data, std::size_t size, std::size_t count, void* user)
{
    auto& buffer = *static_cast<ResponseBuffer*>(user);
    if (size && count > std::numeric_limits<std::size_t>::max() / size) return 0;
    auto bytes = size * count;
    if (bytes > buffer.limit - buffer.body.size()) { buffer.exceeded = true; return 0; }
    try { buffer.body.append(data, bytes); } catch (...) { buffer.failed = true; return 0; }
    return bytes;
}
int progress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{ return *static_cast<const std::atomic<bool>*>(user) ? 1 : 0; }
bool headerSafe(const std::string& value)
{ for (unsigned char c : value) if (c < 32 || c == 127) return false; return true; }
// One easy handle per thread: repeated API calls reuse its connection and TLS
// session instead of paying a handshake per request. curl_easy_reset clears
// the options but keeps the connection cache, so every option is set fresh
// on each request.
CURL* acquireCurl()
{
    struct Cached { CURL* handle = nullptr; ~Cached() { if (handle) curl_easy_cleanup(handle); } };
    static thread_local Cached cached;
    if (!cached.handle) {
        cached.handle = curl_easy_init();
        if (!cached.handle) throw NetworkError(NetworkFailure::Connection, "Could not initialize HTTPS.");
    } else curl_easy_reset(cached.handle);
    return cached.handle;
}
}
HttpError::HttpError(long code, const std::string& message)
    : NetworkError(code == 401 ? NetworkFailure::Authentication : NetworkFailure::Protocol, message), status(code) {}

HttpResponse requestHttps(const HttpRequest& request, const std::atomic<bool>& cancelled)
{
    if (cancelled) throw NetworkError(NetworkFailure::Cancelled, "The request was cancelled.");
    static const auto initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (initialized != CURLE_OK) throw NetworkError(NetworkFailure::Connection, "Could not initialize HTTPS.");
    using Url = std::unique_ptr<CURLU, decltype(&curl_url_cleanup)>;
    Url url(curl_url(), curl_url_cleanup);
    if (!url || curl_url_set(url.get(), CURLUPART_URL, request.url.c_str(), 0) != CURLUE_OK)
        throw NetworkError(NetworkFailure::Protocol, "The API endpoint is invalid.");
    auto part = [&](CURLUPart key) {
        char* value = nullptr; auto status = curl_url_get(url.get(), key, &value, 0);
        std::unique_ptr<char, decltype(&curl_free)> owned(value, curl_free);
        return status == CURLUE_OK ? std::string(value) : std::string();
    };
    if (part(CURLUPART_SCHEME) != "https" || !part(CURLUPART_USER).empty() || !part(CURLUPART_PASSWORD).empty()
        || !part(CURLUPART_FRAGMENT).empty() || request.url.find('\0') != std::string::npos)
        throw NetworkError(NetworkFailure::Protocol, "The API requires an HTTPS endpoint without embedded credentials.");
    if (!headerSafe(request.bearer) || !headerSafe(request.contentType) || request.bearer.size() > 16384)
        throw NetworkError(NetworkFailure::Authentication, "The stored access token is invalid. Sign in again.");
    if (request.method != "GET" && request.method != "POST" && request.method != "DELETE" && request.method != "PUT" && request.method != "PATCH")
        throw NetworkError(NetworkFailure::Protocol, "Unsupported HTTP method.");
    CURL* const handle = acquireCurl();
    using Curl = std::unique_ptr<CURL, void (*)(CURL*)>;
    Curl curl(handle, [](CURL*) {}); // Owned by the thread-local cache.
    curl_slist* list = nullptr;
    auto add = [&](const std::string& text) {
        auto next = curl_slist_append(list, text.c_str());
        if (!next) { curl_slist_free_all(list); list = nullptr; throw std::bad_alloc(); }
        list = next;
    };
    add("Accept: application/json"); add("Expect:");
    if (!request.bearer.empty()) add("Authorization: Bearer " + request.bearer);
    if (!request.contentType.empty()) add("Content-Type: " + request.contentType);
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(list, curl_slist_free_all);
    ResponseBuffer buffer{{}, request.responseLimit};
    auto set = [&](CURLoption option, auto value) {
        if (curl_easy_setopt(curl.get(), option, value) != CURLE_OK)
            throw NetworkError(NetworkFailure::Connection, "Could not configure HTTPS.");
    };
    set(CURLOPT_URL, request.url.c_str());
    set(CURLOPT_PROTOCOLS_STR, "https"); set(CURLOPT_REDIR_PROTOCOLS_STR, "https");
    set(CURLOPT_FOLLOWLOCATION, 0L); set(CURLOPT_MAXREDIRS, 0L);
    set(CURLOPT_SSL_VERIFYPEER, 1L); set(CURLOPT_SSL_VERIFYHOST, 2L);
    set(CURLOPT_SSLVERSION, static_cast<long>(CURL_SSLVERSION_TLSv1_2));
    set(CURLOPT_NOSIGNAL, 1L); set(CURLOPT_CONNECTTIMEOUT, 10L); set(CURLOPT_TIMEOUT, 60L);
    set(CURLOPT_HTTPHEADER, headers.get()); set(CURLOPT_USERAGENT, "air-Mail/0.1");
    set(CURLOPT_WRITEFUNCTION, &receive); set(CURLOPT_WRITEDATA, &buffer);
    set(CURLOPT_NOPROGRESS, 0L); set(CURLOPT_XFERINFOFUNCTION, &progress); set(CURLOPT_XFERINFODATA, &cancelled);
    if (!request.certificate.empty()) set(CURLOPT_CAINFO, request.certificate.c_str());
    const auto host = part(CURLUPART_HOST);
    if (host == "127.0.0.1" || host == "localhost" || host == "10.0.2.2" || host == "[::1]") set(CURLOPT_PROXY, "");
    if (request.method != "GET") {
        set(CURLOPT_CUSTOMREQUEST, request.method.c_str());
        set(CURLOPT_POSTFIELDS, request.body.data());
        set(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
    }
    auto result = curl_easy_perform(curl.get());
    long status = 0; curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    if (result != CURLE_OK) {
        curl_off_t uploaded = 0; curl_easy_getinfo(curl.get(), CURLINFO_SIZE_UPLOAD_T, &uploaded);
        if (request.delivery && uploaded > 0)
            throw NetworkError(NetworkFailure::DeliveryUnknown, "Gmail may have accepted this message. Check Sent before retrying.");
        if (cancelled) throw NetworkError(NetworkFailure::Cancelled, "The request was cancelled.");
        if (buffer.exceeded) throw NetworkError(NetworkFailure::Protocol, "The API response exceeds Mail's size limit.");
        if (buffer.failed) throw std::bad_alloc();
        if (result == CURLE_PEER_FAILED_VERIFICATION || result == CURLE_SSL_CACERT_BADFILE)
            throw NetworkError(NetworkFailure::Certificate, "The API certificate could not be verified.");
        // Never include the response body, URL query, or tokens in user-facing errors.
        throw NetworkError(NetworkFailure::Connection, std::string("HTTPS failed: ") + curl_easy_strerror(result));
    }
    return {status, std::move(buffer.body)};
}
void requireHttpSuccess(const HttpResponse& response, bool delivery)
{
    if (response.status >= 200 && response.status < 300) return;
    if (delivery && response.status >= 500)
        throw NetworkError(NetworkFailure::DeliveryUnknown, "Gmail could not confirm delivery. Check Sent before retrying.");
    std::string message = "The API rejected the request (HTTP " + std::to_string(response.status) + ").";
    if (response.status == 401) message = "Google authorization has expired. Sign in again.";
    if (response.status == 403) message = "Google denied this request. Check the account's permissions.";
    if (response.status == 429) message = "Google's request limit was reached. Try again later.";
    if (response.status >= 300 && response.status < 400) message = "The API returned an unexpected redirect.";
    throw HttpError(response.status, message);
}
std::string urlEncode(const std::string& value)
{
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~') result += c;
        else { result += '%'; result += hex[c >> 4]; result += hex[c & 15]; }
    }
    return result;
}
std::string base64UrlEncode(const std::string& data)
{
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string result; result.reserve((data.size() + 2) / 3 * 4);
    unsigned int bits = 0; int count = 0;
    for (unsigned char c : data) {
        bits = (bits << 8) | c; count += 8;
        while (count >= 6) { count -= 6; result += alphabet[(bits >> count) & 63]; }
    }
    if (count) result += alphabet[(bits << (6 - count)) & 63];
    return result;
}
std::string base64UrlDecode(const std::string& data, std::size_t limit)
{
    if (data.size() > ((limit + 2) / 3) * 4) throw NetworkError(NetworkFailure::Protocol, "The Gmail message exceeds Mail's size limit.");
    auto end = data.find('='); if (end == std::string::npos) end = data.size();
    if (data.size() - end > 2 || (end != data.size() && data.size() % 4) || end % 4 == 1)
        throw NetworkError(NetworkFailure::Protocol, "Invalid base64url data from Google.");
    for (auto i = end; i < data.size(); ++i) if (data[i] != '=') throw NetworkError(NetworkFailure::Protocol, "Invalid base64url padding.");
    if (end != data.size() && (4 - end % 4) % 4 != data.size() - end)
        throw NetworkError(NetworkFailure::Protocol, "Invalid base64url padding.");
    unsigned int bits = 0; int count = 0; std::string result; result.reserve(end / 4 * 3);
    for (std::size_t i = 0; i < end; ++i) {
        unsigned char c = data[i]; int value = -1;
        if (c >= 'A' && c <= 'Z') value = c - 'A'; else if (c >= 'a' && c <= 'z') value = c - 'a' + 26;
        else if (c >= '0' && c <= '9') value = c - '0' + 52; else if (c == '-') value = 62; else if (c == '_') value = 63;
        if (value < 0) throw NetworkError(NetworkFailure::Protocol, "Invalid base64url data from Google.");
        bits = (bits << 6) | static_cast<unsigned int>(value); count += 6;
        if (count >= 8) { count -= 8; result += static_cast<char>((bits >> count) & 255); }
    }
    if (result.size() > limit || (count && (bits & ((1u << count) - 1))))
        throw NetworkError(NetworkFailure::Protocol, "Invalid or oversized base64url data from Google.");
    return result;
}
}
