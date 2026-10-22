#include "ReaderView.h"
#include "Emoji.h"
#include "PreviewLifecycle.h"
#include <WebKit/WebKitView.h>
#include <Application.h>
#include <CardLayout.h>
#include <Clipboard.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <Resources.h>
#include <ScrollView.h>
#include <Window.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace mail::ui {
namespace {
constexpr uint32 kCopyText='rdct',kCopyLink='rdcl',kOpenLink='rdol';
std::shared_ptr<BWebKitContext> gContext;
// MAIL_READER_TRACE=1 logs the reader's requests and page loads to stderr.
const bool gTrace=std::getenv("MAIL_READER_TRACE")!=nullptr;
bool gInitialized=false,gAvailable=false;
class ReaderWebView : public BWebKitView {
public:
    ReaderWebView(BMessenger target)
        :BWebKitView(BRect(0,0,400,300),"message web reader",target,B_FOLLOW_ALL,gContext) {}
    void MessageReceived(BMessage* message) override {
        // A message is a document, not a place to drop files or paste into.
        if(message->what==B_SIMPLE_DATA || message->what==B_REFS_RECEIVED || message->what==B_PASTE) return;
        BWebKitView::MessageReceived(message);
    }
};
std::string newRoot()
{
    auto id=newMessageId(); id.erase(std::remove_if(id.begin(),id.end(),[](unsigned char c){return !((c>='a'&&c<='z')||(c>='0'&&c<='9'));}),id.end());
    return "mail-preview://"+id+"/";
}
void copyText(const char* text)
{
    if(!be_clipboard->Lock()) return;
    be_clipboard->Clear();
    if(auto* data=be_clipboard->Data()) { data->AddData("text/plain",B_MIME_TYPE,text,strlen(text)); be_clipboard->Commit(); }
    be_clipboard->Unlock();
}
}
bool InitializeReaderEngine()
{
    if(gInitialized) return gAvailable;
    gInitialized=true;
    if(BWebKitInitialize()!=B_OK) return false;
    // A private context: messages never share cookies or storage with the web.
    gContext=std::make_shared<BWebKitContext>(nullptr,true);
    if(gContext->InitCheck()!=B_OK) { gContext.reset(); return false; }
    return gAvailable=true;
}
void ShutdownReaderEngine() { gContext.reset(); gAvailable=false; }
bool ReaderEngineBusy() { return gInitialized && (BWebKitHasPendingNativeUI() || PreviewHasPendingRenderer()); }

ReaderView::ReaderView()
    :BView("reader view",0)
{
    fCards=new BCardLayout(); SetLayout(fCards);
    fText=new EmojiTextView("message reader"); fText->MakeEditable(false);
    fText->SetWordWrap(true); fText->SetInsets(16,12,16,12);
    fTextScroll=new BScrollView("reader scroll",fText,0,false,true,B_NO_BORDER);
    fTextScroll->SetExplicitMinSize(BSize(300,200));
    fCards->AddView(fTextScroll);
}
void ReaderView::AttachedToWindow()
{
    BView::AttachedToWindow();
    if(fWeb || !gAvailable) return;
    // The engine reports to a messenger, which needs this view in a looper.
    auto* web=new ReaderWebView(BMessenger(this));
    if(web->InitCheck()!=B_OK) { delete web; return; }
    fWeb=web;
    fWeb->SetExplicitMinSize(BSize(300,200));
    fWeb->SetURLSchemeHandler("mail-preview",BMessenger(this));
    fWeb->SetBeforeUnloadBehavior(B_WEBKIT_BEFORE_UNLOAD_PROCEED);
    fCards->AddView(fWeb);
    Render();
}
void ReaderView::SetEmpty(const char* text)
{
    fMode=Mode::Empty; fMessages.clear(); fExpanded.clear(); fDraft={};
    fText->SetText(text); fText->ScrollTo(0,0); Render();
}
void ReaderView::SetConversation(std::vector<ReceivedPreview> messages,const std::string& plainText,std::set<Id> expanded)
{
    fMode=Mode::Conversation; fMessages=std::move(messages); fExpanded=std::move(expanded);
    fText->SetText(plainText.c_str()); ApplyEmojiFonts(*fText); fText->ScrollTo(0,0); Render();
}
void ReaderView::SetDraft(const Draft& draft,const std::string& plainText)
{
    fMode=Mode::Draft; fMessages.clear(); fDraft=draft;
    fText->SetText(plainText.c_str()); ApplyEmojiFonts(*fText); fText->ScrollTo(0,0); Render();
}
void ReaderView::SetPlain(bool plain) { if(fPlain==plain) return; fPlain=plain; Render(); }
void ReaderView::ShowCard() { fCards->SetVisibleItem(CanFormat() && !fPlain && fMode!=Mode::Empty?1:int32(0)); }
void ReaderView::Render()
{
    ShowCard();
    if(!CanFormat() || fPlain || fMode==Mode::Empty) return;
    fRoot=newRoot();
    try { fHtml=fMode==Mode::Draft?previewHtml(fDraft,fRoot):conversationHtml(fMessages,fRoot,false,fExpanded); }
    catch(const std::exception&) {
        try { fHtml=fMode==Mode::Draft?std::string():conversationHtml(fMessages,fRoot,true,fExpanded); } catch(const std::exception&) { fHtml.clear(); }
        if(fHtml.empty()) { fCards->SetVisibleItem(int32(0)); return; }
    }
    fWeb->LoadURL((fRoot+"message").c_str());
}
void ReaderView::Serve(BMessage& message)
{
    uint64 identifier=0; message.FindUInt64("identifier",&identifier); auto url=std::string(message.GetString("url",""));
    auto fail=[&]{BWebKitView::FailURLSchemeRequest(identifier,"This resource is not part of the message.");};
    if(std::string(message.GetString("method",""))!="GET" || fRoot.empty() || url.rfind(fRoot,0)!=0) { fail(); return; }
    // Received mail may not run scripts or submit forms. Links open new pages,
    // which this view declines and hands to the system browser.
    auto policy=std::string(kPreviewPolicy)+(fMode==Mode::Conversation?"; sandbox allow-same-origin allow-popups":"");
    BMessage headers; headers.AddString("name","Content-Security-Policy"); headers.AddString("value",policy.c_str());
    headers.AddString("name","Cache-Control"); headers.AddString("value","no-store");
    headers.AddString("name","X-Content-Type-Options"); headers.AddString("value","nosniff");
    auto path=url.substr(fRoot.size());
    if(gTrace) std::fprintf(stderr,"reader: request %s\n",path.c_str());
    if(path=="message") { BWebKitView::RespondToURLSchemeRequest(identifier,200,"text/html; charset=utf-8",&headers,fHtml.data(),fHtml.size()); return; }
    if(path=="emoji.ttf") {
        std::size_t size=0; auto* resources=BApplication::AppResources(); const void* font=resources?resources->LoadResource('RAWT',"Mail:NotoEmoji",&size):nullptr;
        if(font && size<=8*1024*1024) BWebKitView::RespondToURLSchemeRequest(identifier,200,"font/ttf",&headers,font,size); else fail();
        return;
    }
    const std::vector<Attachment>* files=nullptr; std::string rest;
    if(fMode==Mode::Draft) { files=&fDraft.attachments; rest=path; }
    else if(path.size()>1 && path[0]=='m') {
        auto slash=path.find('/'); auto index=path.substr(1,slash==std::string::npos?0:slash-1);
        if(!index.empty() && index.size()<6 && index.find_first_not_of("0123456789")==std::string::npos && std::stoul(index)<fMessages.size()) {
            files=&fMessages[std::stoul(index)].files; rest=path.substr(slash+1);
        }
    }
    if(files) for(std::size_t i=0;i<files->size();++i) {
        const auto& file=(*files)[i];
        if(rest!="image/"+std::to_string(i) || !(fMode==Mode::Draft?file.inlineImage:previewImageType(file.mediaType))) continue;
        BWebKitView::RespondToURLSchemeRequest(identifier,200,file.mediaType.c_str(),&headers,file.data.data(),file.data.size()); return;
    }
    fail();
}
void ReaderView::MessageReceived(BMessage* message)
{
    auto openLink=[&](const char* url) {
        if(!url || !openableLink(url) || !Window()) return;
        BMessage link(kReaderLink); link.AddString("url",url); Window()->PostMessage(&link);
    };
    switch(message->what) {
        case B_WEBKIT_URL_SCHEME_REQUEST: Serve(*message); break;
        case B_WEBKIT_NEW_PAGE_REQUESTED:
            BWebKitView::DeclineNewPage(message->GetUInt64("identifier",0));
            if(message->GetBool("user_gesture",false)) openLink(message->GetString("url",nullptr));
            break;
        case B_WEBKIT_LINK_OPEN_REQUESTED: openLink(message->GetString("url",nullptr)); break;
        case B_WEBKIT_PROCESS_EXITED:
            // Reload once; a renderer that keeps failing leaves plain text.
            if(!fWeb || fWebFailed) break;
            if(++fRendererExits>2) { fWebFailed=true; ShowCard(); }
            else Render();
            if(Window()) Window()->PostMessage(kReaderChanged);
            break;
        case B_WEBKIT_CONTEXT_MENU: {
            if(!CanFormat()) break;
            BPopUpMenu menu("reader menu",false,false);
            std::string selected=message->GetString("selected_text",""),link=message->GetString("link_url","");
            if(!link.empty() && openableLink(link)) {
                auto* open=new BMessage(kOpenLink); open->AddString("url",link.c_str()); menu.AddItem(new BMenuItem("Open link",open));
                auto* copy=new BMessage(kCopyLink); copy->AddString("url",link.c_str()); menu.AddItem(new BMenuItem("Copy link address",copy));
            }
            if(!selected.empty()) menu.AddItem(new BMenuItem("Copy",new BMessage(kCopyText)));
            menu.AddItem(new BMenuItem("Select all",new BMessage(B_SELECT_ALL)));
            BPoint where; message->FindPoint("where",&where);
            if(auto* item=menu.Go(fWeb->ConvertToScreen(where),false,true)) {
                auto* chosen=item->Message();
                if(chosen->what==kOpenLink) openLink(chosen->GetString("url",nullptr));
                else if(chosen->what==kCopyLink) copyText(chosen->GetString("url",""));
                else if(chosen->what==kCopyText) fWeb->ExecuteEditCommand(B_COPY);
                else fWeb->ExecuteEditCommand(B_SELECT_ALL);
            }
            break;
        }
        case B_WEBKIT_STATE_CHANGED:
            if(gTrace) std::fprintf(stderr,"reader: %s loading=%d error=%s\n",message->GetString("url",""),message->GetBool("loading",false),message->GetString("loadError",""));
            break;
        case B_WEBKIT_URL_SCHEME_REQUEST_STOPPED: case B_WEBKIT_LINK_HOVERED: case B_WEBKIT_ICON_LOADED: break;
        default: BView::MessageReceived(message);
    }
}
}
