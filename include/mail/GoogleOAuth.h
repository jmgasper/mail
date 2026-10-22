#pragma once
#include "Http.h"
#include <functional>

namespace mail {
inline constexpr const char* kGoogleMailScope = "https://www.googleapis.com/auth/gmail.modify";
struct GoogleEndpoints {
    std::string authorize = "https://accounts.google.com/o/oauth2/v2/auth";
    std::string token = "https://oauth2.googleapis.com/token";
    std::string revoke = "https://oauth2.googleapis.com/revoke";
    std::string gmail = "https://gmail.googleapis.com/gmail/v1/users/me";
    std::string certificate;
};
struct GoogleClient { std::string id; std::string secret; };
struct GoogleTokens {
    GoogleClient client;
    std::string access;
    std::string refresh;
    Timestamp expires = 0;
    std::string scope;
};
// Only the "installed" desktop application format is accepted. Endpoint values
// in a downloaded client file are ignored; production always uses Google's URLs.
GoogleClient parseGoogleClient(const std::string& json);
std::string serializeGoogleTokens(const GoogleTokens&);
GoogleTokens parseGoogleTokens(const std::string&);
GoogleTokens authorizeGoogle(const GoogleClient&, const std::function<void(const std::string&)>& launchBrowser,
    const std::atomic<bool>& cancelled, const std::string& loginHint = {},
    const GoogleEndpoints& = {}, int timeoutSeconds = 300);
GoogleTokens refreshGoogle(const GoogleTokens&, const std::atomic<bool>& cancelled, const GoogleEndpoints& = {});
void revokeGoogle(const GoogleTokens&, const std::atomic<bool>& cancelled, const GoogleEndpoints& = {});
}
