// Drives the real Interface Kit windows against a disposable OAuth/Gmail server.
#include "MailWindow.h"
#include "AccountWindow.h"
#include "ComposerWindow.h"
#include "Credentials.h"
#include "mail/Http.h"
#include "mail/Sync.h"
#include <Alert.h>
#include <Application.h>
#include <Autolock.h>
#include <Bitmap.h>
#include <BitmapStream.h>
#include <Button.h>
#include <CardLayout.h>
#include <CardView.h>
#include <File.h>
#include <ListView.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <MessageRunner.h>
#include <Screen.h>
#include <TextControl.h>
#include <TextView.h>
#include <TranslatorRoster.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <memory>
#include <thread>
#include <unistd.h>

namespace {
void check(bool ok,const char* text) { if (!ok) throw std::runtime_error(text); }
template<class T> T* view(BWindow* window,const char* name)
{
    auto* result=dynamic_cast<T*>(window->FindView(name)); check(result,name); return result;
}
std::size_t discard(char*,std::size_t size,std::size_t count,void*) { return size*count; }
std::size_t header(char* bytes,std::size_t size,std::size_t count,void* target)
{
    std::string line(bytes,size*count);
    if (line.compare(0,10,"Location: ")==0) {
        line=line.substr(10);
        while (!line.empty() && (line.back()=='\r' || line.back()=='\n')) line.pop_back();
        *static_cast<std::string*>(target)=line;
    }
    return size*count;
}
std::pair<long,std::string> browse(const std::string& url,const std::string& certificate)
{
    std::unique_ptr<CURL,decltype(&curl_easy_cleanup)> curl(curl_easy_init(),curl_easy_cleanup);
    check(bool(curl),"fixture browser initializes"); std::string location; long status=0;
    curl_easy_setopt(curl.get(),CURLOPT_URL,url.c_str());
    curl_easy_setopt(curl.get(),CURLOPT_CAINFO,certificate.c_str());
    curl_easy_setopt(curl.get(),CURLOPT_PROXY,"");
    curl_easy_setopt(curl.get(),CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(curl.get(),CURLOPT_TIMEOUT,5L);
    curl_easy_setopt(curl.get(),CURLOPT_PROTOCOLS_STR,"https,http");
    curl_easy_setopt(curl.get(),CURLOPT_WRITEFUNCTION,&discard);
    curl_easy_setopt(curl.get(),CURLOPT_HEADERFUNCTION,&header);
    curl_easy_setopt(curl.get(),CURLOPT_HEADERDATA,&location);
    check(curl_easy_perform(curl.get())==CURLE_OK,"fixture browser request succeeds");
    curl_easy_getinfo(curl.get(),CURLINFO_RESPONSE_CODE,&status); return {status,location};
}
struct Browser {
    std::atomic<bool> redirect{true};
    std::atomic<int> launches{0};
    std::thread job; std::exception_ptr error;
    ~Browser() { if (job.joinable()) job.join(); }
    void finish() { if (job.joinable()) job.join(); if (error) std::rethrow_exception(error); }
    void launch(const std::string& url,const std::string& certificate)
    {
        finish(); ++launches;
        if (!redirect) return;
        job=std::thread([this,url,certificate] {
            try {
                auto response=browse(url,certificate);
                check(response.first==302 && !response.second.empty(),"authorization redirects to callback");
                check(browse(response.second,certificate).first==200,"loopback callback accepts browser redirect");
            } catch (...) { error=std::current_exception(); }
        });
    }
};
void capture(const char* path)
{
    BBitmap* bitmap=nullptr; check(BScreen().GetBitmap(&bitmap)==B_OK,"capture Google account window");
    BBitmapStream stream(bitmap); BFile file(path,B_WRITE_ONLY|B_CREATE_FILE|B_ERASE_FILE);
    check(file.InitCheck()==B_OK,"open screenshot output");
    check(BTranslatorRoster::Default()->Translate(&stream,nullptr,nullptr,&file,B_PNG_FORMAT)==B_OK,"write screenshot");
}
class Google : public BApplication {
public:
    Google(std::string directory,char** args)
        : BApplication("application/x-vnd.air-Mail-Google-Test"),fDirectory(std::move(directory)),
          fBase("https://"+std::string(args[1])+":"+args[3]),fBrowser(std::make_shared<Browser>())
    {
        fOptions.client={"desktop","fixture-secret"};
        fOptions.endpoints={fBase+"/authorize",fBase+"/token",fBase+"/revoke",fBase+"/gmail",args[2]};
        fOptions.launchBrowser=[browser=fBrowser,certificate=std::string(args[2])](const std::string& url) { browser->launch(url,certificate); };
    }
    ~Google() override
    {
        // Stop any authorization before releasing the fixture browser's state.
        if (auto* accounts=window<mail::ui::AccountWindow>()) { if (accounts->Lock()) accounts->Quit(); }
    }
    int result=1;
    void ReadyToRun() override
    {
        try {
            fStore=std::make_shared<mail::Store>(fDirectory+"/mail.db");
            fMail=new mail::ui::MailWindow(fStore,fOptions); fMail->Show(); fMail->PostMessage('macc');
            BMessage tick('test'); fTimer=std::make_unique<BMessageRunner>(BMessenger(this),&tick,200000);
        } catch (const std::exception& error) { fail(error.what()); }
    }
    void MessageReceived(BMessage* message) override
    {
        if (message->what!='test') { BApplication::MessageReceived(message); return; }
        try { tick(); } catch (const std::exception& error) { fail(error.what()); }
    }
private:
    template<class T> T* window()
    {
        for (int32 i=0;i<CountWindows();++i) if (auto* found=dynamic_cast<T*>(WindowAt(i))) return found;
        return nullptr;
    }
    void fail(const char* text) { std::fprintf(stderr,"FAIL Google step %d: %s\n",fStep,text); fTimer.reset(); PostMessage(B_QUIT_REQUESTED); }
    void next(int step) { fStep=step; std::printf("Google UI step %d\n",step); std::fflush(stdout); }
    nlohmann::json fixture(const std::string& path,const std::string& phase={})
    {
        mail::HttpRequest request; request.url=fBase+path; request.certificate=fOptions.endpoints.certificate;
        if (!phase.empty()) { request.method="POST"; request.contentType="application/json"; request.body=nlohmann::json{{"phase",phase}}.dump(); }
        auto response=mail::requestHttps(request,fCancelled); mail::requireHttpSuccess(response); return nlohmann::json::parse(response.body);
    }
    bool idle(mail::ui::AccountWindow* accounts)
    {
        return std::string(view<BButton>(accounts,"google sign in")->Label())!="Cancel";
    }
    void profiles()
    {
        std::exception_ptr errors[2]; std::thread jobs[2];
        for (int i=0;i<2;++i) jobs[i]=std::thread([this,&errors,i] {
            try { auto api=mail::ui::ConnectGmail(fAccount,fCancelled,fOptions.endpoints); mail::verifyGmailIdentity(fAccount,api->profile()); }
            catch (...) { errors[i]=std::current_exception(); }
        });
        for (auto& job:jobs) job.join();
        for (auto& error:errors) if (error) std::rethrow_exception(error);
    }
    void tick()
    {
        check(++fTicks<750,"native Google test timed out");
        auto* accounts=window<mail::ui::AccountWindow>();
        if (fStep==0) {
            if (!accounts) return;
            BAutolock lock(accounts);
            view<BMenuField>(accounts,"provider")->Menu()->ItemAt(3)->SetMarked(true);
            view<BTextControl>(accounts,"account name")->SetText("Native Google");
            accounts->PostMessage('acPr'); next(1);
        } else if (fStep==1) {
            BAutolock lock(accounts);
            check(view<BCardView>(accounts,"provider settings")->CardLayout()->VisibleIndex()==1,"Google provider has its own settings panel");
            check(!view<BTextControl>(accounts,"email")->IsEnabled(),"Google email is supplied by verified profile");
            accounts->PostMessage('acGo'); next(2);
        } else if (fStep==2) {
            if (accounts->LockWithTimeout(0)!=B_OK) return;
            bool ready=idle(accounts) && view<BListView>(accounts,"accounts")->CountItems()==1;
            accounts->Unlock(); if (!ready) return; fBrowser->finish();
            for (const auto& a:fStore->accounts()) if (a.provider==mail::Provider::Gmail) fAccount=a;
            check(fAccount.id>0 && fAccount.email=="user@example.test","native Google sign-in saves verified profile");
            auto tokens=mail::ui::LoadGoogleTokens(fAccount);
            check(tokens.access=="valid-access" && tokens.refresh=="valid-refresh" && tokens.client.id=="desktop","tokens persist in system key store");
            tokens.expires=std::time(nullptr)-1; mail::ui::SaveGoogleTokens(fAccount,tokens); profiles();
            check(fixture("/fixture/auth-stats")["refreshes"]==1,"concurrent workers share one expiry refresh");
            tokens=mail::ui::LoadGoogleTokens(fAccount); check(tokens.access=="refreshed-access","refreshed token is persisted");
            tokens.access="expired-access"; tokens.expires=std::time(nullptr)+3600;
            mail::ui::SaveGoogleTokens(fAccount,tokens); profiles();
            check(fixture("/fixture/auth-stats")["refreshes"]==2,"concurrent rejected tokens share one forced refresh");
            next(3);
        } else if (fStep==3) {
            {
                BAutolock lock(accounts); auto* status=view<BTextView>(accounts,"google status");
                check(status->Bounds().Height()>=status->TextRect().bottom,"Google status displays all help lines without clipping");
            }
            if (const char* path=std::getenv("MAIL_GOOGLE_SCREENSHOT")) capture(path);
            fBrowser->redirect=false; accounts->PostMessage('acGo'); next(4);
        } else if (fStep==4) {
            BAutolock lock(accounts); if (idle(accounts) || fBrowser->launches<2) return;
            check(!view<BTextControl>(accounts,"account name")->IsEnabled(),"sign-in disables metadata edits");
            accounts->PostMessage(B_QUIT_REQUESTED); next(5);
        } else if (fStep==5) {
            if (accounts) return;
            check(mail::ui::LoadGoogleTokens(fAccount).access=="refreshed-access","closing during sign-in preserves existing credentials");
            if (fStore->gmailHistory(fAccount.id)!="100") return;
            auto a=fStore->findRemote(fAccount.id,"gmail:a"),b=fStore->findRemote(fAccount.id,"gmail:b");
            check(a && b && fStore->message(a).thread==fStore->message(b).thread,"closing Accounts starts Gmail receive with provider threading");
            fBrowser->redirect=true; fMail->PostMessage('macc'); next(6);
        } else if (fStep==6) {
            if (!accounts) { if (fTicks%5==0) fMail->PostMessage('macc'); return; }
            BAutolock lock(accounts);
            if (std::string(view<BTextControl>(accounts,"account name")->Text())!="Native Google") return;
            check(!view<BMenuField>(accounts,"provider")->IsEnabled(),"existing Google provider cannot be changed");
            view<BTextControl>(accounts,"account name")->SetText("Renamed Google"); accounts->PostMessage('acSv'); next(7);
        } else if (fStep==7) {
            bool renamed=false;
            for (const auto& a:fStore->accounts()) if (a.id==fAccount.id) renamed=a.name=="Renamed Google";
            if (!renamed) return;
            check(mail::ui::LoadGoogleTokens(fAccount).access=="refreshed-access","metadata edit preserves tokens");
            fixture("/fixture/stage","wrong-account"); accounts->PostMessage('acGo'); next(8);
        } else if (fStep==8) {
            BAutolock lock(accounts);
            if (!idle(accounts) || std::string(view<BTextView>(accounts,"google status")->Text()).find("different")==std::string::npos) return;
            fBrowser->finish(); check(mail::ui::LoadGoogleTokens(fAccount).access=="refreshed-access","wrong-identity reauthorization preserves original credential");
            fixture("/fixture/stage","full"); accounts->PostMessage('acGo'); next(9);
        } else if (fStep==9) {
            if (accounts->LockWithTimeout(0)!=B_OK) return;
            bool ready=idle(accounts) && fBrowser->launches>=4; accounts->Unlock(); if (!ready) return;
            fBrowser->finish(); if (mail::ui::LoadGoogleTokens(fAccount).access!="valid-access") return;
            accounts->PostMessage('acNw'); next(10);
        } else if (fStep==10) {
            BAutolock lock(accounts);
            if (!view<BMenuField>(accounts,"provider")->IsEnabled()) return;
            view<BMenuField>(accounts,"provider")->Menu()->ItemAt(3)->SetMarked(true);
            accounts->PostMessage('acPr'); accounts->PostMessage('acGo'); next(11);
        } else if (fStep==11) {
            if (accounts->LockWithTimeout(0)!=B_OK) return;
            bool ready=idle(accounts) && fBrowser->launches>=5 && view<BListView>(accounts,"accounts")->CountItems()==1;
            accounts->Unlock(); if (!ready) return; fBrowser->finish();
            int count=0; for (const auto& a:fStore->accounts()) if (a.provider==mail::Provider::Gmail) {
                ++count; check(a.id==fAccount.id && a.credentialId==fAccount.credentialId && a.name=="Renamed Google","adding the same identity reconnects existing account");
            }
            check(count==1,"duplicate Google identity is not added");
            accounts->PostMessage(B_QUIT_REQUESTED); next(12);
        } else if (fStep==12) {
            if (accounts) return;
            BAutolock lock(fMail);
            if (std::string(view<BButton>(fMail,"check mail")->Label())!="Check mail") return;
            auto* conversations=view<BListView>(fMail,"conversations"); check(conversations->CountItems()==1,"Gmail Inbox displays provider conversation");
            conversations->Select(0); fMail->PostMessage('msel'); fMail->PostMessage('mrpl'); next(13);
        } else if (fStep==13) {
            auto* composer=window<mail::ui::ComposerWindow>(); if (!composer) return;
            BAutolock lock(composer);
            view<BTextControl>(composer,"compose to")->SetText("recipient@example.test");
            view<BTextControl>(composer,"compose bcc")->SetText("hidden@example.test");
            view<BTextControl>(composer,"compose subject")->SetText("Delivered");
            view<BTextView>(composer,"compose body")->SetText("Native Gmail reply.\n");
            composer->PostMessage('csnd'); next(14);
        } else if (fStep==14) {
            fSent=fStore->findRemote(fAccount.id,"gmail:sent1");
            if (!fSent) { for (const auto& item:fStore->outbox()) check(item.state!="failed" && item.state!="uncertain",item.error.c_str()); return; }
            check(fStore->message(fSent).raw.find("hidden@example.test")==std::string::npos,"Gmail local Sent copy hides BCC");
            check(fStore->outbox().empty() && fStore->drafts().empty(),"Gmail Send finishes outbox and draft");
            fixture("/fixture/stage","sent");
            BAutolock lock(fMail); if (std::string(view<BButton>(fMail,"check mail")->Label())!="Check mail") return;
            fMail->PostMessage('mchk'); next(15);
        } else if (fStep==15) {
            if (fStore->gmailHistory(fAccount.id)!="700") return;
            check(fStore->findRemote(fAccount.id,"gmail:sent1")==fSent && fStore->thread(fStore->message(fSent).thread).size()==3,"history reuses the API Sent receipt without duplication");
            fMail->PostMessage('macc'); next(16);
        } else if (fStep==16) {
            if (!accounts) { if (fTicks%5==0) fMail->PostMessage('macc'); return; }
            // Let the initial list-selection notification load this account.
            BAutolock lock(accounts); if (view<BTextControl>(accounts,"email")->Text()!=fAccount.email) return;
            accounts->PostMessage('acRv'); next(17);
        } else if (fStep==17) {
            auto* alert=window<BAlert>(); if (!alert) return;
            BAutolock lock(alert); check(std::string(alert->Title())=="Revoke Google access","revocation has an explicit confirmation");
            alert->ButtonAt(1)->Invoke(); next(18);
        } else if (fStep==18) {
            if (accounts->LockWithTimeout(0)!=B_OK) return;
            bool revoked=idle(accounts) && std::string(view<BTextView>(accounts,"google status")->Text()).find("revoked")!=std::string::npos;
            accounts->Unlock(); if (!revoked) return;
            bool missing=false; try { mail::ui::LoadGoogleTokens(fAccount); } catch (const std::exception&) { missing=true; }
            check(missing && fStore->findRemote(fAccount.id,"gmail:sent1")==fSent,"revocation removes tokens and keeps cached mail");
            accounts->PostMessage('acGo'); next(19);
        } else if (fStep==19) {
            if (accounts->LockWithTimeout(0)!=B_OK) return;
            bool ready=idle(accounts) && fBrowser->launches>=6; accounts->Unlock(); if (!ready) return;
            fBrowser->finish(); check(mail::ui::LoadGoogleTokens(fAccount).access=="valid-access","revoked account can reconnect");
            for (const char* suffix:{"","-wal"}) {
                std::ifstream file(fDirectory+"/mail.db"+suffix,std::ios::binary);
                std::string bytes((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
                check(bytes.find("valid-refresh")==std::string::npos && bytes.find("fixture-secret")==std::string::npos,"SQLite files contain no OAuth credentials");
            }
            mail::ui::RemoveCredentials(fAccount); result=0; fTimer.reset();
            std::puts("Native Google passed: sign-in, key store, coordinated refresh, cancellation, identity, duplicate add, receive, reply/send, Sent reconciliation, revoke/reconnect");
            std::fflush(stdout); PostMessage(B_QUIT_REQUESTED); next(20);
        }
    }
    std::string fDirectory,fBase;
    std::shared_ptr<Browser> fBrowser;
    mail::ui::GoogleOptions fOptions;
    std::atomic<bool> fCancelled{false};
    std::shared_ptr<mail::Store> fStore;
    mail::ui::MailWindow* fMail=nullptr;
    mail::Account fAccount;
    mail::Id fSent=0;
    std::unique_ptr<BMessageRunner> fTimer;
    int fStep=0,fTicks=0;
};
}
int main(int argc,char** argv)
{
    if (argc!=4) return 2;
    char directory[]="/tmp/mail-native-google-XXXXXX"; if (!mkdtemp(directory)) return 2;
    int result; { Google app(directory,argv); app.Run(); result=app.result; }
    for (const char* file:{"mail.db","mail.db-wal","mail.db-shm"}) unlink((std::string(directory)+"/"+file).c_str());
    rmdir(directory); return result;
}
