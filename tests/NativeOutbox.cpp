// Real native recovery controls, backed only by disposable TLS protocol fixtures.
#include "MailWindow.h"
#include "OutboxWindow.h"
#include "ComposerWindow.h"
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
#include <StringView.h>
#include <TextControl.h>
#include <TextView.h>
#include <TranslatorRoster.h>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <unistd.h>

namespace {
void check(bool value,const char* why) { if (!value) throw std::runtime_error(why); }
template<class T> T* view(BWindow* window,const char* name)
{
    auto* result=dynamic_cast<T*>(window->FindView(name)); check(result,name); return result;
}
class OutboxTest : public BApplication {
public:
    OutboxTest(std::string directory,char** args)
        :BApplication("application/x-vnd.air-Mail-Outbox-Test"),fDirectory(std::move(directory)),fArgs(args) {}
    int result=1;
    void ReadyToRun() override
    {
        try {
            fStore=std::make_shared<mail::Store>(fDirectory+"/mail.db");
            fMail=new mail::ui::MailWindow(fStore); fMail->Show();
            BMessage tick('test'); fTimer=std::make_unique<BMessageRunner>(BMessenger(this),&tick,200000);
        } catch (const std::exception& e) { fail(e.what()); }
    }
    void MessageReceived(BMessage* message) override
    {
        if (message->what!='test') { BApplication::MessageReceived(message); return; }
        try { check(++fTicks<700,"native outbox test timed out"); step(); }
        catch (const std::exception& e) { fail(e.what()); }
    }
private:
    template<class T> T* window()
    {
        for (int32 i=0;i<CountWindows();++i) if (auto* found=dynamic_cast<T*>(WindowAt(i))) return found;
        return nullptr;
    }
    void next() { ++fStep; std::printf("Outbox stage %d\n",fStep); std::fflush(stdout); }
    void fail(const char* why) { std::fprintf(stderr,"FAIL at outbox stage %d: %s\n",fStep,why); fTimer.reset(); PostMessage(B_QUIT_REQUESTED); }
    mail::Id queue(const char* subject,const char* to)
    {
        mail::Draft draft; draft.account=fAccount.id; draft.subject=subject; draft.to=to;
        draft.bcc="hidden@example.test"; draft.text="Recovery body 🎉\n";
        draft.runs={{0,true,false,false,"https://example.test"}};
        mail::Attachment attachment; attachment.name="recovery.txt"; attachment.mediaType="text/plain"; attachment.data="Original attachment bytes";
        draft.attachments.push_back(attachment); draft.id=fStore->saveDraft(draft);
        fStore->enqueueDraft(draft.id,std::time(nullptr)); return draft.id;
    }
    bool indicator(mail::Id total,mail::Id attention)
    {
        BAutolock lock(fMail); check(lock.IsLocked(),"lock Mail indicator");
        std::string expected="Outbox ("+std::to_string(total);
        if (attention) expected+=" · "+std::to_string(attention)+(attention==1?" needs attention":" need attention");
        return view<BButton>(fMail,"outbox")->Label()==expected+")";
    }
    bool checkedMail()
    {
        BAutolock lock(fMail);
        mail::Query query; query.account=fAccount.id; auto rows=fStore->conversations(query,std::time(nullptr));
        return std::string(view<BButton>(fMail,"check mail")->Label())=="Check mail" && rows.size()==1 && rows[0].count==2;
    }
    void selectAndInvoke(int32 index,const char* control)
    {
        auto* outbox=window<mail::ui::OutboxWindow>(); check(outbox,"Outbox exists");
        BAutolock lock(outbox); check(lock.IsLocked(),"lock Outbox");
        view<BListView>(outbox,"outbox list")->Select(index);
        // Selection notification runs before the requested action.
        auto* button=view<BButton>(outbox,control); check(button->IsEnabled(),"selected recovery action is enabled"); button->Invoke();
    }
    void controls(bool retry,bool edit)
    {
        auto* outbox=window<mail::ui::OutboxWindow>(); check(outbox,"Outbox exists");
        BAutolock lock(outbox); check(lock.IsLocked(),"lock Outbox controls");
        check(view<BButton>(outbox,"retry delivery")->IsEnabled()==retry,"retry availability follows current delivery state");
        check(view<BButton>(outbox,"return to draft")->IsEnabled()==edit,"draft availability follows current delivery state");
    }
    bool confirmation(bool retry,bool accept)
    {
        auto* alert=window<BAlert>(); if (!alert) return false;
        BAutolock lock(alert); check(std::string(alert->Title())=="Delivery uncertain","uncertain delivery needs a confirmation");
        check(alert->DefaultButton()==alert->ButtonAt(0),"Cancel is the safe default");
        check(std::string(alert->ButtonAt(1)->Label())==(retry?"Retry delivery":"Return to draft"),"confirmation names the requested action");
        std::string warning=alert->TextView()->Text();
        check(warning.find(retry?"duplicate":"does not recall")!=std::string::npos,"confirmation explains duplicate or recall consequences");
        alert->ButtonAt(accept?1:0)->Invoke(); return true;
    }
    bool inspectDraft(mail::Id id,const char* subject)
    {
        auto* composer=window<mail::ui::ComposerWindow>(); if (!composer) return false;
        BAutolock lock(composer); check(lock.IsLocked(),"lock returned composer");
        check(std::string(view<BTextControl>(composer,"compose subject")->Text())==subject,"return opens the selected original draft");
        check(std::string(view<BTextControl>(composer,"compose bcc")->Text())=="hidden@example.test","return preserves private recipient roles");
        auto draft=fStore->draft(id);
        check(draft.text=="Recovery body 🎉\n" && draft.runs[0].bold && draft.runs[0].link=="https://example.test"
            && draft.attachments[0].data=="Original attachment bytes","recovery preserves rich text, links and attachments");
        check(view<BListView>(composer,"compose attachments")->CountItems()==1,"returned composer restores the attachment");
        composer->PostMessage(B_QUIT_REQUESTED); return true;
    }
    void refresh() { window<mail::ui::OutboxWindow>()->PostMessage('orfh'); fMail->PostMessage(mail::ui::kOutboxChanged); }
    void step()
    {
        auto summary=fStore->outboxSummary();
        auto* outbox=window<mail::ui::OutboxWindow>();
        switch (fStep) {
            case 0: {
                fAccount.name="Outbox fixture"; fAccount.email="user@example.test"; fAccount.provider=mail::Provider::Imap; fAccount.sentMode=mail::SentCopyMode::Local;
                fAccount.incoming={fArgs[1],std::stoi(fArgs[3]),mail::Security::TLS,"user",fArgs[2]};
                fAccount.outgoing={fArgs[1],std::stoi(fArgs[7]),mail::Security::TLS,"user",fArgs[2]};
                auto id=fStore->saveAccount(fAccount);
                for (const auto& account:fStore->accounts()) if (account.id==id) fAccount=account;
                std::puts("Approve this disposable test app's key-store prompts if shown."); std::fflush(stdout);
                mail::ui::SaveCredentials(fAccount,{"test-password","test-password"});
                queue("Retry rejected delivery","retry@example.test");
                fRejected=queue("Return rejected draft","reject@example.test");
                queue("Uncertain retry","drop-once@example.test");
                fUncertain=queue("Uncertain draft","drop@example.test");
                fMail->PostMessage(mail::ui::kSendQueued); next(); break;
            }
            case 1: {
                if (summary.failed!=2 || summary.uncertain!=2 || !indicator(4,4)) return;
                BAutolock lock(fMail); auto* button=view<BButton>(fMail,"outbox"); button->Invoke(); button->Invoke(); next(); break;
            }
            case 2: {
                if (!outbox) return;
                int count=0; for (int32 i=0;i<CountWindows();++i) if (dynamic_cast<mail::ui::OutboxWindow*>(WindowAt(i))) ++count;
                check(count==1,"repeated opening activates the one existing Outbox");
                BAutolock lock(outbox); view<BListView>(outbox,"outbox list")->Select(2); outbox->PostMessage('orfh'); next(); break;
            }
            case 3: {
                BAutolock lock(outbox); check(view<BListView>(outbox,"outbox list")->CurrentSelection()==2,"refresh preserves selected delivery ID");
                check(std::string(view<BTextView>(outbox,"delivery details")->Text()).find("will not retry automatically")!=std::string::npos,"uncertain details explain why delivery is paused");
                controls(true,true);
                if (const char* path=std::getenv("MAIL_OUTBOX_SCREENSHOT")) {
                    BBitmap* bitmap=nullptr; check(BScreen().GetBitmap(&bitmap)==B_OK,"capture Outbox");
                    BBitmapStream stream(bitmap); BFile file(path,B_WRITE_ONLY|B_CREATE_FILE|B_ERASE_FILE);
                    check(BTranslatorRoster::Default()->Translate(&stream,nullptr,nullptr,&file,B_PNG_FORMAT)==B_OK,"write Outbox screenshot");
                }
                fMail->PostMessage('mchk'); next(); break;
            }
            case 4:
                if (!checkedMail()) return;
                check(summary.failed==2 && summary.uncertain==2 && indicator(4,4),"successful receive does not erase delivery attention or retry problems");
                selectAndInvoke(0,"retry delivery"); next(); break;
            case 5:
                if (summary.total()!=3 || !indicator(3,3)) return;
                check(summary.failed==1 && summary.uncertain==2,"explicit retry successfully removes only the selected failure");
                refresh(); next(); break;
            case 6: controls(true,true); selectAndInvoke(0,"return to draft"); next(); break;
            case 7:
                if (!inspectDraft(fRejected,"Return rejected draft")) return;
                check(summary.total()==2 && summary.uncertain==2 && indicator(2,2),"return to draft immediately clears that delivery problem"); next(); break;
            case 8:
                if (window<mail::ui::ComposerWindow>()) return;
                selectAndInvoke(0,"retry delivery"); next(); break;
            case 9: if (confirmation(true,false)) next(); break;
            case 10:
                if (window<BAlert>()) return;
                check(summary.uncertain==2 && summary.total()==2,"cancel retry leaves both uncertain messages untouched");
                fMail->PostMessage('mchk'); next(); break;
            case 11:
                if (!checkedMail()) return;
                check(summary.uncertain==2 && indicator(2,2),"a later mail check still does not resend uncertain deliveries");
                selectAndInvoke(0,"retry delivery"); next(); break;
            case 12: if (confirmation(true,true)) next(); break;
            case 13:
                if (summary.total()!=1 || !indicator(1,1)) return;
                check(summary.uncertain==1,"confirmed uncertain retry finishes only the selected delivery"); refresh(); next(); break;
            case 14: selectAndInvoke(0,"return to draft"); next(); break;
            case 15: if (confirmation(false,false)) next(); break;
            case 16:
                if (window<BAlert>()) return;
                check(summary.uncertain==1 && !window<mail::ui::ComposerWindow>(),"cancel return leaves the uncertain message locked in Outbox");
                selectAndInvoke(0,"return to draft"); next(); break;
            case 17: if (confirmation(false,true)) next(); break;
            case 18:
                if (!inspectDraft(fUncertain,"Uncertain draft")) return;
                check(!summary.total() && indicator(0,0),"confirmed return removes the last attention indicator"); next(); break;
            case 19:
                if (window<mail::ui::ComposerWindow>()) return;
                fExtra=queue("State transition draft","recipient@example.test"); refresh(); next(); break;
            case 20: {
                controls(false,true);
                auto claimed=fStore->claimOutgoing(); check(claimed.draft==fExtra,"claim queued message for an in-flight race"); fActive=claimed.id;
                // The last displayed state is still queued. A worker claimed it
                // before the click reached the window; the handler must re-read it.
                BAutolock lock(outbox); view<BButton>(outbox,"return to draft")->Invoke(); outbox->PostMessage('otry'); next(); break;
            }
            case 21:
                check(summary.sending==1 && !window<mail::ui::ComposerWindow>() && !window<BAlert>(),"stale action cannot edit or retry a newly active send");
                controls(false,false); fStore->finishOutgoing(fActive,"failed","Fixture transport stopped before submission",std::time(nullptr)); refresh(); next(); break;
            case 22: {
                controls(true,true); BAutolock lock(outbox); view<BButton>(outbox,"return to draft")->Invoke(); next(); break;
            }
            case 23:
                if (!inspectDraft(fExtra,"State transition draft")) return;
                fMail->PostMessage('mchk'); refresh(); next(); break;
            case 24:
                if (window<mail::ui::ComposerWindow>() || !checkedMail() || !indicator(0,0)) return;
                check(!summary.total() && fStore->drafts().size()==3,"returning messages never submits them again");
                controls(false,false); outbox->PostMessage(B_QUIT_REQUESTED); next(); break;
            case 25:
                if (outbox) return;
                fMail->PostMessage('mout'); next(); break;
            case 26: {
                if (!outbox) return;
                controls(false,false); BAutolock lock(outbox);
                check(std::string(view<BTextView>(outbox,"delivery details")->Text()).find("No messages waiting")!=std::string::npos,"closed Outbox can reopen with an accurate empty state");
                mail::Query sent; sent.folder="sent"; check(fStore->conversations(sent,std::time(nullptr)).size()==2,"only acknowledged retries create local Sent messages");
                mail::ui::RemoveCredentials(fAccount); result=0; fTimer.reset();
                std::puts("Native Outbox passed: persistent attention, receive updates, singleton, selection, real SMTP rejection/uncertainty, both confirmation choices, retries, rich draft recovery, active-send race and empty state");
                std::fflush(stdout); PostMessage(B_QUIT_REQUESTED); next(); break;
            }
        }
    }
    std::string fDirectory;
    char** fArgs;
    std::shared_ptr<mail::Store> fStore;
    mail::ui::MailWindow* fMail=nullptr;
    mail::Account fAccount;
    mail::Id fRejected=0,fUncertain=0,fExtra=0,fActive=0;
    std::unique_ptr<BMessageRunner> fTimer;
    int fStep=0,fTicks=0;
};
}
int main(int argc,char** argv)
{
    if (argc!=9) return 2;
    char directory[]="/tmp/mail-outbox-XXXXXX"; if (!mkdtemp(directory)) return 2;
    int result; { OutboxTest app(directory,argv); app.Run(); result=app.result; }
    for (const char* name:{"mail.db-wal","mail.db-shm","mail.db"}) unlink((std::string(directory)+"/"+name).c_str());
    rmdir(directory); return result;
}
