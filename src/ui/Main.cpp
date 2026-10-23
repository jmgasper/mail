#include "MailWindow.h"
#include "SnoozeWindow.h"
#include "ReaderView.h"
#include "RepairWindow.h"
#include "mail/Repair.h"
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
            if(!fWindow && !fRepairing) { if(!args.database.empty()) fDatabase=args.database; fOpenReminders|=args.reminders; }
            else if(!args.database.empty() && args.database!=fDatabase) throw std::invalid_argument("Mail is already using another database. Close it before opening this database.");
            else if(!fWindow) fOpenReminders|=args.reminders;
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
            // Damage found during an earlier run is repaired before opening.
            if (mail::StoreNeedsRepair(fDatabase)) Repair();
            else OpenWindow();
        } catch (const std::exception& error) { Fail(error.what()); }
    }
    void RefsReceived(BMessage* message) override { if (fWindow) fWindow->PostMessage(message); }
    void MessageReceived(BMessage* message) override
    {
        // Lets scripts and other apps ask for these as they would from the menu.
        switch (message->what) {
            case 'macc': case 'mchk': case 'mnew': if (fWindow) fWindow->PostMessage(message); return;
            case mail::ui::kRepairDone: Repaired(message); return;
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
    void OpenWindow()
    {
        try {
            fWindow=new mail::ui::MailWindow(std::make_shared<mail::Store>(fDatabase),{},fDatabase);
        } catch (const mail::StoreDamaged&) {
            fWindow=nullptr;
            if (!fRepaired) { Repair(); return; }
            throw;
        }
        fWindow->Show();
        if(fOpenReminders) fWindow->PostMessage(mail::ui::kReminders);
        if(fFiles.HasRef("refs")) fWindow->PostMessage(&fFiles);
        for(const auto& link:fLinks) OpenLink(link);
    }
    void Repair()
    {
        fRepairing=true;
        (new mail::ui::RepairWindow(fDatabase,BMessenger(this)))->Show();
    }
    void Repaired(BMessage* message)
    {
        fRepairing=false; fRepaired=true;
        try {
            if (const char* error; message->FindString("error",&error)==B_OK) {
                std::string text=std::string("Mail could not repair its database.\n\n")+error
                    +"\n\nYou can quit and leave the damaged database as it is, or set it aside and start with an empty database. "
                    "Mail would then need your accounts added again.";
                if ((new BAlert("Mail",text.c_str(),"Start over","Quit",nullptr,B_WIDTH_AS_USUAL,B_STOP_ALERT))->Go()!=0) { PostMessage(B_QUIT_REQUESTED); return; }
                auto aside=mail::SetAsideStore(fDatabase);
                OpenWindow();
                Notify("Mail started with an empty database. The damaged one was kept at:\n"+aside);
                return;
            }
            OpenWindow();
            // Messages on lost pages download again with the next check.
            fWindow->PostMessage('mchk');
            Notify("Mail repaired its damaged database and kept "+std::to_string(message->GetInt64("messages",0))
                +" messages. Anything that could not be recovered downloads again while Mail checks your accounts.\n\n"
                "The damaged copy was kept at:\n"+message->GetString("copy",""));
        } catch (const std::exception& error) { Fail(error.what()); }
    }
    void Notify(const std::string& text) { (new BAlert("Mail",text.c_str(),"OK",nullptr,nullptr,B_WIDTH_AS_USUAL,B_INFO_ALERT))->Go(nullptr); }
    void Fail(const char* error)
    {
        (new BAlert("Mail",error,"Quit",nullptr,nullptr,B_WIDTH_AS_USUAL,B_STOP_ALERT))->Go();
        PostMessage(B_QUIT_REQUESTED);
    }
    void OpenLink(const std::string& url) { BMessage link(mail::ui::kReaderLink); link.AddString("url",url.c_str()); fWindow->PostMessage(&link); }
    void RetryQuit() { BMessage quit(B_QUIT_REQUESTED); BMessageRunner::StartSending(BMessenger(this),&quit,100000,1); }
    bool fReleased=false,fDrained=false;
    int fWaits=0;
    std::string fDatabase;
    bool fOpenReminders=false,fRepairing=false,fRepaired=false;
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
