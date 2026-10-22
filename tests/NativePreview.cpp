#include "MessagePreview.h"
#include "PreviewLifecycle.h"
#include "ComposerWindow.h"
#include "DraftEditor.h"
#include <WebKit/WebKitView.h>
#include <Application.h>
#include <Autolock.h>
#include <Bitmap.h>
#include <BitmapStream.h>
#include <File.h>
#include <MessageRunner.h>
#include <Roster.h>
#include <Screen.h>
#include <StringView.h>
#include <TextControl.h>
#include <TranslatorRoster.h>
#include <nlohmann/json.hpp>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace mail;
using namespace mail::ui;
using Json=nlohmann::json;
namespace {
int checks=0;
void check(bool ok,const char* why) { ++checks; if(!ok) throw std::runtime_error(why); }
const char* inspect=R"JS(JSON.stringify({
 ready:document.readyState==='complete' && document.images.length===2 && [...document.images].every(i=>i.complete && i.naturalWidth>0) && document.fonts.status==='loaded',
 text:document.body.innerText, html:document.body.innerHTML,
 bold:!!document.querySelector('strong'), italic:!!document.querySelector('em'), underline:!!document.querySelector('u'),
 links:document.querySelectorAll('a[href]').length, scripts:document.scripts.length,
 emoji:[...document.fonts].some(f=>f.family==='MailEmoji' && f.status==='loaded'),
 viewport:innerWidth, images:[...document.images].map(i=>{let r=i.getBoundingClientRect();return {natural:i.naturalWidth,x:r.x,y:r.y,w:r.width,h:r.height}})
}))JS";
class PreviewTest : public BApplication {
public:
    PreviewTest(std::string directory,std::string png,std::string probe):BApplication("application/x-vnd.air-Mail-Preview-Test"),fDirectory(std::move(directory)),fPng(std::move(png)),fProbe(std::move(probe)) {}
    int result=1;
    void ReadyToRun() override
    {
        try {
            std::ifstream input(fPng,std::ios::binary); std::string png{std::istreambuf_iterator<char>(input),{}}; check(!png.empty(),"read image fixture");
            auto& d=fSnapshot.draft; d.account=1; d.subject="A message with images 🎉"; d.to="Reader <reader@example.test>"; d.bcc="hidden@example.test";
            fSnapshot.from="Author <author@example.test>"; fSnapshot.key="native-preview-fixture";
            d.attachments={{"Four colours.png","image/png","colours@test",png,true},{"Wide.svg","image/svg+xml","wide@test",
                "<svg xmlns='http://www.w3.org/2000/svg' width='1200' height='60'><rect width='1200' height='60' fill='#2677bc'/><image href='"+fProbe+"/svg' width='1' height='1'/></svg>",true},
                {"notes.txt","text/plain",{},"A regular attachment",false}};
            d.text="Hello café 🎉 — before the image.\n"; d.runs={{0,true,true,true,fProbe+"/link"},{5,false,false,false,{}}};
            auto add=[&](std::size_t index){auto marker=imagePlaceholder(d.attachments[index]); d.images.push_back({d.text.size(),marker.size(),d.attachments[index].contentId}); d.text+=marker;};
            add(0); d.text+="\nAfter the image. A wide image scales to fit:\n"; add(1); d.text+="\nLiteral markup: <script>window.mailInjected=1</script>";
            auto path=WritePreviewTransfer(fSnapshot,BMessenger(this)); struct stat st{};
            check(stat(path.c_str(),&st)==0 && (st.st_mode&0777)==0600,"snapshot is owner-only");
            auto restored=ReadPreviewTransfer(path); check(!std::filesystem::exists(path),"snapshot consumed and removed");
            check(restored.owner==BMessenger(this) && restored.snapshot.draft.attachments[0].data==png,"native transfer preserves owner and binary data");
            bool denied=false; try {ReadPreviewTransfer(fPng);} catch(const std::exception&) {denied=true;} check(denied && std::filesystem::exists(fPng),"arbitrary path rejected without deleting it");
            check(BWebKitInitialize()==B_OK,"initialize native engine"); fContext=std::make_shared<BWebKitContext>(nullptr,true); check(fContext->InitCheck()==B_OK,"private renderer context");
            fPreview=new MessagePreviewWindow(std::move(restored),fContext); fPreview->MoveTo(35,45); fPreview->Show();
            BMessage tick('tick'); fTimer=std::make_unique<BMessageRunner>(BMessenger(this),&tick,150000);
        } catch(const std::exception& e) {Fail(e.what());}
    }
    void MessageReceived(BMessage* message) override
    {
        try {
            if(message->what==B_WEBKIT_JAVASCRIPT_RESULT) {
                fPending=false; const char* value=nullptr;
                if(message->FindString("result",&value)!=B_OK) { message->PrintToStream(); throw std::runtime_error("JavaScript inspection failed"); }
                fDom=Json::parse(value); return;
            }
            if(message->what==kPreviewRefresh || message->what==kPreviewEdit) {check(std::string(message->GetString("preview-key",""))==fSnapshot.key,"preview command identifies its composer"); ++fOwnerCommands; return;}
            if(message->what==kPreviewClosed) { ClosePreview(); return; }
            if(message->what!='tick') {BApplication::MessageReceived(message); return;}
            if(++fTicks>600) throw std::runtime_error("Native preview timed out");
            if(fStep==0) {
                if(fDom.is_null()) { Query(inspect); return; }
                if(!fDom.value("ready",false)) { fDom=nullptr; return; }
                check(fDom["images"][0]["natural"]==480 && fDom["images"][1]["natural"]==1200,"PNG and SVG decoded by WebKit");
                check(fDom["bold"]==true && fDom["italic"]==true && fDom["underline"]==true,"rich formatting present in rendered DOM");
                check(fDom["links"]==0 && fDom["scripts"]==0,"links inert and user markup remains text");
                check(fDom["emoji"]==true && fDom["text"].get<std::string>().find("café 🎉")!=std::string::npos,"bundled emoji font and Unicode loaded");
                auto html=fDom["html"].get<std::string>(); check(html.find("before the image")<html.find("<img") && html.find("<img")<html.find("After the image"),"image rendered between the intended text");
                check(fDom["images"][1]["w"].get<double>()<=fDom["viewport"].get<double>()-30,"wide image constrained to preview width");
                fStep=1; fPaintTicks=0;
            } else if(fStep==1) {
                if(++fPaintTicks<5) return;
                PixelsAndCapture();
                { BAutolock lock(fPreview); fPreview->ResizeTo(610,600); }
                fDom=nullptr; fStep=2; fPaintTicks=0;
            } else if(fStep==2) {
                if(++fPaintTicks<4) return;
                if(fDom.is_null()) {Query(inspect);return;}
                check(fDom["viewport"].get<double>()<650 && fDom["images"][1]["w"].get<double>()<=fDom["viewport"].get<double>()-30,"resizing reflows the large image");
                fDom=nullptr; fStep=3;
                Query("let blocked=false;try{await fetch("+Json(fProbe+"/fetch").dump()+");}catch(e){blocked=true;}return JSON.stringify({blocked,injected:window.mailInjected===1});",true);
            } else if(fStep==3) {
                if(fDom.is_null()) return;
                check(fDom["blocked"]==true && fDom["injected"]==false,"CSP blocks network fetch and escaped body cannot execute");
                fPreview->PostMessage(kPreviewRefresh); fPreview->PostMessage(kPreviewEdit); fStep=4;
            } else if(fStep==4) {
                if(fOwnerCommands!=2) return;
                auto changed=fSnapshot; changed.draft.subject="Updated preview"; changed.draft.text="A refreshed message café 🎉"; changed.draft.images.clear(); changed.draft.attachments.clear(); changed.draft.runs.clear();
                {BAutolock lock(fPreview); fPreview->SetSnapshot({std::move(changed),BMessenger(this)});}
                fDom=nullptr; fStep=5;
            } else if(fStep==5) {
                if(fDom.is_null()) { Query("JSON.stringify({text:document.body.innerText,images:document.images.length})"); return; }
                if(fDom.value("text","")!="A refreshed message café 🎉") {fDom=nullptr;return;}
                check(fDom["images"]==0,"refresh replaces the document and removes old images");
                fPreview->PostMessage(B_QUIT_REQUESTED); fStep=6;
            } else if(fStep==6) {
                if(fPreview) return;
                fContext.reset(); fStep=7;
            } else if(fStep==7) {
                if(BWebKitHasPendingNativeUI() || PreviewHasPendingRenderer()) return;
                fStore=std::make_shared<Store>(fDirectory+"/mail.db"); Account a; a.name="Preview fixture"; a.provider=Provider::Gmail; a.email="author@example.test"; a.id=fStore->saveAccount(a);
                auto draft=fSnapshot.draft; draft.account=a.id; draft.id=fStore->saveDraft(draft); fDraftId=draft.id;
                fComposer=new ComposerWindow(nullptr,fStore,draft); fComposer->MoveTo(20,35); fComposer->Show(); fComposer->PostMessage(kPreviewRefresh); fStep=8;
            } else if(fStep==8) {
                if(!HelperWindow("Preview — "+fSnapshot.draft.subject)) return;
                check(fHelperWindow.IsValid(),"composer launches real companion process"); fFirstWindow=fHelperWindow;
                {BAutolock lock(fComposer); auto* subject=dynamic_cast<BTextControl*>(fComposer->FindView("compose subject")); check(subject,"composer subject field"); subject->SetText("Refreshed through composer");
                    auto* editor=dynamic_cast<DraftEditor*>(fComposer->FindView("compose body")); check(editor,"composer body field"); editor->Select(editor->TextLength(),editor->TextLength()); editor->Insert("\nChanged after preview opened.");}
                check(fHelperWindow.SendMessage(kPreviewRefresh)==B_OK,"request refresh from companion window"); fStep=9;
            } else if(fStep==9) {
                if(!HelperWindow("Preview — Refreshed through composer")) return;
                check(fHelperWindow==fFirstWindow,"refresh reuses the companion window");
                check(fStore->draft(fDraftId).text.find("Changed after preview opened.")!=std::string::npos,"refresh saves latest composer edits");
                check(fHelperWindow.SendMessage(kPreviewEdit)==B_OK,"Edit message returns to composer"); fStep=10;
            } else if(fStep==10) {
                {BAutolock lock(fComposer); if(!fComposer->IsActive()) return; check(true,"Edit message activates originating composer");}
                check(fHelperWindow.SendMessage(B_QUIT_REQUESTED)==B_OK,"close actual preview window"); fStep=11;
            } else if(fStep==11) {
                app_info info; if(be_roster->GetAppInfo(kPreviewSignature,&info)==B_OK) return;
                check(true,"companion drains renderer and exits after last window closes");
                fComposer->PostMessage(kPreviewRefresh); fStep=12;
            } else if(fStep==12) {
                if(!HelperWindow("Preview — Refreshed through composer")) return;
                check(fHelperWindow.SendMessage(B_QUIT_REQUESTED)==B_OK,"close a reopened companion during initial load"); fStep=13;
            } else if(fStep==13) {
                app_info info; if(be_roster->GetAppInfo(kPreviewSignature,&info)==B_OK) return;
                check(true,"reopened companion and its renderer exit");
                if(++fReopens<2) {fComposer->PostMessage(kPreviewRefresh); fStep=12; return;}
                result=0;
                std::printf("Native message preview: %d checks passed (rendering, pixels, reflow, CSP, transfer, refresh, companion and shutdown)\n",checks); std::fflush(stdout);
                fTimer.reset(); PostMessage(B_QUIT_REQUESTED); fStep=14;
            }
        } catch(const std::exception& e) {Fail(e.what());}
    }
    bool QuitRequested() override
    {
        if(fComposer) { if(fComposer->Lock()) fComposer->Quit(); fComposer=nullptr; }
        ClosePreview();
        if(fContext) { fContext.reset(); PostMessage(B_QUIT_REQUESTED); return false; }
        if(BWebKitHasPendingNativeUI() || PreviewHasPendingRenderer()) { fExitReady=false; if(!fQuitTimer) {BMessage quit(B_QUIT_REQUESTED); fQuitTimer=std::make_unique<BMessageRunner>(BMessenger(this),&quit,150000);} return false; }
        fQuitTimer.reset();
        if(!fExitReady) {fExitReady=true; PostMessage(B_QUIT_REQUESTED); return false;}
        return true;
    }
private:
    void Query(const std::string& source,bool async=false) {
        if(fPending) return;
        BAutolock lock(fPreview); auto* web=dynamic_cast<BWebKitView*>(fPreview->FindView("message preview body")); check(web,"preview contains native WebKit view");
        fPending=true; web->EvaluateJavaScript(source.c_str(),BMessenger(this),1,async);
    }
    void ClosePreview() { if(fPreview) {if(fPreview->Lock()) fPreview->Quit(); fPreview=nullptr;} }
    void Fail(const char* why) {std::fprintf(stderr,"FAIL preview step %d: %s\nDOM: %s\n",fStep,why,fDom.dump().c_str()); std::fflush(stderr); fTimer.reset(); BMessenger(kPreviewSignature).SendMessage(B_QUIT_REQUESTED); PostMessage(B_QUIT_REQUESTED);}
    bool HelperWindow(const std::string& title) {
        BMessenger helper(kPreviewSignature); if(!helper.IsValid()) return false;
        BMessage request(B_GET_PROPERTY),reply; request.AddSpecifier("Windows"); if(helper.SendMessage(&request,&reply,1000000,1000000)!=B_OK) return false;
        BMessenger window,extra; if(reply.FindMessenger("result",0,&window)!=B_OK) return false;
        check(reply.FindMessenger("result",1,&extra)!=B_OK,"single preview window for one composer");
        BMessage query(B_GET_PROPERTY),answer; query.AddSpecifier("Title"); if(window.SendMessage(&query,&answer,1000000,1000000)!=B_OK) return false;
        if(title!=answer.GetString("result","")) return false;
        fHelperWindow=window; return true;
    }
    void PixelsAndCapture() {
        BBitmap* raw=nullptr; check(BScreen().GetBitmap(&raw)==B_OK,"capture rendered pixels"); std::unique_ptr<BBitmap> bitmap(raw);
        BPoint origin; {BAutolock lock(fPreview); auto* web=fPreview->FindView("message preview body"); origin=web->ConvertToScreen(BPoint(0,0));}
        auto r=fDom["images"][0];
        auto pixel=[&](double x,double y) {int px=origin.x+r["x"].get<double>()+r["w"].get<double>()*x,py=origin.y+r["y"].get<double>()+r["h"].get<double>()*y;
            check(bitmap->Bounds().Contains(BPoint(px,py)),"image pixel inside screen"); const auto* bits=static_cast<const uint8*>(bitmap->Bits())+py*bitmap->BytesPerRow()+px*4; return rgb_color{bits[2],bits[1],bits[0],255};};
        auto red=pixel(.25,.25),green=pixel(.75,.25),blue=pixel(.25,.75),yellow=pixel(.75,.75);
        check(red.red>200 && red.green<100 && green.green>130 && green.red<80 && blue.blue>170 && blue.red<80 && yellow.red>200 && yellow.green>140,"four image quadrants actually painted on native screen");
        if(const char* path=std::getenv("MAIL_PREVIEW_SCREENSHOT")) {BBitmapStream stream(bitmap.release()); BFile file(path,B_WRITE_ONLY|B_CREATE_FILE|B_ERASE_FILE); check(BTranslatorRoster::Default()->Translate(&stream,nullptr,nullptr,&file,B_PNG_FORMAT)==B_OK,"save preview screenshot");}
    }
    std::string fDirectory,fPng,fProbe; PreviewSnapshot fSnapshot; Json fDom;
    std::shared_ptr<BWebKitContext> fContext; MessagePreviewWindow* fPreview=nullptr; ComposerWindow* fComposer=nullptr;
    std::shared_ptr<Store> fStore; int64_t fDraftId=0;
    std::unique_ptr<BMessageRunner> fTimer,fQuitTimer; BMessenger fHelperWindow,fFirstWindow;
    int fStep=0,fTicks=0,fPaintTicks=0,fOwnerCommands=0,fReopens=0; bool fPending=false,fExitReady=false;
};
}
int main(int argc,char** argv)
{
    if(argc!=3) return 2;
    char directory[]="/tmp/mail-preview-ui-XXXXXX"; if(!mkdtemp(directory)) return 2;
    int result; {PreviewTest app(directory,argv[1],argv[2]); app.Run(); result=app.result;}
    std::filesystem::remove_all(directory); return result;
}
