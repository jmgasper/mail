#include "MailWindow.h"
#include "SnoozeWindow.h"
#include "ReminderWindow.h"
#include "mail/Mime.h"
#include "mail/Snooze.h"
#include <Application.h>
#include <Autolock.h>
#include <Bitmap.h>
#include <BitmapStream.h>
#include <Button.h>
#include <CheckBox.h>
#include <File.h>
#include <ListView.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <MessageRunner.h>
#include <Screen.h>
#include <TextControl.h>
#include <TextView.h>
#include <TranslatorRoster.h>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <unistd.h>
using namespace mail;
namespace {
void check(bool value,const char* why) { if(!value) throw std::runtime_error(why); }
template<class T> T* view(BWindow* w,const char* name) { auto* result=dynamic_cast<T*>(w->FindView(name)); check(result,name); return result; }
void capture(const char* path)
{
    BBitmap* bitmap=nullptr; check(BScreen().GetBitmap(&bitmap)==B_OK,"screen capture"); BBitmapStream stream(bitmap); BFile file(path,B_WRITE_ONLY|B_CREATE_FILE|B_ERASE_FILE);
    check(BTranslatorRoster::Default()->Translate(&stream,nullptr,nullptr,&file,B_PNG_FORMAT)==B_OK,"save screenshot");
}
class Test : public BApplication {
public:
    explicit Test(std::string path):BApplication("application/x-vnd.air-Mail-Snooze-Test"),database(std::move(path)) {}
    int result=1;
    void ReadyToRun() override
    {
        try {
            store=std::make_shared<Store>(database); auto rows=store->reminders(std::time(nullptr)); check(rows.size()==2,"separate process snoozes expired while Mail was closed");
            message=rows[0].message; other=rows[1].message; account=rows[0].account;
            main=new ui::MailWindow(store,{},database); main->Show();
            BMessage tick('test'); timer=std::make_unique<BMessageRunner>(BMessenger(this),&tick,200000);
        } catch(const std::exception& e) { fail(e.what()); }
    }
    void ArgvReceived(int32 argc,char** argv) override
    {
        std::printf("Snooze application received %d arguments\n",static_cast<int>(argc)); std::fflush(stdout);
        if(argc==4 && std::string(argv[1])=="--reminders" && std::string(argv[2])=="--database" && argv[3]==database) {
            clicked=true; if(main) main->PostMessage(ui::kReminders);
        }
    }
    void MessageReceived(BMessage* msg) override
    {
        if(msg->what!='test') { BApplication::MessageReceived(msg); return; }
        try { check(++ticks<600,"native snooze timed out"); step(); } catch(const std::exception& e) { fail(e.what()); }
    }
private:
    template<class T> T* window() { for(int32 i=0;i<CountWindows();++i) if(auto* w=dynamic_cast<T*>(WindowAt(i))) return w; return nullptr; }
    void fail(const char* why) { std::fprintf(stderr,"FAIL snooze stage %d: %s\n",stage,why); timer.reset(); PostMessage(B_QUIT_REQUESTED); }
    void next() { std::printf("Snooze stage %d\n",++stage); std::fflush(stdout); }
    void press(BWindow* w,const char* name) { check(w,"window exists"); BAutolock lock(w); auto* button=view<BButton>(w,name); check(button->IsEnabled(),name); button->Invoke(); }
    void custom(ui::SnoozeWindow* w)
    {
        BAutolock lock(w); auto* item=view<BMenuField>(w,"snooze when")->Menu()->ItemAt(3); item->SetMarked(true);
        check(static_cast<BInvoker*>(item)->Invoke()==B_OK,"custom menu invokes");
    }
    void fields(BWindow* w,const std::string& date,const char* time)
    { BAutolock lock(w); view<BTextControl>(w,"snooze date")->SetText(date.c_str()); view<BTextControl>(w,"snooze time")->SetText(time); }
    std::string sunday(int month)
    {
        auto current=std::time(nullptr); std::tm tm{}; localtime_r(&current,&tm); int year=tm.tm_year+1901;
        char text[32]; std::snprintf(text,sizeof(text),"%04d-%02d-01",year,month); auto value=localSnoozeTimes(text,"12:00")[0]; auto time=static_cast<std::time_t>(value); localtime_r(&time,&tm);
        std::snprintf(text,sizeof(text),"%04d-%02d-%02d",year,month,1+(7-tm.tm_wday)%7); return text;
    }
    void step()
    {
        auto* reminders=window<ui::ReminderWindow>(); auto* snooze=window<ui::SnoozeWindow>();
        switch(stage) {
            case 0: {
                auto rows=store->reminders(std::time(nullptr)); if(!rows[0].notified || !rows[1].notified) return;
                BAutolock lock(main); check(std::string(view<BButton>(main,"reminders")->Label())=="Reminders (2)","persistent indicator includes opted-out account"); next(); break;
            }
            case 1: {
                capture("/boot/home/mail-check/snooze-notification.png");
                BMessenger server("application/x-vnd.Haiku-notification_server"); BMessage request(B_GET_PROPERTY),reply;
                request.AddSpecifier("Frame"); request.AddSpecifier("Window",int32(0));
                check(server.SendMessage(&request,&reply,5000000,5000000)==B_OK,"inspect actual system notification window");
                BRect frame; check(reply.FindRect("result",&frame)==B_OK && frame.IsValid(),"notification bounds"); auto screen=BScreen().Frame();
                FILE* file=std::fopen("/boot/home/mail-check/snooze-click.json","w"); check(file,"write click coordinates");
                std::fprintf(file,"{\"x\":%.0f,\"y\":%.0f,\"width\":%.0f,\"height\":%.0f}",frame.left+frame.Width()/2,frame.bottom-25,screen.Width()+1,screen.Height()+1); std::fclose(file);
                next(); break;
            }
            case 2: if(!clicked || !reminders) return; next(); break;
            case 3: {
                BAutolock lock(reminders); check(view<BListView>(reminders,"reminders list")->CountItems()==2,"notification click opens durable reminder list");
                check(reminders->Frame().bottom<BScreen().Frame().bottom,"reminder form fits screen"); capture("/boot/home/mail-check/snooze-reminders.png");
                press(reminders,"reminder open"); next(); break;
            }
            case 4: {
                BAutolock lock(main); auto* list=view<BListView>(main,"conversations");
                check(list->CurrentSelection()>=0 && std::string(view<BTextView>(main,"message reader")->Text()).find("Reopen reminder body")!=std::string::npos,"open reminder selects conversation in original account");
                press(reminders,"reminder again"); next(); break;
            }
            case 5: if(!snooze) return; custom(snooze); next(); break;
            case 6: fields(snooze,"2090-02-30","09:00"); next(); break;
            case 7: {
                BAutolock lock(snooze); check(!view<BButton>(snooze,"snooze save")->IsEnabled(),"invalid date disables commit"); fields(snooze,sunday(10),"02:30"); next(); break;
            }
            case 8: {
                BAutolock lock(snooze); check(!view<BButton>(snooze,"snooze save")->IsEnabled(),"skipped local time disables commit"); fields(snooze,sunday(4),"02:30"); next(); break;
            }
            case 9: {
                BAutolock lock(snooze); auto* later=view<BCheckBox>(snooze,"snooze later"); check(later->IsEnabled(),"repeated hour exposes occurrence choice"); later->SetValue(B_CONTROL_ON); later->Invoke(); next(); break;
            }
            case 10: {
                BAutolock lock(snooze); expected=localSnoozeTimes(sunday(4),"02:30").back();
                check(std::string(view<BTextView>(snooze,"snooze status")->Text()).find("+1000")!=std::string::npos,"preview distinguishes second clock occurrence");
                check(snooze->Frame().bottom<BScreen().Frame().bottom,"snooze dialog fits display"); capture("/boot/home/mail-check/snooze-custom.png"); press(snooze,"snooze save"); next(); break;
            }
            case 11:
                if(snooze) return;
                { BAutolock lock(reminders); if(view<BListView>(reminders,"reminders list")->CountItems()!=1) return; }
                check(store->message(message).snoozedUntil==expected && store->reminders(std::time(nullptr)).size()==1,"save reschedules exact instant and clears prior due entry");
                { BAutolock lock(reminders); view<BListView>(reminders,"reminders list")->Select(0); }
                press(reminders,"reminder dismiss"); next(); break;
            case 12:
                if(!store->reminders(std::time(nullptr)).empty()) return;
                check(!store->message(other).raw.empty(),"dismiss keeps other account mail");
                { BAutolock lock(main); auto* list=view<BListView>(main,"mailboxes"); for(int32 i=0;i<list->CountItems();++i) if(std::string(static_cast<BStringItem*>(list->ItemAt(i))->Text())=="Snoozed") list->Select(i); }
                next(); break;
            case 13: {
                BAutolock lock(main); auto* list=view<BListView>(main,"conversations"); check(list->CountItems()==1,"custom snooze appears in Snoozed"); list->Select(0); main->PostMessage('msnz'); next(); break;
            }
            case 14: if(!snooze) return; press(snooze,"snooze cancel"); next(); break;
            case 15:
                if(snooze) return;
                check(store->message(message).snoozedUntil==expected,"cancel preserves previous snooze"); main->PostMessage('mwak'); next(); break;
            case 16:
                if(store->message(message).snoozedUntil) return;
                check(store->message(message).unread && store->message(message).folder=="archive","end snooze retains read state and archived folder");
                check(store->reminders(expected).empty(),"end snooze removes future reminder");
                store->snooze(message,std::time(nullptr)+2); main->PostMessage(ui::kSnoozeChanged);
                { BAutolock lock(main); main->Minimize(true); }
                next(); break;
            case 17: {
                auto rows=store->reminders(std::time(nullptr)); if(rows.empty() || !rows[0].notified) return;
                BAutolock lock(main); check(main->IsMinimized(),"ordinary timer notifies while Mail is minimized");
                check(std::string(view<BButton>(main,"reminders")->Label())=="Reminders (1)","timer updates persistent reminder count");
                check(view<BListView>(main,"conversations")->CountItems()==0,"ordinary timer removes expired items from Snoozed");
                capture("/boot/home/mail-check/snooze-live.png"); notifiedAt=std::time(nullptr); next(); break;
            }
            case 18:
                if(std::time(nullptr)-notifiedAt<11) return; // Let the OS finish displaying this disposable notification.
                result=0; timer.reset(); std::puts("Native snooze passed: separate-process overdue restart, actual notification pointer click, reminder open/dismiss/reschedule, custom validation, daylight saving choice, cancellation, early wake and real timer while minimized"); std::fflush(stdout); PostMessage(B_QUIT_REQUESTED); break;
        }
    }
    std::string database; std::shared_ptr<Store> store; ui::MailWindow* main=nullptr;
    std::unique_ptr<BMessageRunner> timer; int stage=0,ticks=0; bool clicked=false; Id message=0,other=0,account=0; Timestamp expected=0,notifiedAt=0;
};
void seed(const std::string& path)
{
    Store store(path); auto until=static_cast<Timestamp>(std::time(nullptr))+2;
    for(int i=0;i<2;++i) {
        Account account; account.name=i?"Separate account":"Reminder account"; account.unified=i==0; account.id=store.saveAccount(account);
        auto msg=parseMessage("From: author@example.test\r\nSubject: Follow up 🎉\r\nMessage-ID: <reminder@example.test>\r\n\r\nReopen reminder body\r\n");
        msg.account=account.id; msg.remoteKey="reminder"; msg.folder="archive"; msg.received=std::time(nullptr); auto id=store.ingest(msg); store.snooze(id,until);
    }
    std::puts("Seed process saved future snoozes and closed its database.");
}
}
int main(int argc,char** argv)
{
    if(argc!=3) return 2;
    setenv("TZ",":Australia/Hobart",1); tzset();
    try { if(std::string(argv[1])=="--seed") { seed(argv[2]); return 0; } Test app(argv[2]); app.Run(); return app.result; }
    catch(const std::exception& e) { std::fprintf(stderr,"FAIL: %s\n",e.what()); return 1; }
}
