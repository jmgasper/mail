#include "MailWindow.h"
#include "ComposerWindow.h"
#include "DraftWindow.h"
#include "DraftEditor.h"
#include "Credentials.h"
#include <Application.h>
#include <Alert.h>
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
#include <TranslatorRoster.h>
#include <nlohmann/json.hpp>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <unistd.h>
using namespace mail;
using namespace mail::ui;
using Json=nlohmann::json;
namespace {
int checks=0;
void check(bool value,const char* why) { ++checks; if (!value) throw std::runtime_error(why); }
template<class T> T* view(BWindow* window,const char* name)
{ auto* v=dynamic_cast<T*>(window->FindView(name)); if (!v) throw std::runtime_error(name); return v; }
class Test : public BApplication {
public:
    Test(std::string path,char** args):BApplication("application/x-vnd.air-Mail-Draft-Test"),fPath(std::move(path)),fBase("https://"+std::string(args[1])+":"+args[3])
    { fOptions.endpoints.gmail=fBase+"/gmail"; fOptions.endpoints.certificate=args[2]; }
    int result=1;
    void ReadyToRun() override {
        try {
            fStore=std::make_shared<Store>(fPath+"/mail.db"); Account a; a.name="Native draft fixture"; a.email="user@example.test"; a.provider=Provider::Gmail; fStore->saveAccount(a); fAccount=fStore->accounts()[0];
            std::puts("Preparing disposable draft test credentials"); std::fflush(stdout);
            SaveGoogleTokens(fAccount,{{"desktop","fixture-secret"},"fixture-access","fixture-refresh",std::time(nullptr)+3600,kGoogleMailScope});
            put("Original Gmail draft");
            fMail=new MailWindow(fStore,fOptions); fMail->Show();
            BMessage tick('tick'); fTimer=std::make_unique<BMessageRunner>(BMessenger(this),&tick,200000);
        } catch (const std::exception& e) { fail(e.what()); }
    }
    void MessageReceived(BMessage* message) override {
        if (message->what!='tick') { BApplication::MessageReceived(message); return; }
        try { tick(); } catch (const std::exception& e) { fail(e.what()); }
    }
private:
    void next(int step) { fStep=step; std::printf("Draft UI step %d\n",step); std::fflush(stdout); }
    void fail(const char* why) { std::fprintf(stderr,"FAIL draft UI step %d: %s\n",fStep,why); fTimer.reset(); PostMessage(B_QUIT_REQUESTED); }
    Json fixture(const std::string& path,const Json& body=Json()) {
        HttpRequest r; r.url=fBase+"/fixture/"+path; r.certificate=fOptions.endpoints.certificate;
        if (!body.is_null()) { r.method="POST"; r.contentType="application/json"; r.body=body.dump(); }
        auto response=requestHttps(r,fCancel); requireHttpSuccess(response); return Json::parse(response.body);
    }
    void put(const std::string& subject) {
        auto raw="From: user@example.test\r\nTo: recipient@example.test\r\nSubject: "+subject+"\r\nMessage-ID: <native-draft@example.test>\r\n\r\nRemote body";
        fixture("put",{{"id","native-draft"},{"raw",base64UrlEncode(raw)}});
    }
    template<class T> T* window() {
        for (int32 i=0;i<CountWindows();++i) if (auto* w=dynamic_cast<T*>(WindowAt(i))) return w;
        return nullptr;
    }
    void press(const char* name) {
        auto* review=window<DraftWindow>(); BAutolock lock(review);
        BMessage refresh('gdrf'); review->MessageReceived(&refresh);
        auto* button=view<BButton>(review,name); check(button->IsEnabled(),"draft recovery action is enabled"); button->Invoke();
    }
    bool idle() { BAutolock lock(fMail); return std::string(view<BButton>(fMail,"check mail")->Label())=="Check mail"; }
    bool confirm(bool accepted,const char* title) {
        auto* alert=window<BAlert>(); if (!alert) return false;
        BAutolock lock(alert); check(std::string(alert->Title())==title,"correct draft recovery confirmation");
        check(alert->DefaultButton()==alert->ButtonAt(0),"draft recovery confirmation defaults to Cancel");
        if (std::string(title)=="Gmail save unconfirmed") check(std::string(alert->TextView()->Text()).find("duplicate")!=std::string::npos,"retry confirmation explains duplicate risk");
        alert->ButtonAt(accepted?1:0)->Invoke(); return true;
    }
    void recoveryDraft(const char* behavior,const char* subject) {
        Draft d; d.account=fAccount.id; d.to="recipient@example.test"; d.subject=subject; d.text="Preserve this local body 🎉";
        d.id=fStore->saveDraft(d); fRecoveryLocal=d.id; fRecoveryId=0; fOriginalMessage=d.messageId;
        fixture("settings",{{"behavior",behavior}}); fMail->PostMessage(kCheckDrafts);
    }
    bool held() {
        for (const auto& state:fStore->gmailDraftStates()) if (state.draft==fRecoveryLocal && !state.active && state.attempt=="create") {
            fRecoveryId=state.id; return true;
        }
        return false;
    }
    bool selectRecovery() {
        auto* review=window<DraftWindow>(); if (!review) return false;
        auto states=fStore->gmailDraftStates(); BAutolock lock(review);
        BMessage refresh('gdrf'); review->MessageReceived(&refresh);
        auto* list=view<BListView>(review,"draft sync list");
        if (list->CountItems()!=static_cast<int32>(states.size())) { review->PostMessage('gdrf'); return false; }
        for (std::size_t i=0;i<states.size();++i) if (states[i].id==fRecoveryId) { list->Select(i); return true; }
        return false;
    }
    void tick() {
        if (++fTicks>400) throw std::runtime_error("native draft test timed out");
        if (fStep==0) {
            auto states=fStore->gmailDraftStates(); if (states.empty() || states[0].active) return;
            check(states.size()==1 && fStore->drafts(fAccount.id).size()==1,"background mail check imports a single editable Gmail draft");
            fState=states[0]; auto d=fStore->draft(fState.draft);
            fComposer=new ComposerWindow(fMail,fStore,d); fComposer->Show(); fComposerMessenger=BMessenger(fComposer);
            fOpened=system_time(); next(7);
        } else if (fStep==7) {
            if (system_time()-fOpened<3000000) return;
            { BAutolock lock(fComposer); check(fComposer->QuitRequested(),"untouched imported composer can close"); }
            auto original=fStore->draft(fState.draft);
            check(original.revision==fState.savedRevision && fixture("stats")["update"]==0,"opening and closing an untouched imported draft preserves its revision and Gmail MIME");
            {
                BAutolock lock(fComposer);
                view<BTextControl>(fComposer,"compose subject")->SetText("Immediate header edit");
                auto* editor=view<DraftEditor>(fComposer,"compose body"); editor->Select(editor->TextLength(),editor->TextLength()); editor->Insert(" final edit");
                check(fComposer->QuitRequested(),"Close saves edits before their queued modification messages");
            }
            auto changed=fStore->draft(fState.draft);
            check(changed.subject=="Immediate header edit" && changed.text==original.text+" final edit","immediate Close preserves both header and body edits");
            { BAutolock lock(fComposer); view<BTextControl>(fComposer,"compose subject")->SetText("Native background save 🎉"); }
            next(1);
        } else if (fStep==1) {
            auto s=fStore->gmailDraftState(fState.id); auto d=fStore->draft(s.draft);
            if (s.active || s.savedRevision!=d.revision || d.subject!="Native background save 🎉") return;
            check(fixture("stats")["update"]==1,"native autosave reaches Gmail without pressing Check mail");
            { BAutolock lock(fComposer); view<BTextControl>(fComposer,"compose subject")->SetText("Keep local version 🎉"); }
            put("Keep remote version"); fMail->PostMessage(kCheckDrafts); next(2);
        } else if (fStep==2) {
            auto s=fStore->gmailDraftState(fState.id); if (s.active || !s.conflict) return;
            { BAutolock lock(fComposer); check(std::string(view<BTextControl>(fComposer,"compose subject")->Text())=="Keep local version 🎉","background conflict leaves unsaved composer text untouched"); }
            fComposer->PostMessage(B_QUIT_REQUESTED); next(3);
        } else if (fStep==3) {
            if (fComposerMessenger.IsValid()) return;
            check(fStore->draft(fState.draft).subject=="Keep local version 🎉","closing a conflicting composer saves its local version");
            fMail->PostMessage(kDraftReview); next(4);
        } else if (fStep==4) {
            auto* review=window<DraftWindow>(); if (!review) return;
            BAutolock lock(review);
            check(view<BListView>(review,"draft sync list")->CountItems()==1,"native review presents the conflicting draft");
            check(view<BButton>(review,"keep draft versions")->IsEnabled(),"keep both is available after closing the composer");
            BBitmap* bitmap=nullptr; check(BScreen().GetBitmap(&bitmap)==B_OK,"capture draft conflict review");
            BBitmapStream stream(bitmap); const char* output=std::getenv("MAIL_DRAFT_SCREENSHOT");
            BFile file(output?output:"/boot/home/mail-check/native-drafts.png",B_WRITE_ONLY|B_CREATE_FILE|B_ERASE_FILE);
            check(BTranslatorRoster::Default()->Translate(&stream,nullptr,nullptr,&file,B_PNG_FORMAT)==B_OK,"write draft review capture");
            view<BButton>(review,"keep draft versions")->Invoke(); next(5);
        } else if (fStep==5) {
            auto drafts=fStore->drafts(fAccount.id); if (drafts.size()!=2) return;
            bool local=false,remote=false;
            for (const auto& d:drafts) { local|=d.subject=="Keep local version 🎉"; remote|=d.subject=="Keep remote version"; }
            check(local && remote,"native keep-both action retains both versions");
            fMail->PostMessage(kCheckDrafts); next(6);
        } else if (fStep==6) {
            auto states=fStore->gmailDraftStates(); if (states.size()!=2) return;
            for (const auto& s:states) if (s.active || s.remoteId.empty()) return;
            check(fixture("stats")["create"]==1 && fixture("stats")["update"]==1,"review creates one new Gmail draft without overwriting the remote version");
            recoveryDraft("rewrite-disconnect","Recover interrupted save 🎉"); next(10);
        } else if (fStep==10) {
            if (!held()) return;
            fMail->PostMessage(kCheckDrafts); next(11);
        } else if (fStep==11) {
            auto state=fStore->gmailDraftState(fRecoveryId); if (state.active || fStore->gmailDraftMatches(state.id).size()!=1) return;
            if (selectRecovery()) next(12);
        } else if (fStep==12) {
            auto* review=window<DraftWindow>(); BAutolock lock(review);
            if (view<BListView>(review,"draft copies")->CountItems()!=1 || !view<BButton>(review,"use gmail copy")->IsEnabled()) return;
            check(std::string(view<BTextView>(review,"draft sync details")->Text()).find("Selected Gmail copy:")!=std::string::npos,"review shows the selected server snapshot");
            BBitmap* bitmap=nullptr; check(BScreen().GetBitmap(&bitmap)==B_OK,"capture draft recovery choices"); BBitmapStream stream(bitmap);
            BFile file("/boot/home/mail-check/native-draft-recovery.png",B_WRITE_ONLY|B_CREATE_FILE|B_ERASE_FILE);
            check(BTranslatorRoster::Default()->Translate(&stream,nullptr,nullptr,&file,B_PNG_FORMAT)==B_OK,"write draft recovery capture");
            view<BButton>(review,"use gmail copy")->Invoke(); next(13);
        } else if (fStep==13) {
            if (confirm(false,"Use Gmail copy")) next(14);
        } else if (fStep==14) {
            if (window<BAlert>() || !idle()) return;
            check(fStore->gmailDraftState(fRecoveryId).attempt=="create","Cancel leaves the uncertain save unchanged"); press("use gmail copy"); next(15);
        } else if (fStep==15) {
            if (confirm(true,"Use Gmail copy")) next(16);
        } else if (fStep==16) {
            if (window<BAlert>()) return;
            auto state=fStore->gmailDraftState(fRecoveryId); if (state.active || !state.attempt.empty() || state.remoteId.empty()) return;
            check(fStore->drafts().size()==3 && fixture("stats")["create"]==2 && fixture("stats")["update"]==1,"native copy selection resolves rewritten MIME without another upload");
            recoveryDraft("disconnect","Explicitly retry a missing save"); next(20);
        } else if (fStep==20) {
            if (!held() || !idle()) return;
            fCheckedRevision=fStore->gmailDraftState(fRecoveryId).revision;
            fixture("drop",{{"id",fixture("stats")["drafts"].back()}}); fMail->PostMessage(kCheckDrafts); next(21);
        } else if (fStep==21) {
            auto state=fStore->gmailDraftState(fRecoveryId); if (state.active || state.revision<=fCheckedRevision || !idle() || !fStore->gmailDraftMatches(state.id).empty()) return;
            if (selectRecovery()) next(22);
        } else if (fStep==22) {
            auto* review=window<DraftWindow>(); BAutolock lock(review);
            if (!view<BButton>(review,"retry draft save")->IsEnabled()) return;
            check(!view<BButton>(review,"use gmail copy")->IsEnabled(),"an empty search cannot choose a nonexistent copy");
            view<BButton>(review,"retry draft save")->Invoke(); next(23);
        } else if (fStep==23) {
            if (confirm(false,"Gmail save unconfirmed")) next(24);
        } else if (fStep==24) {
            if (window<BAlert>() || !idle()) return;
            check(fStore->draft(fRecoveryLocal).messageId==fOriginalMessage && fixture("stats")["create"]==3,"Cancel does not retry or change the draft identity");
            press("retry draft save"); next(25);
        } else if (fStep==25) {
            if (confirm(true,"Gmail save unconfirmed")) next(26);
        } else if (fStep==26) {
            if (window<BAlert>()) return;
            auto state=fStore->gmailDraftState(fRecoveryId); if (state.active || !state.attempt.empty() || state.remoteId.empty()) return;
            check(fStore->draft(fRecoveryLocal).messageId!=fOriginalMessage && fixture("stats")["create"]==4,"confirmed native retry makes one fresh recoverable draft");
            recoveryDraft("rewrite-disconnect","Discard without deleting Gmail"); next(30);
        } else if (fStep==30) {
            if (!held() || !idle()) return;
            fStore->removeDraft(fRecoveryLocal); fMail->PostMessage(kCheckDrafts); next(31);
        } else if (fStep==31) {
            auto state=fStore->gmailDraftState(fRecoveryId); if (state.active || !idle() || fStore->gmailDraftMatches(state.id).size()!=1) return;
            if (selectRecovery()) next(32);
        } else if (fStep==32) {
            auto* review=window<DraftWindow>(); BAutolock lock(review);
            check(std::string(view<BButton>(review,"retry draft save")->Label())=="Stop checking…","discarded uncertain draft offers Stop checking");
            view<BButton>(review,"retry draft save")->Invoke(); next(33);
        } else if (fStep==33) {
            if (confirm(false,"Stop checking discarded draft")) next(34);
        } else if (fStep==34) {
            if (window<BAlert>() || !idle()) return;
            check(fStore->gmailDraftState(fRecoveryId).attempt=="create" && fixture("stats")["delete"]==0,"Cancel keeps the discarded save unresolved and removes no server copy");
            press("retry draft save"); next(35);
        } else if (fStep==35) {
            if (confirm(true,"Stop checking discarded draft")) next(36);
        } else if (fStep==36) {
            if (window<BAlert>() || !idle()) return;
            for (const auto& state:fStore->gmailDraftStates()) if (state.id==fRecoveryId) return;
            if (fStore->drafts().size()!=5) return;
            check(fixture("stats")["create"]==5 && fixture("stats")["delete"]==0,"Stop checking keeps the Gmail copy and a later check imports it without another write");
            if (auto* review=window<DraftWindow>()) { if (review->Lock()) review->Quit(); }
            if (fMail->Lock()) { fMail->Quit(); fMail=nullptr; }
            RemoveCredentials(fAccount); result=0; fTimer.reset();
            std::printf("Native Gmail drafts: %d checks passed (import, background save, editor protection, conflicts, copy selection, retry and discarded save recovery)\n",checks); std::fflush(stdout);
            PostMessage(B_QUIT_REQUESTED);
        }
    }
    std::string fPath,fBase; GoogleOptions fOptions; std::atomic<bool> fCancel{false};
    std::shared_ptr<Store> fStore; Account fAccount; GmailDraftState fState;
    MailWindow* fMail=nullptr; ComposerWindow* fComposer=nullptr; BMessenger fComposerMessenger;
    std::unique_ptr<BMessageRunner> fTimer; int fStep=0,fTicks=0;
    bigtime_t fOpened=0;
    Id fRecoveryLocal=0,fRecoveryId=0,fCheckedRevision=0; std::string fOriginalMessage;
};
}
int main(int argc,char** argv)
{
    if (argc!=4) return 2;
    char directory[]="/tmp/mail-native-drafts-XXXXXX"; if (!mkdtemp(directory)) return 2;
    int result; { Test test(directory,argv); test.Run(); result=test.result; }
    std::filesystem::remove_all(directory); return result;
}
