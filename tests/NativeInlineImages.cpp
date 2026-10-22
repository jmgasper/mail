#include "ComposerWindow.h"
#include "DraftEditor.h"
#include "mail/Mime.h"
#include <Application.h>
#include <Autolock.h>
#include <BitmapStream.h>
#include <Clipboard.h>
#include <DataIO.h>
#include <File.h>
#include <MessageRunner.h>
#include <Screen.h>
#include <TranslatorRoster.h>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <unistd.h>

using namespace mail;
using namespace mail::ui;
namespace {
int checks=0;
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
Draft body(DraftEditor& editor){Draft result;result.account=1;editor.Save(result);validateDraft(result);return result;}
std::string png()
{
    auto* bitmap=new BBitmap(BRect(0,0,479,179),B_RGBA32);check(bitmap->InitCheck()==B_OK,"allocate colored image fixture");
    const uint32 colors[]={0xffff4020,0xff20c050,0xff2070f0,0xffffd020};
    for(int y=0;y<180;++y)for(int x=0;x<480;++x) {
        auto color=x<12 && y<12?0:colors[(y>=90?2:0)+(x>=240?1:0)];std::memcpy(static_cast<char*>(bitmap->Bits())+y*bitmap->BytesPerRow()+x*4,&color,4);
    }
    BBitmapStream stream(bitmap);BMallocIO data;
    check(BTranslatorRoster::Default()->Translate(&stream,nullptr,nullptr,&data,B_PNG_FORMAT)==B_OK,"encode native PNG fixture");
    return {static_cast<const char*>(data.Buffer()),data.BufferLength()};
}
class Test:public BApplication {
public:
    explicit Test(std::string directory):BApplication("application/x-vnd.air-Mail-InlineImages-Test"),fDirectory(std::move(directory)){}
    int result=1;
    void ReadyToRun() override
    {
        try {
            fStore=std::make_shared<Store>(fDirectory+"/mail.db");Account account;account.name="Garden updates";account.email="author@example.test";account.provider=Provider::Gmail;account.id=fStore->saveAccount(account);
            Draft draft;draft.account=account.id;draft.subject="Images in the editable message";draft.to="recipient@example.test";
            draft.text="Before the garden.\n\nAfter the garden.\n";draft.runs={{0,true,false,false,{}},{19,false,false,false,{}}};
            fFile={"garden [café] 🎉.png","image/png","garden-inline@test",png(),true};
            fOwner=new BWindow(BRect(15,25,500,300),"Inline image fixture",B_TITLED_WINDOW,0);fOwner->Show();
            fComposer=new ComposerWindow(fOwner,fStore,draft);fComposer->Show();
            BMessage tick('tick');fTimer=std::make_unique<BMessageRunner>(BMessenger(this),&tick,150000);
        }catch(const std::exception& e){Fail(e.what());}
    }
    void MessageReceived(BMessage* message) override
    {
        if(fFailed)return;
        if(message->what!='tick'){BApplication::MessageReceived(message);return;}
        try {
            if(++fTicks>160)throw std::runtime_error("Inline image native check timed out");
            BAutolock lock(fComposer);auto* editor=dynamic_cast<DraftEditor*>(fComposer->FindView("compose body"));if(!editor)throw std::runtime_error("Native composer editor is missing");
            if(fStep==0) {
                editor->Select(19,19);editor->AddAttachments({fFile},19);fExpected=body(*editor);fImage=fExpected.images.at(0);fOriginalHtml=draftHtml(fExpected);++fStep;return;
            }
            auto frame=editor->InlineImageFrame(fImage.offset);
            if(fStep==1) {
                if(!frame.IsValid())return;
                check(frame.Width()>470 && frame.Height()>175,"decoded image reserves its actual native text-layout dimensions");
                check(editor->PointAt(fImage.offset+fImage.length+1).y>frame.bottom,"following paragraph lies below the image");
                check(draftHtml(body(*editor))==fOriginalHtml,"background decoding preserves body, styles and MIME");
                ++fStep;return;
            }
            if(fStep==2) {
                if(!Pixels(*editor,frame,true))return;
                editor->MouseDown(BPoint((frame.left+frame.right)/2,(frame.top+frame.bottom)/2));int32 begin,end;editor->GetSelection(&begin,&end);
                check(begin==static_cast<int32>(fImage.offset) && end==static_cast<int32>(fImage.offset+fImage.length),"click selects the whole rendered image");
                editor->Copy(be_clipboard);check(be_clipboard->Lock(),"lock image clipboard");
                const void* bytes=nullptr;ssize_t size=0;auto* clip=be_clipboard->Data();bool named=clip->FindData("text/plain",B_MIME_TYPE,&bytes,&size)==B_OK && std::string(static_cast<const char*>(bytes),size)==imagePlaceholder(fFile);
                bool portable=clip->FindData("text/html",B_MIME_TYPE,&bytes,&size)==B_OK && std::string(static_cast<const char*>(bytes),size).find("data:image/png;base64,")!=std::string::npos;
                bool native=clip->FindData("application/x-vnd.Be-text_run_array",B_MIME_TYPE,&bytes,&size)==B_OK;
                if(native){const auto* runs=static_cast<const text_run_array*>(bytes);for(int i=0;i<runs->count;++i){font_family family;runs->runs[i].font.GetFamilyAndStyle(&family,nullptr);native=native && std::string(family).find("Mail Inline")==std::string::npos && runs->runs[i].font.Size()==be_plain_font->Size();}}
                be_clipboard->Unlock();check(named && portable && native,"Copy retains image bytes and named text without private layout fonts");
                editor->Select(end,end);char left=B_LEFT_ARROW;editor->KeyDown(&left,1);editor->GetSelection(&begin,&end);
                check(begin==static_cast<int32>(fImage.offset) && begin==end,"left arrow crosses image atomically");
                char right=B_RIGHT_ARROW;editor->KeyDown(&right,1);editor->GetSelection(&begin,&end);
                check(begin==static_cast<int32>(fImage.offset+fImage.length) && begin==end,"right arrow crosses image atomically");
                editor->KeyDown("X",1);auto typed=body(*editor);check(typed.text.substr(fImage.offset+fImage.length,1)=="X","typing beside the image remains ordinary editable text");
                BFont font;editor->GetFontAndColor(fImage.offset+fImage.length,&font);check(font.Size()==be_plain_font->Size(),"image metrics do not leak into newly typed text");
                editor->Undo(be_clipboard);check(draftHtml(body(*editor))==fOriginalHtml,"Undo restores text and image after typing");
                editor->Select(fImage.offset+fImage.length,fImage.offset+fImage.length);char backspace=B_BACKSPACE;editor->KeyDown(&backspace,1);
                check(body(*editor).images.empty() && body(*editor).attachments.size()==1 && body(*editor).text.find("[Image:")==std::string::npos,"Backspace removes the image as one object and retains the attachment");
                editor->Undo(be_clipboard);check(draftHtml(body(*editor))==fOriginalHtml,"Undo restores the deleted image and exact MIME");
                editor->SetInsets(12,12,editor->Bounds().Width()-272,12);BMessage ready(kInlineImagesReady);editor->MessageReceived(&ready);
                ++fStep;return;
            }
            if(fStep==3) {
                if(!frame.IsValid())return;
                if(!Pixels(*editor,frame,false))return;
                check(frame.IsValid() && frame.Width()<260 && frame.Height()<110,"narrow text column scales the image proportionally");
                check(editor->LineAt(fImage.offset)==editor->LineAt(fImage.offset+fImage.length-1),"Unicode and repeated brackets in the marker do not split the image across lines");
                check(draftHtml(body(*editor))==fOriginalHtml,"reflow does not change persisted body or styles");
                editor->SetInsets(12,12,12,12);BMessage ready(kInlineImagesReady);editor->MessageReceived(&ready);
                editor->Select(fImage.offset,fImage.offset+fImage.length);editor->Toggle(B_BOLD_FACE);auto styled=body(*editor);bool bold=false;
                for(const auto& run:styled.runs)if(run.offset<=fImage.offset)bold=run.bold;
                check(bold && styled.text==fExpected.text && styled.images.size()==1,"formatting selection preserves semantic image styling");
                editor->Undo(be_clipboard);check(draftHtml(body(*editor))==fOriginalHtml,"format Undo retains rendered image semantics");
                editor->Select(editor->TextLength(),editor->TextLength());editor->Paste(be_clipboard);
                auto copied=body(*editor);check(copied.images.size()==2 && copied.attachments.size()==1 && copied.attachments[0].data==fFile.data,"native paste repeats the bitmap with one original MIME part");
                editor->Undo(be_clipboard);editor->Redo();check(body(*editor).images.size()==2,"image paste Undo/Redo restores both placements");
                editor->Undo(be_clipboard);auto saved=body(*editor);saved.account=fExpected.account;saved.subject="Reloaded inline image";saved.id=fStore->saveDraft(saved);auto loaded=fStore->draft(saved.id);check(loaded.id==saved.id,"persist image draft");editor->Load(loaded);
                ++fStep;return;
            }
            if(fStep==4) {
                if(!frame.IsValid())return;
                check(draftHtml(body(*editor))==fOriginalHtml && body(*editor).attachments[0].data==fFile.data,"reopened draft restores pixels without changing binary contents");
                check(!editor->CanUndo() && !editor->CanRedo(),"asynchronous image layout adds no Undo history");
                editor->Select(fImage.offset,fImage.offset);char forward=B_DELETE;editor->KeyDown(&forward,1);check(body(*editor).images.empty(),"forward Delete removes the entire image");editor->Undo(be_clipboard);
                Attachment broken{"broken.png","image/png","broken@test","not a bitmap",true};editor->AddAttachments({broken},editor->TextLength());fBrokenOffset=body(*editor).images.back().offset;
                Attachment vector{"masked.png","image/png","vector@test","<svg xmlns='http://www.w3.org/2000/svg' width='240' height='120'><rect width='240' height='120' fill='red'/></svg>",true};editor->AddAttachments({vector},editor->TextLength());fVectorOffset=body(*editor).images.back().offset;
                ++fStep;return;
            }
            if(fStep==5) {
                if(++fWait<5)return;
                check(!editor->InlineImageFrame(fBrokenOffset).IsValid() && body(*editor).text.find("[Image: broken.png]")!=std::string::npos,"undecodable image retains an editable named marker and original bytes");
                check(editor->InlineImageFrame(fImage.offset).IsValid(),"one failed image does not remove the working image");
                check(!editor->InlineImageFrame(fVectorOffset).IsValid(),"SVG disguised as PNG is not passed to an automatic document decoder");
                editor->Undo(be_clipboard);editor->Undo(be_clipboard);check(draftHtml(body(*editor))==fOriginalHtml,"failed preview does not add history or block Undo");
                Draft longDraft;longDraft.account=fExpected.account;longDraft.attachments={fFile};longDraft.runs={{0,false,false,false,{}}};
                auto marker=imagePlaceholder(fFile);
                for(int i=0;i<10;++i){longDraft.images.push_back({longDraft.text.size(),marker.size(),fFile.contentId});longDraft.text+=marker+"\n";}
                longDraft.text+="Final paragraph.";fLastImage=longDraft.images.back().offset;editor->Load(longDraft);++fStep;return;
            }
            if(fStep==6) {
                if(!editor->InlineImageFrame(fLastImage).IsValid())return;
                editor->MakeFocus();editor->Select(editor->TextLength(),editor->TextLength());editor->ScrollToSelection();
                check(editor->Bounds().top>500,"long image draft scrolls to its final paragraph");
                editor->KeyDown("Z",1);check(editor->Bounds().Contains(editor->PointAt(editor->TextLength())),"typing below multiple images keeps the caret visible");
                editor->Undo(be_clipboard);check(editor->Bounds().Contains(editor->PointAt(editor->TextLength())),"Undo below images keeps the caret visible");
                auto scroll=editor->Bounds().top;editor->MakeFocus(false);BMessage ready(kInlineImagesReady);editor->MessageReceived(&ready);
                check(std::abs(editor->Bounds().top-scroll)<2,"background image layout preserves the scrolled reading position");
                // Closing with newly queued image work must cancel and join it.
                auto closing=body(*editor);closing.attachments[0].contentId="closing@test";for(auto& image:closing.images)image.contentId="closing@test";editor->Load(closing);
                fTimer.reset();result=0;std::printf("Native inline images: %d checks passed (painted pixels, reflow, selection, keyboard, portable clipboard, history, persistence and decoder fallback)\n",checks);PostMessage(B_QUIT_REQUESTED);
            }
        }catch(const std::exception& e){Fail(e.what());}
    }
private:
    bool Pixels(DraftEditor& editor,BRect frame,bool save)
    {
        fComposer->UpdateIfNeeded();editor.Sync();
        editor.ConvertToScreen(&frame);BBitmap* raw=nullptr;if(BScreen().GetBitmap(&raw)!=B_OK)throw std::runtime_error("Cannot capture native image pixels");std::unique_ptr<BBitmap> bitmap(raw);
        if(bitmap->ColorSpace()!=B_RGB32 && bitmap->ColorSpace()!=B_RGBA32)throw std::runtime_error("Unreadable native pixel format");
        int counts[4]={};
        for(int y=std::max(0,int(frame.top));y<=std::min(int(bitmap->Bounds().bottom),int(frame.bottom));++y)for(int x=std::max(0,int(frame.left));x<=std::min(int(bitmap->Bounds().right),int(frame.right));++x) {
            const auto* p=static_cast<const uint8*>(bitmap->Bits())+y*bitmap->BytesPerRow()+x*4;int b=p[0],g=p[1],r=p[2];
            if(r>210 && g<110 && b<90)++counts[0];
            if(g>150 && r<80 && b<120)++counts[1];
            if(b>190 && r<80 && g<150)++counts[2];
            if(r>210 && g>170 && b<90)++counts[3];
        }
        if(counts[0]<=500 || counts[1]<=500 || counts[2]<=500 || counts[3]<=500)return false;
        auto* alpha=static_cast<const uint8*>(bitmap->Bits())+(int(frame.top)+5)*bitmap->BytesPerRow()+(int(frame.left)+5)*4;auto background=editor.ViewColor();
        check(std::abs(int(alpha[0])-background.blue)<3 && std::abs(int(alpha[1])-background.green)<3 && std::abs(int(alpha[2])-background.red)<3,"transparent image pixels preserve the editor background");
        if(const char* path=std::getenv("MAIL_INLINE_SCREENSHOT")){auto output=std::string(path)+(save?"":".reflow.png");BBitmapStream stream(bitmap.release());BFile file(output.c_str(),B_WRITE_ONLY|B_CREATE_FILE|B_ERASE_FILE);check(BTranslatorRoster::Default()->Translate(&stream,nullptr,nullptr,&file,B_PNG_FORMAT)==B_OK,"write native inline-image capture");}
        check(counts[0]>500 && counts[1]>500 && counts[2]>500 && counts[3]>500,"all four image quadrants paint within native editor geometry");
        return true;
    }
    void Fail(const char* error){if(fFailed)return;fFailed=true;fTimer.reset();std::fprintf(stderr,"FAIL inline images step %d: %s\n",fStep,error);PostMessage(B_QUIT_REQUESTED);}
    std::string fDirectory,fOriginalHtml;std::shared_ptr<Store> fStore;BWindow* fOwner=nullptr;ComposerWindow* fComposer=nullptr;
    std::unique_ptr<BMessageRunner> fTimer;Attachment fFile;Draft fExpected;InlineImage fImage;std::size_t fBrokenOffset=0,fVectorOffset=0,fLastImage=0;
    int fStep=0,fTicks=0,fWait=0;bool fFailed=false;
};
}
int main()
{
    char directory[]="/tmp/mail-inline-ui-XXXXXX";if(!mkdtemp(directory))return 2;
    int result;{Test app(directory);app.Run();result=app.result;}std::filesystem::remove_all(directory);return result;
}
