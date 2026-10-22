#include "MessagePreview.h"
#include "Emoji.h"
#include <WebKit/WebKitView.h>
#include <Application.h>
#include <Button.h>
#include <LayoutBuilder.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <Resources.h>
#include <StringView.h>
#include <algorithm>
#include <stdexcept>

namespace mail::ui {
namespace {
constexpr uint32 kZoomIn='pvzi',kZoomOut='pvzo',kZoomReset='pvzr',kPlainText='pvpt';
class PreviewWebView : public BWebKitView {
public:
    PreviewWebView(BMessenger target,std::shared_ptr<BWebKitContext> context)
        :BWebKitView(BRect(0,0,650,400),"message preview body",target,B_FOLLOW_ALL,std::move(context)) {}
    void MessageReceived(BMessage* message) override {
        // This is a document preview, not a navigation target for file/URL drops.
        if(message->what==B_SIMPLE_DATA || message->what==B_REFS_RECEIVED || message->what==B_PASTE) return;
        BWebKitView::MessageReceived(message);
    }
};
}
MessagePreviewWindow::MessagePreviewWindow(TransferredPreview snapshot,std::shared_ptr<BWebKitContext> context)
    :BWindow(BRect(180,70,1020,740),"Message preview",B_TITLED_WINDOW,B_AUTO_UPDATE_SIZE_LIMITS|B_ASYNCHRONOUS_CONTROLS|B_CLOSE_ON_ESCAPE)
{
    auto label=[](const char* name) { auto* view=new EmojiStringView(name,""); view->SetExplicitMinSize(BSize(80,B_SIZE_UNSET)); view->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED,B_SIZE_UNSET)); return view; };
    fSubject=label("preview subject"); fSubject->SetFont(be_bold_font); fFrom=label("preview from"); fTo=label("preview to");
    fCc=label("preview cc"); fBcc=label("preview bcc"); fFiles=label("preview files"); fStatus=label("message preview status");
    fDate=label("preview date");
    fThread=new BMenuField("preview conversation","Message:",new BPopUpMenu("Conversation")); fThread->Menu()->SetMaxContentWidth(600);
    fPlain=new BButton("preview plain text","Show plain text",new BMessage(kPlainText));
    fEdit=new BButton("preview edit","Edit message",new BMessage(kPreviewEdit));
    fWeb=new PreviewWebView(BMessenger(this),std::move(context));
    if(fWeb->InitCheck()!=B_OK) { delete fWeb; throw std::runtime_error("Could not create the message renderer."); }
    fWeb->SetExplicitMinSize(BSize(420,220)); fWeb->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED,B_SIZE_UNLIMITED));
    BLayoutBuilder::Group<>(this,B_VERTICAL).SetInsets(12).Add(fThread).Add(fSubject).Add(fFrom).Add(fTo).Add(fCc).Add(fBcc).Add(fDate).Add(fFiles)
        .Add(fWeb,1).Add(fStatus)
        .AddGroup(B_HORIZONTAL).Add(new BButton("Smaller",new BMessage(kZoomOut))).Add(new BButton("100%",new BMessage(kZoomReset)))
        .Add(new BButton("Larger",new BMessage(kZoomIn))).AddGlue().Add(new BButton("Refresh",new BMessage(kPreviewRefresh)))
        .Add(fPlain).Add(fEdit).Add(new BButton("Close",new BMessage(B_QUIT_REQUESTED))).End();
    fWeb->SetURLSchemeHandler("mail-preview",BMessenger(this));
    fWeb->SetBeforeUnloadBehavior(B_WEBKIT_BEFORE_UNLOAD_PROCEED);
    SetSnapshot(std::move(snapshot));
}
bool MessagePreviewWindow::QuitRequested()
{
    BMessage close(kPreviewClosed); close.AddMessenger("window",BMessenger(this)); be_app->PostMessage(&close); return false;
}
void MessagePreviewWindow::SetSnapshot(TransferredPreview snapshot)
{
    fSnapshot=std::move(snapshot); const auto& s=fSnapshot.snapshot; const auto& d=s.draft;
    auto label=[](BStringView* view,const std::string& value) { view->SetText(value.c_str()); view->SetToolTip(value.c_str()); };
    auto* received=s.received?&*s.received:nullptr;
    const auto& subject=received?received->subject:d.subject; const auto& cc=received?received->cc:d.cc;
    label(fSubject,subject.empty()?"(No subject)":subject); label(fFrom,"From: "+(received?received->from:s.from)); label(fTo,"To: "+(received?received->to:d.to));
    label(fCc,cc.empty()?"":"Cc: "+cc); label(fBcc,!received && !d.bcc.empty()?"Bcc (hidden recipients): "+d.bcc:"");
    label(fDate,received?received->date:"");
    while(auto* item=fThread->Menu()->RemoveItem(int32(0))) delete item;
    if(received) for(const auto& item:received->thread) {
        auto* command=new BMessage(kPreviewSelect); command->AddInt64("message",item.id);
        auto* entry=new BMenuItem((item.date+" — "+item.sender).c_str(),command); entry->SetMarked(item.id==received->id); fThread->Menu()->AddItem(entry);
    }
    fThread->Menu()->SetTargetForItems(this);
    auto visible=[](BView* view,bool show) {if(show && view->IsHidden(view)) view->Show(); if(!show && !view->IsHidden(view)) view->Hide();};
    visible(fThread,received && received->thread.size()>1); visible(fPlain,received); visible(fEdit,!received);
    fPlain->SetEnabled(received && !received->html.empty());
    std::string files;
    for(const auto& file:received?received->files:d.attachments) if(!file.inlineImage) { if(!files.empty()) files+=", "; files+=file.name; }
    label(fFiles,files.empty()?"":"Attachments: "+files);
    for(auto* optional:{fCc,fBcc,fDate,fFiles}) {
        bool empty=!*optional->Text();
        if(empty && !optional->IsHidden(optional)) optional->Hide();
        if(!empty && optional->IsHidden(optional)) optional->Show();
    }
    Render(); SetTitle(((received?"Message — ":"Preview — ")+(subject.empty()?"(No subject)":subject)).c_str());
}
void MessagePreviewWindow::Render()
{
    auto id=newMessageId(); id.erase(std::remove_if(id.begin(),id.end(),[](unsigned char c){return !((c>='a'&&c<='z')||(c>='0'&&c<='9'));}),id.end());
    fRoot="mail-preview://"+id+"/"; const auto& s=fSnapshot.snapshot;
    fRenderNotice.clear();
    try { fHtml=s.received?receivedHtml(*s.received,fRoot,fPlainText):previewHtml(s.draft,fRoot); }
    catch(const std::exception& error) {
        if(!s.received) throw;
        fPlainText=true; fHtml=receivedHtml(*s.received,fRoot,true); fRenderNotice=std::string(error.what())+" Plain text is shown.";
    }
    fPlain->SetLabel(fPlainText?"Show formatted message":"Show plain text"); fStatus->SetText("Loading message…");
    fWeb->LoadURL((fRoot+"message").c_str());
}
void MessagePreviewWindow::Serve(BMessage& message)
{
    uint64 identifier=0; message.FindUInt64("identifier",&identifier); auto url=std::string(message.GetString("url",""));
    auto fail=[&]{BWebKitView::FailURLSchemeRequest(identifier,"This resource is not part of the message preview.");};
    if(std::string(message.GetString("method",""))!="GET") { fail(); return; }
    auto policy=std::string(kPreviewPolicy);
    if(fSnapshot.snapshot.received) policy+="; sandbox allow-same-origin";
    BMessage headers; headers.AddString("name","Content-Security-Policy"); headers.AddString("value",policy.c_str());
    headers.AddString("name","Cache-Control"); headers.AddString("value","no-store");
    headers.AddString("name","X-Content-Type-Options"); headers.AddString("value","nosniff");
    if(url==fRoot+"message") { BWebKitView::RespondToURLSchemeRequest(identifier,200,"text/html; charset=utf-8",&headers,fHtml.data(),fHtml.size()); return; }
    // main_frame identifies the requesting frame, including its images/fonts;
    // it does not mean this is a top-level navigation. Exact URLs below bound
    // the resources to this snapshot.
    if(url==fRoot+"emoji.ttf") {
        std::size_t size=0; auto* resources=BApplication::AppResources(); const void* font=resources?resources->LoadResource('RAWT',"Mail:NotoEmoji",&size):nullptr;
        if(font && size<=8*1024*1024) BWebKitView::RespondToURLSchemeRequest(identifier,200,"font/ttf",&headers,font,size); else fail();
        return;
    }
    const auto& s=fSnapshot.snapshot; const auto& files=s.received?s.received->files:s.draft.attachments;
    for(std::size_t i=0;i<files.size();++i) if((s.received?previewImageType(files[i].mediaType):files[i].inlineImage) && url==fRoot+"image/"+std::to_string(i)) {
        const auto& file=files[i]; BWebKitView::RespondToURLSchemeRequest(identifier,200,file.mediaType.c_str(),&headers,file.data.data(),file.data.size()); return;
    }
    fail();
}
void MessagePreviewWindow::MessageReceived(BMessage* message)
{
    switch(message->what) {
        case B_WEBKIT_URL_SCHEME_REQUEST: Serve(*message); break;
        case B_WEBKIT_STATE_CHANGED:
            if(std::string(message->GetString("url",""))==fRoot+"message" && !message->GetBool("loading",true)) {
                auto error=message->GetString("loadError",""); fStatus->SetText(*error?error:(!fRenderNotice.empty()?fRenderNotice.c_str():(fSnapshot.snapshot.received?"Remote content is blocked. Links are shown as text.":"Preview of the saved draft. Refresh after editing.")));
            } break;
        case B_WEBKIT_PROCESS_EXITED: fStatus->SetText("The renderer stopped. Refresh to reload this preview."); break;
        case B_WEBKIT_NEW_PAGE_REQUESTED: BWebKitView::DeclineNewPage(message->GetUInt64("identifier",0)); break;
        case kPlainText:
            if(fSnapshot.snapshot.received) {fPlainText=!fPlainText; Render();} break;
        case kPreviewRefresh: case kPreviewEdit: case kPreviewSelect: {
            BMessage request(message->what); request.AddString("preview-key",Key().c_str());
            if(fSnapshot.snapshot.received) request.AddInt64("message",message->GetInt64("message",fSnapshot.snapshot.received->id));
            if(fSnapshot.owner.SendMessage(&request)!=B_OK) fStatus->SetText("The source window is closed. This view keeps its saved snapshot.");
            break;
        }
        case kZoomIn: fWeb->IncreaseZoomFactor(); break;
        case kZoomOut: fWeb->DecreaseZoomFactor(); break;
        case kZoomReset: fWeb->ResetZoomFactor(); break;
        default: BWindow::MessageReceived(message);
    }
}
}
