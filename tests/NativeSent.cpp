// Native recovery controls against a disposable server that loses APPEND receipts.
#include "MailWindow.h"
#include "OutboxWindow.h"
#include "Credentials.h"
#include <Alert.h>
#include <Application.h>
#include <Autolock.h>
#include <Bitmap.h>
#include <BitmapStream.h>
#include <Button.h>
#include <File.h>
#include <ListView.h>
#include <MessageRunner.h>
#include <Screen.h>
#include <TextView.h>
#include <TranslatorRoster.h>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <unistd.h>
namespace {
int checks=0;
void check(bool value,const char* text) { ++checks; if (!value) throw std::runtime_error(text); }
template<class T> T* view(BWindow* window,const char* name) { auto* v=dynamic_cast<T*>(window->FindView(name)); check(v,name); return v; }
class Test : public BApplication {
public:
    Test(const std::string& dir,char** args):BApplication("application/x-vnd.air-Mail-Sent-Test"),directory(dir),args(args) {}
    int result=1;
    void ReadyToRun() override
    {
        try {
            store=std::make_shared<mail::Store>(directory+"/mail.db");
            account.name="Sent recovery fixture"; account.email="user@example.test"; account.provider=mail::Provider::Imap;
            account.incoming={args[1],std::stoi(args[3]),mail::Security::TLS,"native",args[2]};
            account.outgoing={args[1],std::stoi(args[7]),mail::Security::TLS,"native",args[2]};
            auto id=store->saveAccount(account); for (const auto& a:store->accounts()) if (a.id==id) account=a;
            std::puts("Approve disposable Sent test key-store prompts if shown."); std::fflush(stdout);
            mail::ui::SaveCredentials(account,{"test-password","test-password"});
            mail::Draft d; d.account=account.id; d.to="recipient@example.test"; d.bcc="hidden@example.test";
            d.subject="Delivered — Sent copy unconfirmed"; d.text="Native Sent copy recovery 🎉";
            store->enqueueDraft(store->saveDraft(d),std::time(nullptr));
            main=new mail::ui::MailWindow(store); main->Show();
            BMessage tick('test'); timer=std::make_unique<BMessageRunner>(BMessenger(this),&tick,200000);
        } catch (const std::exception& e) { fail(e.what()); }
    }
    void MessageReceived(BMessage* m) override
    {
        if (m->what!='test') { BApplication::MessageReceived(m); return; }
        try { check(++ticks<700,"native Sent test timeout"); step(); } catch (const std::exception& e) { fail(e.what()); }
    }
private:
    template<class T> T* window() { for (int32 i=0;i<CountWindows();++i) if (auto* w=dynamic_cast<T*>(WindowAt(i))) return w; return nullptr; }
    void fail(const char* text) { std::fprintf(stderr,"FAIL Sent stage %d: %s\n",stage,text); timer.reset(); PostMessage(B_QUIT_REQUESTED); }
    void next() { ++stage; std::printf("Sent stage %d\n",stage); std::fflush(stdout); }
    bool idle() { BAutolock lock(main); return std::string(view<BButton>(main,"check mail")->Label())=="Check mail"; }
    void refresh() { window<mail::ui::OutboxWindow>()->PostMessage('orfh'); main->PostMessage(mail::ui::kOutboxChanged); }
    void press(const char* name) { auto* w=window<mail::ui::OutboxWindow>(); BAutolock lock(w); auto* b=view<BButton>(w,name); check(b->IsEnabled(),"copy action enabled"); b->Invoke(); }
    bool confirm(bool accepted)
    {
        auto* alert=window<BAlert>(); if (!alert) return false;
        BAutolock lock(alert); check(std::string(alert->Title())=="Sent copy unconfirmed","copy confirmation is separate from delivery");
        check(alert->DefaultButton()==alert->ButtonAt(0),"copy retry defaults to Cancel");
        auto text=std::string(alert->TextView()->Text()); check(text.find("will not send")!=std::string::npos && text.find("second copy")!=std::string::npos,"copy confirmation explains duplication without resending");
        alert->ButtonAt(accepted?1:0)->Invoke(); return true;
    }
    void step()
    {
        auto copies=store->sentCopies(); auto* outbox=window<mail::ui::OutboxWindow>();
        switch(stage) {
            case 0:
                if (!idle() || copies.size()!=1 || copies[0].state!="uncertain") return;
                id=copies[0].id; check(store->outbox().empty(),"delivery complete despite failed copy receipt");
                main->PostMessage('mout'); next(); break;
            case 1: {
                if (!outbox) return;
                BAutolock lock(outbox); auto* list=view<BListView>(outbox,"outbox list");
                check(list->CountItems()==1,"one Sent copy recovery entry");
                check(std::string(view<BTextView>(outbox,"delivery details")->Text()).find("Delivery to recipients is complete")!=std::string::npos,"details distinguish delivered message");
                check(std::string(view<BButton>(outbox,"return to draft")->Label())=="Keep local copy","Sent entry cannot return to draft");
                { BAutolock mainLock(main); check(std::string(view<BButton>(main,"outbox")->Label())=="Outbox (0) · 1 Sent copy","persistent count distinguishes delivery and copy work"); }
                BBitmap* bitmap=nullptr; check(BScreen().GetBitmap(&bitmap)==B_OK,"capture Sent recovery");
                BBitmapStream stream(bitmap); BFile file("/boot/home/mail-check/sent.png",B_WRITE_ONLY|B_CREATE_FILE|B_ERASE_FILE);
                check(BTranslatorRoster::Default()->Translate(&stream,nullptr,nullptr,&file,B_PNG_FORMAT)==B_OK,"save Sent capture");
                press("retry delivery"); next(); break;
            }
            case 2: if (confirm(false)) next(); break;
            case 3:
                if (window<BAlert>()) return;
                check(copies[0].state=="uncertain" && !copies[0].mayAppend,"Cancel preserves uncertain copy");
                store->retrySentCopy(id,true); check(store->claimSentCopy(id).id==id,"claim copy before stale UI action");
                outbox->PostMessage('oedt'); outbox->PostMessage('otry'); next(); break;
            case 4: {
                check(copies[0].state=="checking" && !window<BAlert>(),"stale copy actions cannot mutate an active check");
                BAutolock lock(outbox); check(!view<BButton>(outbox,"retry delivery")->IsEnabled() && !view<BButton>(outbox,"return to draft")->IsEnabled(),"active copy controls disabled");
                store->releaseSentCopy(id,"Fixture review before explicit retry",false,false); refresh(); next(); break;
            }
            case 5: press("retry delivery"); next(); break;
            case 6: if (confirm(true)) next(); break;
            case 7:
                if (window<BAlert>() || !idle() || copies.empty() || copies[0].state!="uncertain" || copies[0].error.find("Fixture")!=std::string::npos) return;
                check(store->outbox().empty() && store->drafts().empty(),"explicit copy retry does not create SMTP/draft work");
                refresh(); next(); break;
            case 8: press("return to draft"); next(); break;
            case 9: {
                if (!copies.empty()) return;
                check(!store->message(id).raw.empty() && store->message(id).folder=="sent","keep local retains delivered Sent message");
                BAutolock lock(outbox); if (view<BListView>(outbox,"outbox list")->CountItems()!=0) return;
                check(!view<BButton>(outbox,"retry delivery")->IsEnabled(),"empty copy queue disables retry");
                mail::ui::RemoveCredentials(account); timer.reset(); result=0;
                std::printf("Native Sent recovery passed (%d checks): delivered/copy distinction, Cancel and explicit retry, active check guards, keep local, no SMTP resend\n",checks); std::fflush(stdout);
                PostMessage(B_QUIT_REQUESTED); next(); break;
            }
        }
    }
    std::string directory; char** args; std::shared_ptr<mail::Store> store; mail::Account account;
    mail::ui::MailWindow* main=nullptr; std::unique_ptr<BMessageRunner> timer;
    int stage=0,ticks=0; mail::Id id=0;
};
}
int main(int argc,char** argv)
{
    if (argc!=9) return 2;
    char directory[]="/tmp/mail-native-sent-XXXXXX"; if (!mkdtemp(directory)) return 2;
    int result; { Test app(directory,argv); app.Run(); result=app.result; }
    for (const char* name:{"mail.db-wal","mail.db-shm","mail.db"}) unlink((std::string(directory)+"/"+name).c_str());
    rmdir(directory); return result;
}
