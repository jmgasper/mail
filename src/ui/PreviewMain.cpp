#include "MessagePreview.h"
#include "PreviewLifecycle.h"
#include <WebKit/WebKitView.h>
#include <Alert.h>
#include <Application.h>
#include <Autolock.h>
#include <MessageRunner.h>
#include <algorithm>
#include <cstdio>
#include <memory>
#include <vector>

namespace {
class PreviewApplication : public BApplication {
public:
    explicit PreviewApplication(std::string path):BApplication(mail::ui::kPreviewSignature),fPending{std::move(path)} {}
    void ArgvReceived(int32 argc,char** argv) override
    {
        if(argc!=2) return;
        if(!fReady) { if(std::find(fPending.begin(),fPending.end(),argv[1])==fPending.end()) fPending.emplace_back(argv[1]); }
        else Open(argv[1]);
    }
    void ReadyToRun() override
    {
        fReady=true; auto pending=std::move(fPending); for(const auto& path:pending) Open(path);
    }
    void Open(const std::string& path)
    {
        try {
            auto preview=mail::ui::ReadPreviewTransfer(path);
            if(!fContext) {
                if(BWebKitInitialize()!=B_OK) throw std::runtime_error("Could not initialize the message renderer.");
                fContext=std::make_shared<BWebKitContext>(nullptr,true);
                if(fContext->InitCheck()!=B_OK) throw std::runtime_error("Could not create a private message preview.");
            }
            fExitReady=false; fQuitTimer.reset();
            for(auto* window:fWindows) {
                BAutolock lock(window);
                if(lock.IsLocked() && window->Key()==preview.snapshot.key) { window->SetSnapshot(std::move(preview)); window->Activate(); return; }
            }
            auto* window=new mail::ui::MessagePreviewWindow(std::move(preview),fContext); fWindows.push_back(window); window->Show();
        } catch(const std::exception& e) {
            std::fprintf(stderr,"Mail preview: %s\n",e.what()); (new BAlert("Message preview",e.what(),"Close"))->Go();
            if(fWindows.empty()) ScheduleExit();
        }
    }
    void MessageReceived(BMessage* message) override
    {
        if(message->what==mail::ui::kPreviewClosed) {
            BMessenger target; message->FindMessenger("window",&target);
            auto found=std::find_if(fWindows.begin(),fWindows.end(),[&](auto* window){return BMessenger(window)==target;});
            if(found!=fWindows.end()) { auto* window=*found; fWindows.erase(found); if(window->Lock()) window->Quit(); }
            if(fWindows.empty()) ScheduleExit();
            return;
        }
        BApplication::MessageReceived(message);
    }
    bool QuitRequested() override
    {
        // An argv delivery can reopen a preview while an automatic shutdown
        // message is queued. Only an explicit quit should close that new view.
        if(CurrentMessage() && CurrentMessage()->GetBool("idle-exit",false) && !fWindows.empty()) return false;
        if(!fWindows.empty()) {
            auto windows=std::move(fWindows); for(auto* window:windows) if(window->Lock()) window->Quit();
            ScheduleExit(); return false;
        }
        if(fContext) { fContext.reset(); ScheduleExit(); return false; }
        if(BWebKitHasPendingNativeUI() || mail::ui::PreviewHasPendingRenderer()) {
            if(!fQuitTimer) { BMessage quit(B_QUIT_REQUESTED); quit.AddBool("idle-exit",true); fQuitTimer=std::make_unique<BMessageRunner>(BMessenger(this),&quit,100000); }
            fExitReady=false; return false;
        }
        fQuitTimer.reset();
        if(!fExitReady) { fExitReady=true; ScheduleExit(); return false; }
        return true;
    }
private:
    void ScheduleExit() { BMessage quit(B_QUIT_REQUESTED); quit.AddBool("idle-exit",true); PostMessage(&quit); }
    std::vector<std::string> fPending;
    std::vector<mail::ui::MessagePreviewWindow*> fWindows;
    std::shared_ptr<BWebKitContext> fContext;
    std::unique_ptr<BMessageRunner> fQuitTimer;
    bool fReady=false,fExitReady=false;
};
}
int main(int argc,char** argv)
{
    if(argc!=2) { std::fprintf(stderr,"MailPreview is opened by Mail's Preview message command.\n"); return 2; }
    PreviewApplication app(argv[1]); app.Run(); return 0;
}
