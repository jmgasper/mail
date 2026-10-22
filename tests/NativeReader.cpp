#include "MessagePreview.h"
#include "PreviewLifecycle.h"
#include "MailWindow.h"
#include "mail/Mime.h"
#include <WebKit/WebKitView.h>
#include <Application.h>
#include <Autolock.h>
#include <Bitmap.h>
#include <BitmapStream.h>
#include <Button.h>
#include <File.h>
#include <ListView.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <MessageRunner.h>
#include <Roster.h>
#include <Screen.h>
#include <StringView.h>
#include <TranslatorRoster.h>
#include <nlohmann/json.hpp>
#include <unistd.h>
#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace mail;
using namespace mail::ui;
using Json=nlohmann::json;
namespace {
int checks=0;
void check(bool ok,const char* why) {++checks;if(!ok)throw std::runtime_error(why);}
class ObservedReader : public MessagePreviewWindow {
public:
    using MessagePreviewWindow::MessagePreviewWindow;
    void MessageReceived(BMessage* message) override {
        if(message->what==B_WEBKIT_STATE_CHANGED || message->what==B_WEBKIT_FIND_RESULT) be_app->PostMessage(message);
        MessagePreviewWindow::MessageReceived(message);
    }
};
class ReaderTest : public BApplication {
public:
    ReaderTest(std::string directory,std::string png,std::string probe):BApplication("application/x-vnd.air-Mail-Reader-Test"),fDirectory(std::move(directory)),fPng(std::move(png)),fProbe(std::move(probe)) {}
    int result=1;
    void ReadyToRun() override
    {
        try {
            std::ifstream input(fPng,std::ios::binary);std::string png{std::istreambuf_iterator<char>(input),{}};check(!png.empty(),"read native image fixture");
            fSnapshot.key="native-received-fixture";fSnapshot.received.emplace();auto& m=*fSnapshot.received;
            m.id=7;m.from="Ada <ada@example.test>";m.to="Reader <reader@example.test>";m.cc="copy@example.test";m.subject="Received garden update 🎉";m.date="21 October 2026, 10:30";
            m.text="Original plain-text alternative café 🎉\nBefore image. After image.";
            m.thread={{6,"Earlier author","20 October 2026"},{7,m.from,m.date}};
            m.files={{"Garden.png","image/png","garden@test",png,false},{"Wide.svg","image/svg+xml","wide@test","<svg xmlns='http://www.w3.org/2000/svg' width='1200' height='40'><rect width='1200' height='40' fill='#2677bc'/><image href='"+fProbe+"/svg' width='1' height='1'/></svg>",true}};
            m.html="<!doctype html SYSTEM '"+fProbe+"/external.dtd'><html><head><style>@import url('"+fProbe+"/style');#notice{color:rgb(18,52,86);background-image:url('"+fProbe+"/background')}</style>"
                "<link rel=preconnect href='"+fProbe+"'><meta http-equiv=refresh content='0;url="+fProbe+"/refresh'></head><body>"
                "<table cellpadding=4><tr><td id=notice><b>Received café 🎉</b></td></tr></table><p>Before image.</p><img src='CID:garden%40test' width=240 alt='Garden'><p>After image.</p><img src='cid:wide@test'>"
                "<img src='"+fProbe+"/pixel' alt='Remote photo'><img srcset='"+fProbe+"/srcset 1x' src='file:///boot/home/secret'>"
                "<a href='"+fProbe+"/link' ping='"+fProbe+"/ping'>Read more</a><script>window.mailInjected=true;fetch('"+fProbe+"/script')</script>"
                "<iframe src='"+fProbe+"/frame'></iframe><form action='"+fProbe+"/form'><input autofocus onfocus='window.mailInjected=true'></form>"
                "<svg><script>window.mailInjected=true</script></svg><math><mtext><img onerror='window.mailInjected=true' src=x></mtext></math></body></html>";
            auto path=WritePreviewTransfer(fSnapshot,BMessenger(this));auto transfer=ReadPreviewTransfer(path);
            check(transfer.snapshot.received && transfer.snapshot.received->html==m.html && !std::filesystem::exists(path),"received transfer preserves source and removes the private file");
            check(BWebKitInitialize()==B_OK,"initialize native renderer");fContext=std::make_shared<BWebKitContext>(nullptr,true);check(fContext->InitCheck()==B_OK,"private reader context");
            fPreview=new ObservedReader(std::move(transfer),fContext);fPreview->Show();
            BMessage tick('tick');fTimer=std::make_unique<BMessageRunner>(BMessenger(this),&tick,100000);
        }catch(const std::exception& e){Fail(e.what());}
    }
    void MessageReceived(BMessage* message) override
    {
        if(fFailed)return;
        try {
            if(message->what==B_WEBKIT_STATE_CHANGED) {
                auto url=std::string(message->GetString("url",""));
                fLoaded=url.rfind("mail-preview://",0)==0 && !message->GetBool("loading",true);
                return;
            }
            if(message->what==B_WEBKIT_FIND_RESULT) {
                if(fQuery==message->GetString("query","")) {fPending=false;fFound=message->GetBool("found",false);}
                return;
            }
            if(message->what==B_WEBKIT_JAVASCRIPT_RESULT) {
                check(std::string(message->GetString("error",""))=="Cannot execute JavaScript in this document","received sandbox rejects JavaScript execution, including embedding inspection");
                fScriptBlocked=true;return;
            }
            if(message->what==kPreviewSelect || message->what==kPreviewRefresh) {
                check(std::string(message->GetString("preview-key",""))==fSnapshot.key,"received command identifies its source conversation");
                auto next=fSnapshot;
                if(message->what==kPreviewSelect) {
                    check(message->GetInt64("message",0)==6,"conversation picker requests the chosen message");
                    next.received->id=6;next.received->subject="Earlier message";next.received->html="<p>Earlier formatted message</p>";next.received->text="Earlier plain message";next.received->files.clear();
                }else check(message->GetInt64("message",0)==6,"refresh retains the currently selected message identity");
                BAutolock lock(fPreview);fLoaded=false;fQuery.clear();fPreview->SetSnapshot({std::move(next),BMessenger(this)});return;
            }
            if(message->what==kPreviewClosed){ClosePreview();return;}
            if(message->what!='tick'){BApplication::MessageReceived(message);return;}
            if(++fTicks>900)throw std::runtime_error("Native reader timed out");
            if(fStep==0) {
                if(!Ready("Received café"))return;
                if(!fScriptRequested) {
                    BAutolock lock(fPreview);auto* web=dynamic_cast<BWebKitView*>(fPreview->FindView("message preview body"));
                    web->EvaluateJavaScript("window.mailInjected=true",BMessenger(this),1);fScriptRequested=true;return;
                }
                if(!fScriptBlocked)return;
                check(true,"formatted received text is searchable without executing script");
                {BAutolock lock(fPreview);auto* edit=fPreview->FindView("preview edit");auto* picker=dynamic_cast<BMenuField*>(fPreview->FindView("preview conversation"));
                    check(edit && edit->IsHidden(edit) && picker && picker->Menu()->CountItems()==2 && picker->Menu()->ItemAt(1)->IsMarked(),"received window has conversation selection and no draft-edit command");
                    check(std::string(dynamic_cast<BStringView*>(fPreview->FindView("preview date"))->Text())==fSnapshot.received->date,"received date appears outside sender HTML");}
                fStep=1;fPaint=0;
            }else if(fStep==1) {
                if(++fPaint<5)return;
                Capture();TogglePlain();fStep=2;
            }else if(fStep==2) {
                if(!Ready("Original plain-text alternative"))return;
                check(!ImagePixels(),"plain-text toggle removes rendered images");TogglePlain();fStep=3;
            }else if(fStep==3) {
                if(!Ready("Received café") || !ImagePixels())return;
                check(true,"formatted toggle restores attached images");
                {BAutolock lock(fPreview);auto* picker=dynamic_cast<BMenuField*>(fPreview->FindView("preview conversation"));check(static_cast<BInvoker*>(picker->Menu()->ItemAt(0))->Invoke()==B_OK,"invoke native conversation picker");}
                fQuery.clear();fStep=4;
            }else if(fStep==4) {
                if(!Ready("Earlier formatted message"))return;
                check(!ImagePixels(),"switching messages removes previous inline resources");fPreview->PostMessage(kPreviewRefresh);fQuery.clear();fStep=5;
            }else if(fStep==5) {
                if(!Ready("Received café") || !ImagePixels())return;
                auto nested=fSnapshot;for(int i=0;i<140;++i)nested.received->html="<div>"+nested.received->html+"</div>";
                {BAutolock lock(fPreview);fLoaded=false;fPreview->SetSnapshot({std::move(nested),BMessenger(this)});}fQuery.clear();fStep=6;
            }else if(fStep==6) {
                if(!Ready("Original plain-text alternative"))return;
                check(!ImagePixels(),"overly complex HTML falls back to its plain alternative");fPreview->PostMessage(B_QUIT_REQUESTED);fStep=7;
            }else if(fStep==7) {
                if(fPreview)return;
                fContext.reset();fStep=8;
            }else if(fStep==8) {
                if(BWebKitHasPendingNativeUI() || PreviewHasPendingRenderer())return;
                StartMail();fStep=9;
            }else if(fStep==9) {
                BAutolock lock(fMail);auto* list=dynamic_cast<BListView*>(fMail->FindView("conversations"));if(!list || list->CountItems()!=1)return;
                list->Select(0);fStep=10;
            }else if(fStep==10) {
                BAutolock lock(fMail);auto* button=dynamic_cast<BButton*>(fMail->FindView("formatted conversation"));if(!button || !button->IsEnabled())return;
                check(button->Invoke()==B_OK,"main reader opens formatted conversation through its native button");fStep=11;
            }else if(fStep==11) {
                if(!Helper("Message — Latest received message"))return;
                fFirst=fHelper;
                check(fStore->message(fEarlier).unread && fStore->message(fLatest).unread,"formatted inspection does not alter unread state");
                BMessage select(kPreviewSelect);select.AddInt64("message",fEarlier);check(fHelper.SendMessage(&select)==B_OK,"companion requests an earlier cached message");fStep=12;
            }else if(fStep==12) {
                if(!Helper("Message — Earlier received message"))return;
                check(fHelper==fFirst,"conversation navigation reuses the companion window");
                check(fHelper.SendMessage(kPreviewRefresh)==B_OK,"refresh received message through the actual Mail owner");fAfter=system_time()+300000;fStep=13;
            }else if(fStep==13) {
                if(system_time()<fAfter || !Helper("Message — Earlier received message"))return;
                check(fHelper==fFirst,"refresh preserves the selected received message");check(fHelper.SendMessage(B_QUIT_REQUESTED)==B_OK,"close received companion");fStep=14;
            }else if(fStep==14) {
                app_info info;if(be_roster->GetAppInfo(kPreviewSignature,&info)==B_OK)return;
                check(true,"received companion drains its renderer and exits");
                {BAutolock lock(fMail);auto* list=dynamic_cast<BListView*>(fMail->FindView("conversations"));check(list->Invoke()==B_OK,"invoke main conversation row");}fStep=15;
            }else if(fStep==15) {
                if(!Helper("Message — Latest received message"))return;
                check(true,"double-click invocation opens a received conversation again");fHelper.SendMessage(B_QUIT_REQUESTED);fStep=16;
            }else if(fStep==16) {
                app_info info;if(be_roster->GetAppInfo(kPreviewSignature,&info)==B_OK)return;
                result=0;std::printf("Native received reader: %d checks passed (HTML, CID pixels, privacy, plain text, thread picker, fallback, Mail integration and helper lifecycle)\n",checks);std::fflush(stdout);
                fTimer.reset();PostMessage(B_QUIT_REQUESTED);fStep=17;
            }
        }catch(const std::exception& e){Fail(e.what());}
    }
    bool QuitRequested() override
    {
        if(fMail){if(fMail->Lock())fMail->Quit();fMail=nullptr;}ClosePreview();
        if(fContext){fContext.reset();PostMessage(B_QUIT_REQUESTED);return false;}
        if(BWebKitHasPendingNativeUI() || PreviewHasPendingRenderer()){fExitReady=false;if(!fQuitTimer){BMessage q(B_QUIT_REQUESTED);fQuitTimer=std::make_unique<BMessageRunner>(BMessenger(this),&q,150000);}return false;}
        fQuitTimer.reset();if(!fExitReady){fExitReady=true;PostMessage(B_QUIT_REQUESTED);return false;}return true;
    }
private:
    bool Ready(const std::string& query)
    {
        if(!fLoaded)return false;
        if(fQuery!=query || (!fPending && !fFound)) {
            BAutolock lock(fPreview);auto* web=dynamic_cast<BWebKitView*>(fPreview->FindView("message preview body"));
            if(!web)throw std::runtime_error("Reader WebKit view missing");
            fQuery=query;fPending=true;fFound=false;web->FindString(query.c_str());return false;
        }
        if(fPending || !fFound)return false;
        {BAutolock lock(fPreview);dynamic_cast<BWebKitView*>(fPreview->FindView("message preview body"))->HideFindUI();}
        return true;
    }
    void TogglePlain(){BAutolock lock(fPreview);auto* button=dynamic_cast<BButton*>(fPreview->FindView("preview plain text"));check(button && button->IsEnabled() && button->Invoke()==B_OK,"invoke native plain/HTML toggle");fLoaded=false;fQuery.clear();}
    void ClosePreview(){if(fPreview){if(fPreview->Lock())fPreview->Quit();fPreview=nullptr;}}
    void Fail(const char* why){fFailed=true;std::fprintf(stderr,"FAIL reader step %d: %s (query=%s found=%d loaded=%d)\n",fStep,why,fQuery.c_str(),fFound,fLoaded);std::fflush(stderr);fTimer.reset();BMessenger(kPreviewSignature).SendMessage(B_QUIT_REQUESTED);PostMessage(B_QUIT_REQUESTED);}
    bool ImagePixels(bool capture=false)
    {
        BBitmap* raw=nullptr;if(BScreen().GetBitmap(&raw)!=B_OK)throw std::runtime_error("Cannot capture reader pixels");std::unique_ptr<BBitmap> bitmap(raw);BRect area;
        {BAutolock lock(fPreview);auto* web=fPreview->FindView("message preview body");area=web->ConvertToScreen(web->Bounds()) & bitmap->Bounds();}
        auto pixel=[&](int x,int y){const auto* b=static_cast<const uint8*>(bitmap->Bits())+y*bitmap->BytesPerRow()+x*4;return rgb_color{b[2],b[1],b[0],255};};
        int left=100000,top=100000,right=-1,bottom=-1;bool styled=false;
        for(int y=area.top;y<=area.bottom;++y)for(int x=area.left;x<=area.right;++x){auto p=pixel(x,y);if(p.red>200 && p.green<100 && p.blue<100){left=std::min(left,x);right=std::max(right,x);top=std::min(top,y);bottom=std::max(bottom,y);}if(p.red==18 && p.green==52 && p.blue==86)styled=true;}
        if(right-left<10 || bottom-top<10)return false;
        int cx=(left+right)/2,cy=(top+bottom)/2,dx=right-left+1,dy=bottom-top+1;
        if(!area.Contains(BPoint(cx+dx,cy+dy)))return false;
        auto green=pixel(cx+dx,cy),blue=pixel(cx,cy+dy),yellow=pixel(cx+dx,cy+dy);
        if(!(green.green>130 && green.red<80 && blue.blue>170 && blue.red<80 && yellow.red>200 && yellow.green>140))return false;
        if(capture){
            check(true,"received CID image is painted with all four expected colours");check(styled,"received stylesheet colour appears on screen");
            if(const char* path=std::getenv("MAIL_READER_SCREENSHOT")){BBitmapStream stream(bitmap.release());BFile file(path,B_WRITE_ONLY|B_CREATE_FILE|B_ERASE_FILE);check(BTranslatorRoster::Default()->Translate(&stream,nullptr,nullptr,&file,B_PNG_FORMAT)==B_OK,"save received reader screenshot");}
        }
        return true;
    }
    void Capture(){check(ImagePixels(true),"attached image paints in received reader");}
    void StartMail()
    {
        fStore=std::make_shared<Store>(fDirectory+"/mail.db");Account a;a.name="Received fixture";a.provider=Provider::Local;a.email="reader@example.test";a.id=fStore->saveAccount(a);
        auto make=[&](bool latest){auto m=parseMessage("From: Author <author@example.test>\r\nTo: reader@example.test\r\nSubject: "+std::string(latest?"Latest received message":"Earlier received message")+"\r\nMessage-ID: <"+(latest?std::string("latest"):std::string("earlier"))+"@reader.test>\r\n"+(latest?"References: <earlier@reader.test>\r\n":"")+"Content-Type: text/html; charset=utf-8\r\n\r\n<p>Received <b>body</b> café 🎉</p>");m.account=a.id;m.remoteKey=latest?"latest":"earlier";m.received=latest?1792560001:1792560000;return fStore->ingest(m);};
        fEarlier=make(false);fLatest=make(true);check(fStore->thread(fStore->message(fLatest).thread).size()==2,"real MIME messages form one cached thread");
        fMail=new MailWindow(fStore);fMail->MoveTo(10,35);fMail->Show();
    }
    bool Helper(const std::string& title)
    {
        BMessenger helper(kPreviewSignature);if(!helper.IsValid())return false;BMessage query(B_GET_PROPERTY),answer;query.AddSpecifier("Windows");if(helper.SendMessage(&query,&answer,1000000,1000000)!=B_OK)return false;
        BMessenger window,extra;if(answer.FindMessenger("result",0,&window)!=B_OK)return false;if(answer.FindMessenger("result",1,&extra)==B_OK)throw std::runtime_error("Multiple received windows for one conversation");
        BMessage request(B_GET_PROPERTY),reply;request.AddSpecifier("Title");if(window.SendMessage(&request,&reply,1000000,1000000)!=B_OK || title!=reply.GetString("result",""))return false;fHelper=window;return true;
    }
    std::string fDirectory,fPng,fProbe;PreviewSnapshot fSnapshot;std::string fQuery;
    std::shared_ptr<BWebKitContext> fContext;std::shared_ptr<Store> fStore;MessagePreviewWindow* fPreview=nullptr;MailWindow* fMail=nullptr;
    std::unique_ptr<BMessageRunner> fTimer,fQuitTimer;BMessenger fHelper,fFirst;Id fEarlier=0,fLatest=0;
    int fStep=0,fTicks=0,fPaint=0;bigtime_t fAfter=0;bool fPending=false,fFound=false,fLoaded=false,fScriptRequested=false,fScriptBlocked=false,fExitReady=false,fFailed=false;
};
}
int main(int argc,char** argv)
{
    if(argc!=3)return 2;
    char directory[]="/tmp/mail-reader-ui-XXXXXX";if(!mkdtemp(directory))return 2;
    int result;{ReaderTest app(directory,argv[1],argv[2]);app.Run();result=app.result;}std::filesystem::remove_all(directory);return result;
}
