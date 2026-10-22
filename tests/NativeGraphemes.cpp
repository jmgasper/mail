#include "ComposerWindow.h"
#include "DraftEditor.h"
#include "Emoji.h"
#include "mail/Mime.h"
#include <Application.h>
#include <Autolock.h>
#include <Clipboard.h>
#include <MessageRunner.h>
#include <TextControl.h>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <unistd.h>

using namespace mail;
using namespace mail::ui;
namespace {
int checks=0;
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
Draft body(DraftEditor& editor){Draft d;d.account=1;editor.Save(d);validateDraft(d);return d;}
void plainClipboard(const char* text)
{
    check(be_clipboard->Lock(),"lock disposable clipboard");be_clipboard->Clear();be_clipboard->Data()->AddData("text/plain",B_MIME_TYPE,text,std::strlen(text));be_clipboard->Commit();be_clipboard->Unlock();
}
bool clipboardIs(const std::string& expected,bool portable=false)
{
    if(!be_clipboard->Lock())return false;
    const void* bytes=nullptr;ssize_t length=0;auto* data=be_clipboard->Data();
    bool okay=data->FindData("text/plain",B_MIME_TYPE,&bytes,&length)==B_OK && std::string(static_cast<const char*>(bytes),length)==expected;
    if(portable){okay=okay && data->FindData("application/x-vnd.Be-text_run_array",B_MIME_TYPE,&bytes,&length)==B_OK;if(okay){const auto* runs=static_cast<const text_run_array*>(bytes);for(int i=0;i<runs->count;++i)okay=okay && !IsEmojiFont(runs->runs[i].font);}}
    be_clipboard->Unlock();return okay;
}
void exercise(DraftEditor& editor)
{
    for(const std::string cluster:{"👩🏽‍💻","🇦🇺","👨‍👩‍👧‍👦","👩‍👩‍👧‍👧","1️⃣","é","🏳️‍🌈","👩🏾‍❤️‍👩🏼","각"}) {
        Draft d;d.account=1;d.text="A"+cluster+"Z";d.runs={{0,false,false,false,{}}};auto original=draftHtml(d);int32 end=1+cluster.size();
        editor.Load(d);editor.Select(1,1);char right=B_RIGHT_ARROW,left=B_LEFT_ARROW;editor.KeyDown(&right,1);int32 a,b;editor.GetSelection(&a,&b);
        check(a==end && b==end,"Right arrow crosses a whole joined emoji or combining character");editor.KeyDown(&left,1);editor.GetSelection(&a,&b);check(a==1 && b==1,"Left arrow crosses a whole grapheme");
        editor.Select(end,end);char backspace=B_BACKSPACE;editor.KeyDown(&backspace,1);check(body(editor).text=="AZ","Backspace removes a whole grapheme");
        editor.Undo(be_clipboard);check(draftHtml(body(editor))==original,"Undo restores exact Unicode and formatting");editor.Redo();check(body(editor).text=="AZ","Redo reapplies the atomic deletion");editor.Undo(be_clipboard);
        editor.Select(1,1);char forward=B_DELETE;editor.KeyDown(&forward,1);check(body(editor).text=="AZ","Delete removes a whole grapheme");editor.Undo(be_clipboard);
        editor.Select(2,3);editor.Copy(be_clipboard);check(clipboardIs(cluster,true),"Partial selection copies a complete grapheme with portable fonts");
        editor.Cut(be_clipboard);check(body(editor).text=="AZ","Cut removes the complete copied grapheme");editor.Undo(be_clipboard);
        editor.Select(2,3);editor.Toggle(B_BOLD_FACE);auto styled=body(editor);bool bold=false;for(const auto& run:styled.runs)if(run.offset<=1)bold=run.bold;
        check(bold && styled.text==d.text && std::none_of(styled.runs.begin(),styled.runs.end(),[&](const auto& run){return run.offset>1 && run.offset<static_cast<std::size_t>(end);}),"Formatting expands to the full grapheme");editor.Undo(be_clipboard);
        editor.Select(2,3);editor.Link("https://example.test/emoji");auto linked=body(editor);bool link=false;for(const auto& run:linked.runs)if(run.offset<=1)link=run.link=="https://example.test/emoji";
        check(link && linked.text==d.text,"Link insertion keeps a joined character intact");editor.Undo(be_clipboard);
        plainClipboard("Q");editor.Select(2,3);editor.Paste(be_clipboard);auto pasted=body(editor).text;
        if(pasted!="AQZ")std::fprintf(stderr,"Paste fixture sequence %s produced %s (%zu bytes)\n",cluster.c_str(),pasted.c_str(),pasted.size());
        check(pasted=="AQZ","Paste replaces complete graphemes");editor.Undo(be_clipboard);check(draftHtml(body(editor))==original,"Undo rich replacement restores source bytes");
        bool noBreak=true;for(int32 at=1;at<end-1;++at)noBreak=noBreak && !editor.CanEndLine(at);check(noBreak,"Line-break candidates cannot split a grapheme");
        editor.Select(2,2);editor.KeyDown("x",1);check(body(editor).text.find(cluster)!=std::string::npos,"Typing at an interior byte position preserves the complete character");editor.Undo(be_clipboard);
    }
}
class Window:public ComposerWindow {
public:
    using ComposerWindow::ComposerWindow;
    void MessageReceived(BMessage* message) override {if(message->what=='gack')be_app->PostMessage(message);else ComposerWindow::MessageReceived(message);}
};
class Test:public BApplication {
public:
    explicit Test(std::string directory):BApplication("application/x-vnd.air-Mail-Graphemes-Test"),fDirectory(std::move(directory)){}
    int result=1;
    void ReadyToRun() override
    {
        try {
            fStore=std::make_shared<Store>(fDirectory+"/mail.db");Account account;account.provider=Provider::Gmail;account.email="author@example.test";account.name="Grapheme fixture";account.id=fStore->saveAccount(account);
            Draft d;d.account=account.id;d.to="recipient@example.test";d.subject="Unicode editing";d.text="Ready.";
            fOwner=new BWindow(BRect(20,30,500,300),"Grapheme fixture",B_TITLED_WINDOW,0);fOwner->Show();fComposer=new Window(fOwner,fStore,d);fComposer->Show();
            BMessage tick('tick');fTimer=std::make_unique<BMessageRunner>(BMessenger(this),&tick,100000);
        }catch(const std::exception& error){Fail(error.what());}
    }
    void MessageReceived(BMessage* message) override
    {
        if(fFailed)return;
        if(message->what=='gack'){fPending=false;return;}
        if(message->what!='tick'){BApplication::MessageReceived(message);return;}
        try {
            if(++fTicks>100)throw std::runtime_error("Native grapheme check timed out");
            if(fPending)return;
            BAutolock lock(fComposer);auto* editor=dynamic_cast<DraftEditor*>(fComposer->FindView("compose body"));auto* subject=dynamic_cast<BTextControl*>(fComposer->FindView("compose subject"));
            if(!editor || !subject)throw std::runtime_error("Composer inputs are missing");
            auto* header=subject->TextView();int32 begin,end;int32 clusterEnd=1+fCluster.size();
            if(fStep==0) {
                exercise(*editor);Draft d;d.account=1;d.text="A"+fCluster+"Z";editor->Load(d);editor->MakeFocus();editor->Select(clusterEnd,clusterEnd);Key(editor,B_LEFT_ARROW,B_SHIFT_KEY);
            }else if(fStep==1){editor->GetSelection(&begin,&end);check(begin==1 && end==clusterEnd,"Shift-Left selects an entire emoji");Key(editor,B_LEFT_ARROW,B_SHIFT_KEY);}
            else if(fStep==2){editor->GetSelection(&begin,&end);check(begin==0 && end==clusterEnd,"Repeated Shift-Left extends from the same anchor");Key(editor,B_RIGHT_ARROW,B_SHIFT_KEY);}
            else if(fStep==3){editor->GetSelection(&begin,&end);check(begin==1 && end==clusterEnd,"Shift-Right shrinks the selection by a whole character");Key(editor,B_RIGHT_ARROW,B_SHIFT_KEY);}
            else if(fStep==4){editor->GetSelection(&begin,&end);check(begin==clusterEnd && end==clusterEnd,"Shift navigation can collapse a complete emoji selection");subject->SetText(("A"+fCluster+"Z").c_str());ApplyEmojiFonts(*header);header->MakeFocus();header->Select(clusterEnd,clusterEnd);Key(header,B_BACKSPACE,0);}
            else if(fStep==5){check(std::string(header->Text())=="AZ","Native subject field deletes a complete emoji");Command(header,B_UNDO);}
            else if(fStep==6){check(std::string(header->Text())=="A"+fCluster+"Z","Native subject Undo restores the complete sequence");header->Select(2,3);Command(header,B_COPY);}
            else if(fStep==7){check(clipboardIs(fCluster,true),"Subject Copy keeps full Unicode without private font IDs");Command(header,B_CUT);}
            else if(fStep==8){check(std::string(header->Text())=="AZ" && clipboardIs(fCluster,true),"Subject Cut preserves native history and portable clipboard text");Command(header,B_UNDO);}
            else if(fStep==9){check(std::string(header->Text())=="A"+fCluster+"Z","Subject Cut Undo restores original bytes");plainClipboard("Q");header->Select(2,3);Command(header,B_PASTE);}
            else if(fStep==10){check(std::string(header->Text())=="AQZ","Subject Paste replaces a whole grapheme");Command(header,B_UNDO);}
            else if(fStep==11){check(std::string(header->Text())=="A"+fCluster+"Z","Subject Paste Undo preserves its native undo behavior");header->Select(clusterEnd,clusterEnd);Key(header,B_LEFT_ARROW,B_SHIFT_KEY);}
            else if(fStep==12){header->GetSelection(&begin,&end);check(begin==1 && end==clusterEnd,"Subject Shift-Left selects complete sequences");Key(header,B_RIGHT_ARROW,B_SHIFT_KEY);}
            else if(fStep==13){header->GetSelection(&begin,&end);check(begin==clusterEnd && end==clusterEnd,"Subject Shift-Right shrinks without getting stuck inside the emoji");fTimer.reset();result=0;std::printf("Native graphemes: %d checks passed (joined emoji, skin tones, flags, combining text, keyboard selection/deletion, formatting, clipboard and native subject Undo)\n",checks);PostMessage(B_QUIT_REQUESTED);}
            ++fStep;
        }catch(const std::exception& error){Fail(error.what());}
    }
private:
    void Key(BTextView* view,char byte,uint32 flags){BMessage key(B_KEY_DOWN);char bytes[]={byte,0};key.AddString("bytes",bytes);key.AddInt32("modifiers",flags);key.AddInt32("raw_char",byte);key.AddInt64("when",system_time());Send(view,key);}
    void Command(BTextView* view,uint32 what){BMessage message(what);Send(view,message);}
    void Send(BTextView* view,BMessage& message){fPending=true;fComposer->PostMessage(&message,view);fComposer->PostMessage('gack');}
    void Fail(const char* error){if(fFailed)return;fFailed=true;fTimer.reset();std::fprintf(stderr,"FAIL graphemes step %d: %s\n",fStep,error);PostMessage(B_QUIT_REQUESTED);}
    std::string fDirectory,fCluster="👩🏽‍💻";std::shared_ptr<Store> fStore;BWindow* fOwner=nullptr;Window* fComposer=nullptr;
    std::unique_ptr<BMessageRunner> fTimer;int fStep=0,fTicks=0;bool fPending=false,fFailed=false;
};
}
int main(){char path[]="/tmp/mail-graphemes-ui-XXXXXX";if(!mkdtemp(path))return 2;int result;{Test app(path);app.Run();result=app.result;}std::filesystem::remove_all(path);return result;}
