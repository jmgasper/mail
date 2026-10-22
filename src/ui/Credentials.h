#pragma once
#include "mail/Gmail.h"

namespace mail::ui {
struct Credentials { std::string incoming; std::string outgoing; };
Credentials LoadCredentials(const Account&);
void SaveCredentials(const Account&,const Credentials&);
void RemoveCredentials(const Account&);
// Production uses Google's fixed endpoints and the system browser. Tests can
// supply an isolated HTTPS fixture without persisting endpoint overrides.
struct GoogleOptions {
    GoogleClient client;
    GoogleEndpoints endpoints;
    std::function<void(const std::string&)> launchBrowser;
};
GoogleClient ReadGoogleClient();
void OpenGoogleBrowser(const std::string&);
GoogleTokens LoadGoogleTokens(const Account&);
void SaveGoogleTokens(const Account&,const GoogleTokens&);
std::unique_ptr<GmailApi> ConnectGmail(const Account&,const std::atomic<bool>& cancelled,const GoogleEndpoints& = {});
void RevokeGoogleAccess(const Account&,const std::atomic<bool>& cancelled,const GoogleEndpoints& = {});
}
