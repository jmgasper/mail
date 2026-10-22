#pragma once
#include "Provider.h"

namespace mail {
struct HttpRequest {
    std::string url;
    std::string method = "GET";
    std::string contentType;
    std::string body;
    std::string bearer;
    std::string certificate; // Explicit CA for test fixtures; empty uses system trust.
    std::size_t responseLimit = 48 * 1024 * 1024;
    bool delivery = false; // An interrupted upload must never be retried automatically.
};
struct HttpResponse { long status = 0; std::string body; };
class HttpError : public NetworkError {
public:
    HttpError(long status, const std::string& message);
    long status;
};
// HTTPS only. Redirects are rejected so credentials cannot escape to another host.
HttpResponse requestHttps(const HttpRequest&, const std::atomic<bool>& cancelled);
void requireHttpSuccess(const HttpResponse&, bool delivery = false);
std::string urlEncode(const std::string&);
std::string base64UrlEncode(const std::string&);
std::string base64UrlDecode(const std::string&, std::size_t limit = 32 * 1024 * 1024);
}
