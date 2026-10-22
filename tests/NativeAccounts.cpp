// Uses only disposable protocol accounts and the guest's system key store.
#include "MailWindow.h"
#include "AccountWindow.h"
#include "Credentials.h"
#include "ComposerWindow.h"
#include "OutboxWindow.h"
#include "Emoji.h"
#include "mail/Mime.h"
#include <Application.h>
#include <Autolock.h>
#include <Bitmap.h>
#include <BitmapStream.h>
#include <Button.h>
#include <File.h>
#include <Entry.h>
#include <ListView.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <MessageRunner.h>
#include <Screen.h>
#include <StringView.h>
#include <TabView.h>
#include <TextControl.h>
#include <TextView.h>
#include <TranslatorRoster.h>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <fstream>
#include <stdexcept>
#include <unistd.h>

namespace {
void check(bool ok,const char* text) { if (!ok) throw std::runtime_error(text); }
template<class T> T* view(BView* parent,const char* name)
{
    auto* result=dynamic_cast<T*>(parent->FindView(name)); check(result,name); return result;
}
void capture(const std::string& path)
{
    BBitmap* bitmap=nullptr; check(BScreen().GetBitmap(&bitmap)==B_OK,"capture screen");
    BBitmapStream stream(bitmap); BFile file(path.c_str(),B_WRITE_ONLY|B_CREATE_FILE|B_ERASE_FILE);
    check(file.InitCheck()==B_OK,"open screen output");
    check(BTranslatorRoster::Default()->Translate(&stream,nullptr,nullptr,&file,B_PNG_FORMAT)==B_OK,"write screen output");
}
class Accounts : public BApplication {
public:
    Accounts(std::string directory,char** args)
        : BApplication("application/x-vnd.air-Mail-Accounts-Test"),fDirectory(std::move(directory)),fArgs(args) {}
    int result=1;
    void ReadyToRun() override
    {
        try {
            fStore=std::make_shared<mail::Store>(fDirectory+"/mail.db");
            fMail=new mail::ui::MailWindow(fStore); fMail->Show();
            fMail->PostMessage('macc');
            BMessage tick('test'); fTimer=std::make_unique<BMessageRunner>(BMessenger(this),&tick,200000);
        } catch (const std::exception& e) { fail(e.what()); }
    }
    void MessageReceived(BMessage* message) override
    {
        if (message->what!='test') { BApplication::MessageReceived(message); return; }
        try {
            check(++fTicks<700,"account test timed out");
            if (fStep==0) {
                for (int32 i=0;i<CountWindows();++i)
                    if (auto* window=dynamic_cast<mail::ui::AccountWindow*>(WindowAt(i))) fAccounts=window;
                if (!fAccounts) return;
                BAutolock lock(fAccounts); check(lock.IsLocked(),"lock account window");
                auto* provider=dynamic_cast<BMenuField*>(fAccounts->FindView("provider")); check(provider,"account provider control");
                auto* sent=dynamic_cast<BMenuField*>(fAccounts->FindView("sent copies")); check(sent,"Sent copy setting"); sent->Menu()->ItemAt(3)->SetMarked(true);
                provider->Menu()->ItemAt(2)->SetMarked(true); fAccounts->PostMessage('acPr'); fStep=1;
            } else if (fStep==1) {
                BAutolock lock(fAccounts); check(lock.IsLocked(),"lock account window");
                auto* tabs=dynamic_cast<BTabView*>(fAccounts->FindView("servers")); check(tabs && tabs->CountTabs()==2,"incoming and outgoing server tabs");
                auto* incoming=tabs->TabAt(0)->View(); auto* outgoing=tabs->TabAt(1)->View();
                check(std::string(view<BTextControl>(incoming,"host")->Text())=="127.0.0.1"
                    && std::string(view<BTextControl>(incoming,"port")->Text())=="1143"
                    && std::string(view<BTextControl>(outgoing,"port")->Text())=="1025","Bridge presets use local endpoints");
                check(view<BTextControl>(incoming,"password")->TextView()->IsTypingHidden()
                    && view<BTextControl>(outgoing,"password")->TextView()->IsTypingHidden(),"password controls mask typing");
                dynamic_cast<BTextControl*>(fAccounts->FindView("account name"))->SetText("Native test account");
                dynamic_cast<BTextControl*>(fAccounts->FindView("email"))->SetText("user@example.test");
                dynamic_cast<BMenuField*>(fAccounts->FindView("provider"))->Menu()->ItemAt(0)->SetMarked(true);
                for (auto* server : {incoming,outgoing}) {
                    view<BTextControl>(server,"host")->SetText(fArgs[1]);
                    view<BTextControl>(server,"username")->SetText("user");
                    view<BTextControl>(server,"certificate")->SetText(fArgs[2]);
                    view<BMenuField>(server,"security")->Menu()->ItemAt(0)->SetMarked(true);
                }
                view<BTextControl>(incoming,"port")->SetText(fArgs[3]);
                view<BTextControl>(outgoing,"port")->SetText(fArgs[7]);
                view<BTextControl>(incoming,"password")->SetText("test-password");
                fAccounts->PostMessage('acSv'); fStep=2;
                std::puts("Account save requested; approve the disposable test app's key-store prompt if shown."); std::fflush(stdout);
            } else if (fStep==2) {
                // Save runs on the account window; its key-store permission dialog
                // must be answered normally. Avoid blocking this test looper.
                if (fAccounts->LockWithTimeout(0)!=B_OK) return;
                auto* list=dynamic_cast<BListView*>(fAccounts->FindView("accounts"));
                bool saved=list && list->CountItems()==1; fAccounts->Unlock();
                if (!saved) return;
                for (const auto& a : fStore->accounts()) if (a.provider==mail::Provider::Imap) fAccount=a;
                check(fAccount.id>0,"native save persists account metadata");
                check(fAccount.sentMode==mail::SentCopyMode::Local,"native Sent copy setting persists");
                auto credentials=mail::ui::LoadCredentials(fAccount);
                check(credentials.incoming=="test-password" && credentials.outgoing=="test-password","passwords round-trip through the system key store");
                mail::ui::SaveCredentials(fAccount,{"replacement","outgoing replacement"});
                credentials=mail::ui::LoadCredentials(fAccount);
                check(credentials.incoming=="replacement" && credentials.outgoing=="outgoing replacement","replacement retains distinct incoming and outgoing passwords");
                mail::ui::SaveCredentials(fAccount,{"test-password","test-password"});
                fStep=20;
            } else if (fStep==20) {
                if (const char* path=std::getenv("MAIL_ACCOUNTS_SCREENSHOT")) capture(path);
                fAccounts->PostMessage(B_QUIT_REQUESTED); fAccounts=nullptr; fStep=3;
            } else if (fStep==3) {
                BAutolock lock(fMail); check(lock.IsLocked(),"lock mailbox window");
                auto* status=dynamic_cast<BStringView*>(fMail->FindView("status")); check(status,"mail status view");
                std::string text=status->Text();
                mail::Query query; query.account=fAccount.id;
                auto rows=fStore->conversations(query,2000000000);
                if (rows.empty()) {
                    if (fTicks%50==0) { std::printf("Waiting for sync: %s\n",text.c_str()); std::fflush(stdout); }
                    return;
                }
                if (rows[0].count<2) return;
                check(rows.size()==1 && rows[0].count==2,"closing Accounts triggers real background IMAP receive");
                check(fStore->remoteFolders(fAccount.id).size()==4,"background sync persists the server folder catalog");
                fMail->PostMessage('macc'); fStep=4;
            } else if (fStep==4) {
                for (int32 i=0;i<CountWindows();++i)
                    if (auto* window=dynamic_cast<mail::ui::AccountWindow*>(WindowAt(i))) fAccounts=window;
                if (!fAccounts) { if (fTicks%5==0) fMail->PostMessage('macc'); return; }
                BAutolock lock(fAccounts); check(lock.IsLocked(),"lock reopened account window");
                auto* tabs=dynamic_cast<BTabView*>(fAccounts->FindView("servers"));
                auto* name=dynamic_cast<BTextControl*>(fAccounts->FindView("account name"));
                if (std::string(name->Text())!="Native test account") return;
                check(std::string(view<BTextControl>(tabs->TabAt(0)->View(),"password")->Text()).empty(),"editing an account never displays the saved secret");
                name->SetText("Renamed native account"); fAccounts->PostMessage('acSv'); fStep=5;
            } else if (fStep==5) {
                if (fAccounts->LockWithTimeout(0)!=B_OK) return;
                auto* list=dynamic_cast<BListView*>(fAccounts->FindView("accounts"));
                bool saved=list && list->CountItems()==1 && std::string(static_cast<BStringItem*>(list->ItemAt(0))->Text())=="Renamed native account";
                fAccounts->Unlock(); if (!saved) return;
                check(mail::ui::LoadCredentials(fAccount).incoming=="test-password","blank password on edit preserves saved credentials");
                fAccounts->PostMessage(B_QUIT_REQUESTED); fAccounts=nullptr;
                fMail->PostMessage('mnew'); fStep=6;
            } else if (fStep==6) {
                for (int32 i=0;i<CountWindows();++i)
                    if (auto* window=dynamic_cast<mail::ui::ComposerWindow*>(WindowAt(i))) fComposer=window;
                if (!fComposer) return;
                BAutolock lock(fComposer); check(lock.IsLocked(),"lock composer");
                dynamic_cast<BTextControl*>(fComposer->FindView("compose to"))->SetText("recipient@example.test");
                dynamic_cast<BTextControl*>(fComposer->FindView("compose bcc"))->SetText("hidden@example.test");
                dynamic_cast<BTextControl*>(fComposer->FindView("compose subject"))->SetText("Native compose café");
                auto* editor=dynamic_cast<BTextView*>(fComposer->FindView("compose body")); check(editor,"native rich text editor");
                editor->SetText("Hello from the native composer.\n"); editor->Select(0,5);
                BMessage bold('cbld'); fComposer->MessageReceived(&bold); editor->Select(6,6);
                std::ofstream(fDirectory+"/attach.txt")<<"Native attachment bytes";
                std::string png=mail::base64Decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+a5e8AAAAASUVORK5CYII=");
                std::ofstream image(fDirectory+"/image.png",std::ios::binary); image<<png; image.close();
                BMessage drop(B_SIMPLE_DATA); drop.AddBool("inline",true);
                for (const char* name:{"attach.txt","image.png"}) { entry_ref ref; check(get_ref_for_path((fDirectory+"/"+name).c_str(),&ref)==B_OK,"attachment drop ref"); drop.AddRef("refs",&ref); }
                BMessenger(editor).SendMessage(&drop); fStep=7;
            } else if (fStep==7) {
                auto drafts=fStore->drafts(); if (drafts.empty() || drafts[0].attachments.size()!=2) return;
                if (drafts[0].subject!="Native compose café" || drafts[0].runs.empty() || !drafts[0].runs[0].bold) {
                    std::fprintf(stderr,"Draft subject=%s, runs=%zu, first bold=%d\n",drafts[0].subject.c_str(),drafts[0].runs.size(),drafts[0].runs.empty()?0:drafts[0].runs[0].bold);
                }
                check(drafts[0].subject=="Native compose café" && !drafts[0].runs.empty() && drafts[0].runs[0].bold,"native formatting and headers reach autosaved draft");
                check(drafts[0].attachments[1].inlineImage,"dropped images are marked for inline delivery");
                check(drafts[0].images.size()==1 && drafts[0].images[0].offset==6,"image drop is positioned inside body text");
                fDraft=drafts[0].id;
                BAutolock lock(fComposer); check(lock.IsLocked(),"lock composer for emoji");
                auto* editor=dynamic_cast<BTextView*>(fComposer->FindView("compose body")); editor->Select(editor->TextLength(),editor->TextLength());
                BMessage emoji('cemo'); emoji.AddString("emoji","🎉"); fComposer->PostMessage(&emoji); fComposer->PostMessage(B_QUIT_REQUESTED);
                fComposer=nullptr; fStep=8;
            } else if (fStep==8) {
                for (int32 i=0;i<CountWindows();++i) if (dynamic_cast<mail::ui::ComposerWindow*>(WindowAt(i))) return;
                auto draft=fStore->draft(fDraft); check(draft.text.find("🎉")!=std::string::npos,"closing immediately after an edit saves the latest emoji");
                BMessage open(mail::ui::kOpenDraft); open.AddInt64("draft",fDraft); fMail->PostMessage(&open); fStep=9;
            } else if (fStep==9) {
                for (int32 i=0;i<CountWindows();++i) if (auto* window=dynamic_cast<mail::ui::ComposerWindow*>(WindowAt(i))) fComposer=window;
                if (!fComposer) return;
                BAutolock lock(fComposer); check(lock.IsLocked(),"lock reopened composer");
                auto* editor=dynamic_cast<BTextView*>(fComposer->FindView("compose body")); BFont font; editor->GetFontAndColor(0,&font);
                check(font.Face()&B_BOLD_FACE,"reopened draft restores native bold text");
                check(std::string(editor->Text()).find("🎉")!=std::string::npos,"reopened draft restores Unicode text");
                editor->GetFontAndColor(std::string(editor->Text()).find("🎉"),&font);
                bool glyph=false; font.GetHasGlyphs("🎉",1,&glyph,false);
                check(mail::ui::IsEmojiFont(font) && glyph,"reopened composer displays an actual emoji glyph");
                fStep=21;
            } else if (fStep==21) {
                if (const char* path=std::getenv("MAIL_COMPOSER_SCREENSHOT")) capture(path);
                fComposer->PostMessage('csnd'); fComposer=nullptr; fStep=10;
            } else if (fStep==10) {
                mail::Query query; query.account=fAccount.id; query.folder="sent";
                auto rows=fStore->conversations(query,2000000000);
                if (rows.empty()) {
                    for (const auto& item:fStore->outbox()) check(item.state!="failed" && item.state!="uncertain",item.error.c_str());
                    return;
                }
                auto sent=fStore->thread(rows[0].id)[0];
                check(sent.subject=="Native compose café" && sent.attachments.size()==2 && sent.html.find("<strong>Hello</strong>")!=std::string::npos,"native Send reaches SMTP and preserves rich MIME in Sent");
                check(sent.raw.find("hidden@example.test")==std::string::npos && fStore->drafts().empty() && fStore->outbox().empty(),"sending hides BCC and completes the draft/outbox transition");
                mail::ui::RemoveCredentials(fAccount);
                bool missing=false; try { mail::ui::LoadCredentials(fAccount); } catch (const std::exception&) { missing=true; }
                check(missing,"test credential is removed from the system key store");
                result=0; std::puts("Native accounts/composer passed: setup, key store, IMAP receive, rich text, emoji, file/image drops, draft reopen, SMTP send and Sent copy");
                std::fflush(stdout); PostMessage(B_QUIT_REQUESTED); fStep=11;
            }
        } catch (const std::exception& e) { fail(e.what()); }
    }
private:
    void fail(const char* text) { std::fprintf(stderr,"FAIL: %s\n",text); PostMessage(B_QUIT_REQUESTED); }
    std::string fDirectory;
    char** fArgs;
    std::shared_ptr<mail::Store> fStore;
    mail::ui::MailWindow* fMail=nullptr;
    mail::ui::AccountWindow* fAccounts=nullptr;
    mail::ui::ComposerWindow* fComposer=nullptr;
    mail::Id fDraft=0;
    mail::Account fAccount;
    std::unique_ptr<BMessageRunner> fTimer;
    int fStep=0,fTicks=0;
};
}
int main(int argc,char** argv)
{
    if (argc!=9) return 2;
    char directory[]="/tmp/mail-accounts-XXXXXX"; if (!mkdtemp(directory)) return 2;
    int result;
    { Accounts app(directory,argv); app.Run(); result=app.result; }
    for (const char* file : {"mail.db-wal","mail.db-shm","mail.db","attach.txt","image.png"}) unlink((std::string(directory)+"/"+file).c_str());
    rmdir(directory); return result;
}
