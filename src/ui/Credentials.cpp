#include "Credentials.h"
#include <DataIO.h>
#include <Application.h>
#include <Entry.h>
#include <File.h>
#include <FindDirectory.h>
#include <KeyStore.h>
#include <Message.h>
#include <Path.h>
#include <Roster.h>
#include <Url.h>
#include <ctime>
#include <cstring>
#include <mutex>
#include <stdexcept>

namespace mail::ui {
namespace {
constexpr const char* kService="application/x-vnd.air-Mail/credentials";
std::mutex googleMutex;
void check(status_t status)
{
    if (status!=B_OK) throw std::runtime_error(std::string("Could not access the system key store: ")+strerror(status));
}
void validate(const Account& account)
{
    if (account.credentialId.size()!=32 || account.credentialId.find_first_not_of("0123456789abcdef")!=std::string::npos)
        throw std::runtime_error("This account has no valid credential identifier.");
}
}
Credentials LoadCredentials(const Account& account)
{
    validate(account);
    BKeyStore store; BKey key;
    auto status=store.GetKey(B_KEY_TYPE_GENERIC,kService,account.credentialId.c_str(),key);
    if (status==B_ENTRY_NOT_FOUND) throw std::runtime_error(account.provider==Provider::Gmail
        ? "Open Accounts and sign in to Google again for "+account.name+"."
        : "Open Accounts and enter the password for "+account.name+".");
    check(status);
    BMemoryIO data(key.Data(),key.DataLength()); BMessage message;
    check(message.Unflatten(&data));
    const char* incoming=nullptr; const char* outgoing=nullptr;
    check(message.FindString("incoming",&incoming)); check(message.FindString("outgoing",&outgoing));
    return {incoming,outgoing};
}
void SaveCredentials(const Account& account,const Credentials& credentials)
{
    validate(account);
    BMessage message; check(message.AddString("incoming",credentials.incoming.c_str()));
    check(message.AddString("outgoing",credentials.outgoing.c_str()));
    BMallocIO data; check(message.Flatten(&data));
    BKey replacement; check(replacement.SetTo(B_KEY_PURPOSE_NETWORK,kService,account.credentialId.c_str(),
        static_cast<const uint8*>(data.Buffer()),data.BufferLength()));
    BKeyStore store; BKey previous;
    auto status=store.GetKey(B_KEY_TYPE_GENERIC,kService,account.credentialId.c_str(),previous);
    if (status==B_ENTRY_NOT_FOUND) { check(store.AddKey(replacement)); return; }
    check(status);
    if (previous.DataLength()==data.BufferLength() && std::memcmp(previous.Data(),data.Buffer(),data.BufferLength())==0) return;
    // Haiku has no atomic replacement API. Keep a copy until replacement is
    // confirmed, and preserve the previous key on a normal write failure.
    check(store.RemoveKey(previous));
    status=store.AddKey(replacement);
    if (status==B_OK) return;
    auto restored=store.AddKey(previous);
    if (restored!=B_OK) throw std::runtime_error("The key store could not save or restore this account's credentials. Open Accounts to reconnect.");
    check(status);
}
void RemoveCredentials(const Account& account)
{
    validate(account);
    BKeyStore store; BKey key;
    auto status=store.GetKey(B_KEY_TYPE_GENERIC,kService,account.credentialId.c_str(),key);
    if (status==B_ENTRY_NOT_FOUND) return;
    check(status); check(store.RemoveKey(key));
}
GoogleClient ReadGoogleClient()
{
    std::vector<std::string> candidates;
    BPath settings;
    if (find_directory(B_USER_SETTINGS_DIRECTORY,&settings)==B_OK) {
        settings.Append("Mail/google-client.json"); candidates.emplace_back(settings.Path());
    }
    app_info info;
    if (be_app && be_app->GetAppInfo(&info)==B_OK) {
        BEntry executable(&info.ref),parent; BPath path;
        if (executable.GetParent(&parent)==B_OK && parent.GetPath(&path)==B_OK) {
            // A development build keeps it beside Mail; the package in data/mail.
            BPath installed(path); installed.Append("../data/mail/google-client.json",true);
            path.Append("Mail.google-client.json"); candidates.emplace_back(path.Path());
            if (installed.InitCheck()==B_OK) candidates.emplace_back(installed.Path());
        }
    }
    for (const auto& path : candidates) {
        BFile file(path.c_str(),B_READ_ONLY);
        if (file.InitCheck()==B_ENTRY_NOT_FOUND) continue;
        check(file.InitCheck()); off_t size=0; check(file.GetSize(&size));
        if (size<=0 || size>65536) throw std::runtime_error("The Google desktop client file is empty or too large.");
        std::string text(static_cast<std::size_t>(size),'\0');
        if (file.Read(text.data(),text.size())!=size) throw std::runtime_error("Could not read the Google desktop client file.");
        return parseGoogleClient(text);
    }
    throw std::runtime_error("Google sign-in is not configured in this build. Put the Google Desktop app client JSON in ~/config/settings/Mail/google-client.json, or install it as data/mail/google-client.json in Mail's package. See Mail's Google setup instructions.");
}
void OpenGoogleBrowser(const std::string& url)
{
    auto status=BUrl(url.c_str(),false).OpenWithPreferredApplication(false);
    if (status!=B_OK) throw std::runtime_error("Could not open the system browser. Set a preferred web browser and try signing in again.");
}
GoogleTokens LoadGoogleTokens(const Account& account)
{
    std::lock_guard<std::mutex> guard(googleMutex);
    if (account.provider!=Provider::Gmail) throw std::invalid_argument("This is not a Google account.");
    return parseGoogleTokens(LoadCredentials(account).incoming);
}
void SaveGoogleTokens(const Account& account,const GoogleTokens& tokens)
{
    std::lock_guard<std::mutex> guard(googleMutex);
    if (account.provider!=Provider::Gmail) throw std::invalid_argument("This is not a Google account.");
    SaveCredentials(account,{serializeGoogleTokens(tokens),"google-oauth-v1"});
}
std::unique_ptr<GmailApi> ConnectGmail(const Account& account,const std::atomic<bool>& cancelled,const GoogleEndpoints& endpoints)
{
    if (account.provider!=Provider::Gmail) throw std::invalid_argument("This is not a Google account.");
    return std::make_unique<GmailApi>([account,&cancelled,endpoints,last=std::string{}](bool force) mutable {
        std::lock_guard<std::mutex> guard(googleMutex);
        if (cancelled) throw NetworkError(NetworkFailure::Cancelled,"Google access was cancelled.");
        auto tokens=parseGoogleTokens(LoadCredentials(account).incoming);
        // Another worker may already have replaced the token rejected by this
        // API instance. Reuse that replacement instead of refreshing it again.
        if (tokens.expires<=std::time(nullptr)+60 || (force && tokens.access==last)) {
            tokens=refreshGoogle(tokens,cancelled,endpoints);
            SaveCredentials(account,{serializeGoogleTokens(tokens),"google-oauth-v1"});
        }
        last=tokens.access; return last;
    },cancelled,endpoints);
}
void RevokeGoogleAccess(const Account& account,const std::atomic<bool>& cancelled,const GoogleEndpoints& endpoints)
{
    std::lock_guard<std::mutex> guard(googleMutex);
    if (account.provider!=Provider::Gmail) throw std::invalid_argument("This is not a Google account.");
    if (cancelled) throw NetworkError(NetworkFailure::Cancelled,"Google access was cancelled.");
    auto tokens=parseGoogleTokens(LoadCredentials(account).incoming);
    revokeGoogle(tokens,cancelled,endpoints);
    RemoveCredentials(account);
}
}
