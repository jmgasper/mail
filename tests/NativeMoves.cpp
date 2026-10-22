// Exercise the actual review window against the disposable TLS move fixture.
#include "MailWindow.h"
#include "MoveWindow.h"
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
#include <unistd.h>
namespace {
int checks=0;
void check(bool value,const char* text) { ++checks; if (!value) throw std::runtime_error(text); }
template<class T> T* view(BWindow* window,const char* name) { auto* v=dynamic_cast<T*>(window->FindView(name)); check(v,name); return v; }
class Test : public BApplication {
public:
    Test(const std::string& dir,char** args):BApplication("application/x-vnd.air-Mail-Moves-Test"),directory(dir),args(args) {}
    int result=1;
    void ReadyToRun() override
    {
        try {
            store=std::make_shared<mail::Store>(directory+"/mail.db");
            std::puts("Approve disposable move fixture key-store prompts if shown."); std::fflush(stdout);
            for (const std::string name:{"retry_duplicate","keep_local","queued_cancel"}) {
                mail::Account a; a.name=name; a.email="review@example.test"; a.provider=mail::Provider::Imap;
                a.incoming={args[1],std::stoi(args[3]),mail::Security::TLS,name,args[2]}; a.outgoing=a.incoming;
                auto id=store->saveAccount(a); for (const auto& saved:store->accounts()) if (saved.id==id) a=saved;
                mail::ui::SaveCredentials(a,{"test-password","test-password"}); accounts.push_back(a);
            }
            main=new mail::ui::MailWindow(store); main->Show();
            BMessage tick('test'); timer=std::make_unique<BMessageRunner>(BMessenger(this),&tick,200000);
        } catch (const std::exception& e) { fail(e.what()); }
    }
    void MessageReceived(BMessage* m) override
    {
        if (m->what!='test') { BApplication::MessageReceived(m); return; }
        try { check(++ticks<700,"native move test timeout"); step(); } catch (const std::exception& e) { fail(e.what()); }
    }
private:
    template<class T> T* window() { for (int32 i=0;i<CountWindows();++i) if (auto* w=dynamic_cast<T*>(WindowAt(i))) return w; return nullptr; }
    void fail(const char* text) { std::fprintf(stderr,"FAIL move stage %d: %s\n",stage,text); timer.reset(); PostMessage(B_QUIT_REQUESTED); }
    void next() { ++stage; std::printf("Move stage %d\n",stage); std::fflush(stdout); }
    bool idle() { BAutolock lock(main); return std::string(view<BButton>(main,"check mail")->Label())=="Check mail"; }
    void refresh() { window<mail::ui::MoveWindow>()->PostMessage('mvrf'); main->PostMessage(mail::ui::kMovesChanged); }
    void press(const char* name) { auto* w=window<mail::ui::MoveWindow>(); BAutolock lock(w); auto* b=view<BButton>(w,name); check(b->IsEnabled(),"move action enabled"); b->Invoke(); }
    bool confirm(bool accepted,bool retry)
    {
        auto* alert=window<BAlert>(); if (!alert) return false;
        BAutolock lock(alert);
        check(std::string(alert->Title())==(retry?"Folder copy unconfirmed":"Keep local copy"),"correct move confirmation");
        check(alert->DefaultButton()==alert->ButtonAt(0),"move confirmation defaults to Cancel");
        auto text=std::string(alert->TextView()->Text());
        check(retry?(text.find("duplicate")!=std::string::npos && text.find("does not send mail")!=std::string::npos)
            :text.find("No server messages will be removed")!=std::string::npos,"move confirmation explains its effect");
        alert->ButtonAt(accepted?1:0)->Invoke(); return true;
    }
    void step()
    {
        auto reviews=store->moveReviews(); auto* moves=window<mail::ui::MoveWindow>();
        switch(stage) {
            case 0:
                if (!idle()) return;
                ids.clear();
                for (const auto& account:accounts) {
                    auto id=store->findRemote(account.id,mail::imapRemoteKey({"INBOX","41","1"})); if (!id) return;
                    ids.push_back(id);
                }
                for (auto id:ids) { store->tag(id,"Keep",true); store->move(id,"archive"); }
                main->PostMessage(mail::ui::kMovesChanged); main->PostMessage(mail::ui::kCheckMoves); next(); break;
            case 1:
                if (!idle() || reviews.size()!=3) return;
                for (const auto& review:reviews) if (review.active || review.error.empty()) return;
                main->PostMessage(mail::ui::kFolderMoves); next(); break;
            case 2: {
                if (!moves) return;
                BAutolock lock(moves);
                check(view<BListView>(moves,"move list")->CountItems()==3,"review shows interrupted and queued moves");
                auto text=std::string(view<BTextView>(moves,"move details")->Text());
                check(text.find("From: INBOX")!=std::string::npos && text.find("To: Archive")!=std::string::npos && text.find("retry_duplicate")!=std::string::npos,"review identifies account and both folders");
                { BAutolock mainLock(main); check(std::string(view<BButton>(main,"folder moves")->Label())=="Folder moves (3)","main window exposes pending move count"); }
                BBitmap* bitmap=nullptr; check(BScreen().GetBitmap(&bitmap)==B_OK,"capture move review");
                BBitmapStream stream(bitmap); BFile file("/boot/home/mail-check/moves.png",B_WRITE_ONLY|B_CREATE_FILE|B_ERASE_FILE);
                check(BTranslatorRoster::Default()->Translate(&stream,nullptr,nullptr,&file,B_PNG_FORMAT)==B_OK,"save move capture");
                press("retry move"); next(); break;
            }
            case 3: if (confirm(false,true)) next(); break;
            case 4:
                if (window<BAlert>()) return;
                check(reviews[0].remote.stage=="started","Cancel preserves uncertain move");
                check(store->claimMove(reviews[0]),"claim move before stale UI actions");
                moves->PostMessage('mvrt'); moves->PostMessage('mvkp'); next(); break;
            case 5: {
                check(reviews[0].active && !window<BAlert>(),"stale UI actions cannot alter active move");
                BAutolock lock(moves);
                check(!view<BButton>(moves,"retry move")->IsEnabled() && !view<BButton>(moves,"keep move")->IsEnabled(),"active move controls are disabled");
                store->releaseMove(reviews[0].change,"Fixture review before explicit retry"); refresh(); next(); break;
            }
            case 6: press("retry move"); next(); break;
            case 7: if (confirm(true,true)) next(); break;
            case 8:
                if (window<BAlert>() || !idle() || reviews.size()!=3 || reviews[0].active || reviews[0].remote.firstUid!="8" || reviews[0].remote.stage!="started") return;
                check(store->outbox().empty(),"explicit folder copy creates no SMTP work"); refresh(); next(); break;
            case 9: press("check move"); next(); break;
            case 10:
                if (!idle() || reviews.size()!=2) return;
                check(store->findRemote(accounts[0].id,mail::imapRemoteKey({"Archive","77","8"}))==ids[0],"Check server recovers only the explicitly retried copy");
                refresh(); next(); break;
            case 11: {
                BAutolock lock(moves); if (view<BListView>(moves,"move list")->CountItems()!=2) return;
                check(reviews[0].change.message==ids[1],"selection follows the remaining move");
                store->move(ids[1],"trash"); store->setStarred(ids[1],true); press("keep move"); next(); break;
            }
            case 12: if (confirm(false,false)) next(); break;
            case 13:
                if (window<BAlert>()) return;
                check(reviews.size()==2,"Cancel keeps unresolved journal"); press("keep move"); next(); break;
            case 14: if (confirm(true,false)) next(); break;
            case 15:
                if (window<BAlert>() || reviews.size()!=1) return;
                check(store->message(ids[1]).folder=="trash" && store->message(ids[1]).starred && store->message(ids[1]).tags==std::vector<std::string>{"Keep"},"keep local retains latest folder, flags and tags");
                main->PostMessage(mail::ui::kCheckMoves); next(); break;
            case 16: {
                if (!idle() || reviews.size()!=1) return;
                BAutolock lock(moves); if (view<BListView>(moves,"move list")->CountItems()!=1) return;
                check(std::string(view<BButton>(moves,"keep move")->Label())=="Cancel move","unstarted move offers cancellation");
                press("keep move"); next(); break;
            }
            case 17: {
                if (!reviews.empty()) return;
                BAutolock lock(moves); if (view<BListView>(moves,"move list")->CountItems()) return;
                check(!view<BButton>(moves,"retry move")->IsEnabled() && !view<BButton>(moves,"keep move")->IsEnabled(),"empty review disables actions");
                check(store->message(ids[2]).folder=="inbox","queued cancellation restores source folder");
                check(store->outbox().empty() && store->drafts().empty(),"review never sends mail or creates drafts");
                { BAutolock mainLock(main); check(std::string(view<BButton>(main,"folder moves")->Label())=="Folder moves (0)","completed review clears main count"); }
                for (const auto& account:accounts) mail::ui::RemoveCredentials(account);
                timer.reset(); result=0; std::printf("Native move review passed (%d checks): Cancel, explicit retry, active guards, recovery, keep local, queued cancel, no sending\n",checks); std::fflush(stdout);
                PostMessage(B_QUIT_REQUESTED); next(); break;
            }
        }
    }
    std::string directory; char** args; std::shared_ptr<mail::Store> store; std::vector<mail::Account> accounts;
    std::vector<mail::Id> ids; mail::ui::MailWindow* main=nullptr; std::unique_ptr<BMessageRunner> timer;
    int stage=0,ticks=0;
};
}
int main(int argc,char** argv)
{
    if (argc!=9) return 2;
    char directory[]="/tmp/mail-native-moves-XXXXXX"; if (!mkdtemp(directory)) return 2;
    int result; { Test app(directory,argv); app.Run(); result=app.result; }
    for (const char* name:{"mail.db-wal","mail.db-shm","mail.db"}) unlink((std::string(directory)+"/"+name).c_str());
    rmdir(directory); return result;
}
