#include "MailWindow.h"
#include "SnoozeWindow.h"
#include <Alert.h>
#include <Application.h>
#include <Directory.h>
#include <FindDirectory.h>
#include <Path.h>
#include <sys/stat.h>
#include <cstdio>
#include <stdexcept>

namespace {
struct Arguments { std::string database; bool reminders=false; };
Arguments parse(int argc,char** argv)
{
    Arguments result;
    for(int i=1;i<argc;++i) {
        auto arg=std::string(argv[i]);
        if(arg=="--reminders") result.reminders=true;
        else if(arg=="--database" && i+1<argc) { BPath path(argv[++i],nullptr,true); if(path.InitCheck()!=B_OK) throw std::invalid_argument("Invalid database path."); result.database=path.Path(); }
        else throw std::invalid_argument("Usage: Mail [--database PATH] [--reminders]");
    }
    return result;
}
class MailApplication : public BApplication {
public:
    explicit MailApplication(Arguments args) : BApplication("application/x-vnd.air-Mail"), fDatabase(std::move(args.database)),fOpenReminders(args.reminders) {}
    void ArgvReceived(int32 argc,char** argv) override
    {
        try {
            auto args=parse(argc,argv);
            if(!fWindow) { if(!args.database.empty()) fDatabase=args.database; fOpenReminders|=args.reminders; }
            else if(!args.database.empty() && args.database!=fDatabase) throw std::invalid_argument("Mail is already using another database. Close it before opening this database.");
            else if(args.reminders) fWindow->PostMessage(mail::ui::kReminders);
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
            fWindow=new mail::ui::MailWindow(std::make_shared<mail::Store>(fDatabase),{},fDatabase);
            fWindow->Show();
            if(fOpenReminders) fWindow->PostMessage(mail::ui::kReminders);
        } catch (const std::exception& error) {
            (new BAlert("Mail",error.what(),"Quit",nullptr,nullptr,B_WIDTH_AS_USUAL,B_STOP_ALERT))->Go();
            PostMessage(B_QUIT_REQUESTED);
        }
    }
    void RefsReceived(BMessage* message) override { if (fWindow) fWindow->PostMessage(message); }
private:
    std::string fDatabase;
    bool fOpenReminders=false;
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
