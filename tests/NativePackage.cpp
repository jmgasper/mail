#include "mail/Store.h"
#include <AppFileInfo.h>
#include <Application.h>
#include <Bitmap.h>
#include <BitmapStream.h>
#include <Entry.h>
#include <File.h>
#include <MessageRunner.h>
#include <Path.h>
#include <Resources.h>
#include <Roster.h>
#include <Screen.h>
#include <TranslatorRoster.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>

namespace {
constexpr const char* kMail="application/x-vnd.air-Mail";
constexpr const char* kPreview="application/x-vnd.air-Mail-Preview";
constexpr const char* kInstalled="/boot/system/apps/airOS-Mail/";
int checks=0;
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
std::string path(const entry_ref& ref){BEntry entry(&ref,true);BPath result;check(entry.GetPath(&result)==B_OK,"resolve executable path");return result.Path();}
BMessenger window(const char* signature,const std::string& title)
{
    BMessenger app(signature);BMessage query(B_GET_PROPERTY),reply;query.AddSpecifier("Windows");
    if(!app.IsValid() || app.SendMessage(&query,&reply,1000000,1000000)!=B_OK)return {};
    BMessenger result;
    for(int i=0;reply.FindMessenger("result",i,&result)==B_OK;++i){
        BMessage request(B_GET_PROPERTY),answer;request.AddSpecifier("Title");
        if(result.SendMessage(&request,&answer,1000000,1000000)==B_OK && title==answer.GetString("result",""))return result;
    }
    return {};
}
void metadata(const char* app,const char* signature)
{
    BFile file((std::string(kInstalled)+app).c_str(),B_READ_ONLY);check(file.InitCheck()==B_OK,"installed executable exists");
    BAppFileInfo info(&file);char type[B_MIME_TYPE_LENGTH]={};
    check(info.GetSignature(type)==B_OK && std::string(type)==signature,"package MIME attributes preserve the app signature");
    uint32 flags=0;check(info.GetAppFlags(&flags)==B_OK && (flags&B_LAUNCH_MASK)==B_SINGLE_LAUNCH,"package keeps single-launch flags");
    for(int size:{16,32}){BBitmap icon(BRect(0,0,size-1,size-1),B_RGBA32);check(info.GetIcon(&icon,size==16?B_MINI_ICON:B_LARGE_ICON)==B_OK,"Tracker icon survives stripping and packaging");}
    BResources resources(&file);size_t bytes=0;
    check(resources.LoadResource('RAWT',"Mail:NotoEmoji",&bytes) && bytes>100000,"packaged font resource survives stripping");
    check(resources.LoadResource('TEXT',"Mail:NotoEmoji:OFL",&bytes) && bytes>1000,"packaged font license survives stripping");
}
bool previewPainted(const BMessenger& target)
{
    BMessage request(B_GET_PROPERTY),reply;request.AddSpecifier("Frame");BRect frame;
    auto status=target.SendMessage(&request,&reply,1000000,1000000);
    check(status==B_OK && reply.FindRect("result",&frame)==B_OK,"read packaged preview window frame");
    BBitmap* raw=nullptr;if(BScreen().GetBitmap(&raw)!=B_OK)return false;std::unique_ptr<BBitmap> bitmap(raw);
    frame=frame&bitmap->Bounds();auto* pixels=static_cast<const uint8*>(bitmap->Bits());int red=0,green=0,blue=0,yellow=0;
    for(int y=std::max(0,int(frame.top));y<=int(frame.bottom);++y)for(int x=std::max(0,int(frame.left));x<=int(frame.right);++x){
        const auto* p=pixels+y*bitmap->BytesPerRow()+x*4;
        red+=p[2]>200 && p[1]<100;green+=p[1]>130 && p[2]<80;
        blue+=p[0]>170 && p[2]<80;
        // The fixture's yellow quadrant is translucent over the white page.
        yellow+=p[2]>235 && p[1]>210 && p[1]<240 && p[0]>150 && p[0]<200;
    }
    static int polls=0;if(++polls%20==0)std::fprintf(stderr,"Preview paint at %.0f,%.0f–%.0f,%.0f: red %d green %d blue %d yellow %d\n",frame.left,frame.top,frame.right,frame.bottom,red,green,blue,yellow);
    return red>500 && green>500 && blue>500 && yellow>500;
}
class Test:public BApplication {
public:
    Test(std::string database,std::string image,bool seed):BApplication("application/x-vnd.air-Mail-Package-Test"),fDatabase(std::move(database)),fImage(std::move(image)),fSeed(seed){}
    int result=1;
    void ReadyToRun() override
    {
        try{
            check(!be_roster->IsRunning(kMail) && !be_roster->IsRunning(kPreview),"test starts with Mail and helper closed");
            metadata("Mail",kMail);metadata("MailPreview",kPreview);
            if(fSeed)check(!std::filesystem::exists(fDatabase),"seed uses a fresh disposable database");
            fStore=std::make_unique<mail::Store>(fDatabase);
            if(fSeed){
                mail::Account account;account.name="Package fixture";account.email="author@example.test";account.id=fStore->saveAccount(account);
                mail::Draft draft;draft.account=account.id;draft.to="recipient@example.test";draft.subject="Installed Mail 👩🏽‍💻";draft.text="This draft survives package upgrades.\n";
                draft.runs.push_back({0,true,false,false,{}});draft.runs.push_back({4,false,false,false,{}});
                std::ifstream input(fImage,std::ios::binary);std::string data((std::istreambuf_iterator<char>(input)),{});check(data.size()>100,"load fixture image");
                draft.attachments.push_back({"Package image.png","image/png","package-image@example.test",data,true});
                auto marker=mail::imagePlaceholder(draft.attachments[0]);draft.images.push_back({draft.text.size(),marker.size(),draft.attachments[0].contentId});draft.text+=marker;
                fStore->saveDraft(draft);
            }
            auto drafts=fStore->drafts();check(drafts.size()==1,"exactly one fixture draft persists");fDraft=drafts[0];
            if(!fSeed)check(fDraft.text.find("🇦🇺")!=std::string::npos,"previous installed app's edit survived package upgrade");
            // The installed app owns this database while running. Inspect its
            // writes only after exit, avoiding a second native SQLite writer.
            fStore.reset();fStarted=system_time();
            BEntry entry("/boot/system/data/deskbar/menu/Applications/airOS/Mail",true);entry_ref ref;
            check(entry.GetRef(&ref)==B_OK && path(ref)==std::string(kInstalled)+"Mail","Deskbar link resolves to the packaged Mail");
            const char* args[]={"--database",fDatabase.c_str()};
            auto status=fSeed?be_roster->Launch(&ref,2,args,&fTeam):be_roster->Launch(kMail,2,args,&fTeam);
            check(status==B_OK,"launch packaged Mail through Deskbar or registered signature after upgrade");
            BMessage tick('tick');fTimer=std::make_unique<BMessageRunner>(BMessenger(this),&tick,200000);
        }catch(const std::exception& error){Fail(error.what());}
    }
    void MessageReceived(BMessage* message) override
    {
        if(message->what!='tick'){BApplication::MessageReceived(message);return;}
        if(fFailed)return;
        try{
            check(system_time()-fStarted<50000000,"packaged app interaction timed out");
            if(fStep==0){
                auto main=window(kMail,"Mail");if(!main.IsValid())return;
                app_info info;check(be_roster->GetAppInfo(kMail,&info)==B_OK && info.team==fTeam && path(info.ref)==std::string(kInstalled)+"Mail","roster runs the packaged executable");
                BMessage open('mopd');open.AddInt64("draft",fDraft.id);check(main.SendMessage(&open)==B_OK,"open persisted draft in installed app");++fStep;
            }else if(fStep==1){
                fComposer=window(kMail,fDraft.subject);if(!fComposer.IsValid())return;
                if(fSeed){BMessage emoji('cemo');emoji.AddString("emoji","🇦🇺");check(fComposer.SendMessage(&emoji)==B_OK,"edit draft through real composer command");}
                check(fComposer.SendMessage('pvrf')==B_OK,"installed composer opens its adjacent preview helper");++fStep;
            }else if(fStep==2){
                if(!window(kPreview,"Preview — "+fDraft.subject).IsValid())return;
                app_info info;check(be_roster->GetAppInfo(kPreview,&info)==B_OK && path(info.ref)==std::string(kInstalled)+"MailPreview","roster runs the packaged helper");++fStep;
            }else if(fStep==3){
                if(!previewPainted(window(kPreview,"Preview — "+fDraft.subject)))return;
                check(true,"packaged preview paints all four fixture image quadrants");
                if(const char* filename=std::getenv("MAIL_PACKAGE_SCREENSHOT")){
                    BBitmap* bitmap=nullptr;check(BScreen().GetBitmap(&bitmap)==B_OK,"capture installed composer and preview");BBitmapStream stream(bitmap);BFile output(filename,B_WRITE_ONLY|B_CREATE_FILE|B_ERASE_FILE);
                    check(BTranslatorRoster::Default()->Translate(&stream,nullptr,nullptr,&output,B_PNG_FORMAT)==B_OK,"save installed app capture");
                }
                check(BMessenger(kPreview).SendMessage(B_QUIT_REQUESTED)==B_OK,"close packaged helper");++fStep;
            }else if(fStep==4){
                if(be_roster->IsRunning(kPreview))return;
                check(fComposer.SendMessage(B_QUIT_REQUESTED)==B_OK,"close and save installed composer");++fStep;
            }else if(fStep==5){
                if(window(kMail,fDraft.subject).IsValid())return;
                check(BMessenger(kMail).SendMessage(B_QUIT_REQUESTED)==B_OK,"close packaged Mail");++fStep;
            }else if(fStep==6){
                if(be_roster->IsRunning(kMail))return;
                fStore=std::make_unique<mail::Store>(fDatabase);
                auto saved=fStore->draft(fDraft.id);
                check(saved.text.find("🇦🇺")!=std::string::npos && saved.subject==fDraft.subject,"installed composer saved exact emoji and subject");
                check(saved.attachments.size()==1 && saved.attachments[0].data==fDraft.attachments[0].data && saved.images.size()==1 && !saved.runs.empty(),"installed app retains image bytes, placement and formatting");
                fTimer.reset();result=0;std::printf("Native package: %d checks passed (%s, Deskbar launch, metadata, installed app/helper, draft edit and persistence)\n",checks,fSeed?"fresh install":"after upgrade");PostMessage(B_QUIT_REQUESTED);
            }
        }catch(const std::exception& error){Fail(error.what());}
    }
private:
    void Fail(const char* why){if(fFailed)return;fFailed=true;fTimer.reset();std::fprintf(stderr,"FAIL package step %d: %s\n",fStep,why);
        if(const char* filename=std::getenv("MAIL_PACKAGE_SCREENSHOT")){BBitmap* bitmap=nullptr;if(BScreen().GetBitmap(&bitmap)==B_OK){BBitmapStream stream(bitmap);BFile output((std::string(filename)+".failed.png").c_str(),B_WRITE_ONLY|B_CREATE_FILE|B_ERASE_FILE);BTranslatorRoster::Default()->Translate(&stream,nullptr,nullptr,&output,B_PNG_FORMAT);}}
        BMessenger(kPreview).SendMessage(B_QUIT_REQUESTED);BMessenger(kMail).SendMessage(B_QUIT_REQUESTED);PostMessage(B_QUIT_REQUESTED);}
    std::string fDatabase,fImage;bool fSeed,fFailed=false;int fStep=0;team_id fTeam=-1;bigtime_t fStarted=0;
    mail::Draft fDraft;std::unique_ptr<mail::Store> fStore;BMessenger fComposer;std::unique_ptr<BMessageRunner> fTimer;
};
}
int main(int argc,char** argv)
{
    if(argc!=4 || (std::string(argv[1])!="seed" && std::string(argv[1])!="verify")){std::fprintf(stderr,"Usage: mail_native_package seed|verify DATABASE FIXTURE_PNG\n");return 2;}
    Test app(argv[2],argv[3],std::string(argv[1])=="seed");app.Run();return app.result;
}
