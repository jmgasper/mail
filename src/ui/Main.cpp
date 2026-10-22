#include "MailWindow.h"
#include "SnoozeWindow.h"
#include "ReaderView.h"
#include <Alert.h>
#include <Application.h>
#include <Directory.h>
#include <Entry.h>
#include <MessageRunner.h>
#include <FindDirectory.h>
#include <Path.h>
#include <sys/stat.h>
#include <cstdio>
#include <strings.h>
#include <vector>
#include <stdexcept>

namespace {
struct Arguments { std::string database; bool reminders=false; BMessage files{B_REFS_RECEIVED}; std::vector<std::string> links; };
Arguments parse(int argc,char** argv)
{
    Arguments result;
    for(int i=1;i<argc;++i) {
        auto arg=std::string(argv[i]);
        if(arg=="--reminders") result.reminders=true;
        else if(arg=="--database" && i+1<argc) { BPath path(argv[++i],nullptr,true); if(path.InitCheck()!=B_OK) throw std::invalid_argument("Invalid database path."); result.database=path.Path(); }
        else if (strncasecmp(argv[i],"mailto:",7)==0) result.links.push_back(arg);
        else if (entry_ref ref; arg.rfind("-",0)!=0 && get_ref_for_path(argv[i],&ref)==B_OK && BEntry(&ref).IsFile()) result.files.AddRef("refs",&ref);
        else throw std::invalid_argument("Usage: Mail [--database PATH] [--reminders] [MESSAGE FILE…] [mailto:ADDRESS]");
    }
    return result;
}
class MailApplication : public BApplication {
public:
    explicit MailApplication(Arguments args) : BApplication("application/x-vnd.air-Mail"), fDatabase(std::move(args.database)),fOpenReminders(args.reminders),fFiles(args.files),fLinks(args.links) {}
    void ArgvReceived(int32 argc,char** argv) override
    {
        try {
            auto args=parse(argc,argv);
            if(!fWindow) { if(!args.database.empty()) fDatabase=args.database; fOpenReminders|=args.reminders; }
            else if(!args.database.empty() && args.database!=fDatabase) throw std::invalid_argument("Mail is already using another database. Close it before opening this database.");
            else if(args.reminders) fWindow->PostMessage(mail::ui::kReminders);
            if(args.files.HasRef("refs")) { if(fWindow) fWindow->PostMessage(&args.files); else fFiles=args.files; }
            if(fWindow) for(const auto& link:args.links) OpenLink(link);
            else fLinks=args.links;
        } catch(const std::exception& e) { (new BAlert("Mail",e.what(),"OK"))->Go(); }
    }
    void ReadyToRun() override
    {
        try {
            if (fDatabase.empty()) {
                BPath path;
                if (find_directory(B_USER_SETTINGS_DIRECTORY,&path)!=B_OK) throw std::runtime_error("Could not find your settings folder.");
                path.Append("Mail");
                if (create_directory(path.Path(),0700)!=B_OK) throw std::runtime_error("Could not create your Mail folder.");
                chmod(path.Path(),0700);
                path.Append("mail.db"); fDatabase=path.Path();
            }
            // Without the web engine, messages are shown as plain text.
            mail::ui::InitializeReaderEngine();
            fWindow=new mail::ui::MailWindow(std::make_shared<mail::Store>(fDatabase),{},fDatabase);
            fWindow->Show();
            if(fOpenReminders) fWindow->PostMessage(mail::ui::kReminders);
            if(fFiles.HasRef("refs")) fWindow->PostMessage(&fFiles);
            for(const auto& link:fLinks) OpenLink(link);
        } catch (const std::exception& error) {
            (new BAlert("Mail",error.what(),"Quit",nullptr,nullptr,B_WIDTH_AS_USUAL,B_STOP_ALERT))->Go();
            PostMessage(B_QUIT_REQUESTED);
        }
    }
    void RefsReceived(BMessage* message) override { if (fWindow) fWindow->PostMessage(message); }
    void MessageReceived(BMessage* message) override
    {
        // Lets scripts and other apps ask for these as they would from the menu.
        switch (message->what) {
            case 'macc': case 'mchk': case 'mnew': if (fWindow) fWindow->PostMessage(message); return;
        }
        BApplication::MessageReceived(message);
    }
    bool QuitRequested() override
    {
        // Close the windows, release the web engine, then keep the loop running
        // until its helper processes have finished (at most a few seconds).
        if (!BApplication::QuitRequested()) return false;
        fWindow=nullptr;
        if (!fReleased) { fReleased=true; mail::ui::ShutdownReaderEngine(); }
        if (mail::ui::ReaderEngineBusy() && ++fWaits<50) { RetryQuit(); return false; }
        if (!fDrained) { fDrained=true; RetryQuit(); return false; }
        return true;
    }
private:
    void OpenLink(const std::string& url) { BMessage link(mail::ui::kReaderLink); link.AddString("url",url.c_str()); fWindow->PostMessage(&link); }
    void RetryQuit() { BMessage quit(B_QUIT_REQUESTED); BMessageRunner::StartSending(BMessenger(this),&quit,100000,1); }
    bool fReleased=false,fDrained=false;
    int fWaits=0;
    std::string fDatabase;
    bool fOpenReminders=false;
    BMessage fFiles;
    std::vector<std::string> fLinks;
    mail::ui::MailWindow* fWindow=nullptr;
};
}
int main(int argc,char** argv)
{
    // Separate data location supports disposable native smoke tests.
    umask(0077);
    try { MailApplication app(parse(argc,argv)); app.Run(); return 0; }
    catch(const std::exception& e) { std::fprintf(stderr,"%s\n",e.what()); return 2; }
}
