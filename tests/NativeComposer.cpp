#include "ComposerWindow.h"
#include "DraftEditor.h"
#include "AttachmentViews.h"
#include "mail/Mime.h"
#include "mail/Clipboard.h"
#include <Application.h>
#include <Autolock.h>
#include <Bitmap.h>
#include <BitmapStream.h>
#include <Clipboard.h>
#include <File.h>
#include <MessageRunner.h>
#include <MenuBar.h>
#include <MenuItem.h>
#include <Screen.h>
#include <TextControl.h>
#include <TranslatorRoster.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

using namespace mail;
using namespace mail::ui;
namespace {
int checks=0;
void check(bool value,const char* why) { ++checks; if(!value) throw std::runtime_error(why); }
Draft body(DraftEditor& editor) { Draft d; d.account=1; editor.Save(d); validateDraft(d); return d; }
void htmlClipboard(DraftEditor& editor,const Attachment& image)
{
    BClipboard& clipboard=*be_clipboard;
    auto put=[&](const std::string& html,const char* plain) {
        check(clipboard.Lock(),"lock native HTML clipboard");clipboard.Clear();auto* data=clipboard.Data();
        data->AddData("text/html",B_MIME_TYPE,html.data(),html.size());
        if(plain)data->AddData("text/plain",B_MIME_TYPE,plain,std::char_traits<char>::length(plain));
        check(clipboard.Commit()==B_OK,"commit native HTML clipboard");clipboard.Unlock();
    };
    auto html="<p><b>Hello café 🎉</b> <a href='https://example.test/clipboard'>link</a> <img src='data:image/png;base64,"+base64Encode(image.data)+"'></p>";
    Draft seed;seed.account=1;seed.text="Before after";editor.Load(seed);editor.Select(7,7);put(html,"Hello café 🎉 link image");editor.Paste(&clipboard);auto pasted=body(editor);
    check(pasted.text.find("Before Hello café 🎉 link")!=std::string::npos && pasted.images.size()==1 && pasted.attachments[0].data==image.data,"native HTML paste imports text, emoji and an embedded binary image");
    check(draftHtml(pasted).find("<strong>Hello café 🎉</strong>")!=std::string::npos && draftHtml(pasted).find("href=\"https://example.test/clipboard\"")!=std::string::npos,"native HTML paste keeps semantic styles and link destination");
    auto savedHtml=draftHtml(pasted);editor.Undo(&clipboard);check(body(editor).text==seed.text && body(editor).attachments.empty(),"one Undo removes the entire rich HTML paste");
    editor.Redo();check(draftHtml(body(editor))==savedHtml && body(editor).attachments[0].data==image.data,"Redo restores HTML styles, positions and image bytes together");
    editor.Select(0,editor.TextLength());editor.Copy(&clipboard);
    check(clipboard.Lock(),"inspect exported HTML clipboard");const void* bytes=nullptr;ssize_t size=0;auto* data=clipboard.Data();
    check(data->FindData("text/html",B_MIME_TYPE,&bytes,&size)==B_OK && std::string(static_cast<const char*>(bytes),size).find("data:image/png;base64,")!=std::string::npos,"Copy offers self-contained HTML to other apps");
    check(data->HasMessage("mail:fragment") && data->HasData("text/plain",B_MIME_TYPE),"HTML export retains Mail and native plain representations");
    data->RemoveName("mail:fragment");data->RemoveName("application/x-vnd.Be-text_run_array");clipboard.Commit();clipboard.Unlock();
    Draft empty;empty.account=1;editor.Load(empty);editor.Paste(&clipboard);auto imported=body(editor);
    check(imported.text==pasted.text && imported.images.size()==1 && imported.attachments[0].data==image.data,"HTML from another application can round-trip through native clipboard");
    editor.Select(0,editor.TextLength());editor.Copy(&clipboard);check(clipboard.Lock(),"modify only the plain clipboard representation");data=clipboard.Data();data->RemoveName("text/plain");data->AddData("text/plain",B_MIME_TYPE,"Fresh plain",11);clipboard.Commit();clipboard.Unlock();
    editor.Load(empty);editor.Paste(&clipboard);check(body(editor).text=="Fresh plain" && body(editor).images.empty(),"stale Mail HTML does not override a changed plain representation");
    editor.Load(empty);put(html,nullptr);BMessage paste(B_PASTE);editor.MessageReceived(&paste);
    check(body(editor).images.size()==1,"native Paste message accepts an HTML-only clipboard");
    editor.Load(seed);editor.Select(0,6);put(std::string(kClipboardHtmlLimit+1,'x'),"Fallback");editor.Paste(&clipboard);
    check(body(editor).text=="Fallback after","oversized HTML falls back to available native plain text");editor.Undo(&clipboard);check(body(editor).text==seed.text,"fallback paste remains one undo step");
    editor.Select(0,6);put("<script>ignored()</script>",nullptr);editor.Paste(&clipboard);check(body(editor).text==seed.text && editor.CanRedo(),"empty filtered HTML leaves selection and redo intact");
    std::string nested;for(int i=0;i<140;++i)nested+="<div>";nested+='x';for(int i=0;i<140;++i)nested+="</div>";put(nested,nullptr);
    bool rejected=false;try{editor.Paste(&clipboard);}catch(const std::exception&){rejected=true;}check(rejected && body(editor).text==seed.text && editor.CanRedo(),"HTML-only parse failure preserves the draft and redo branch");
    if(clipboard.Lock()){clipboard.Clear();clipboard.Commit();clipboard.Unlock();}
}
void history(DraftEditor& editor,const Attachment& image)
{
    Draft seed; seed.account=1; seed.text="Start café 🎉.\nEnd."; editor.Load(seed);
    check(!editor.CanUndo() && !editor.CanRedo(),"loading a draft starts with empty editing history");
    std::vector<Draft> states{body(editor)};
    editor.Select(0,0); editor.ReplaceSelection("One "); states.push_back(body(editor));
    editor.Select(0,3); editor.Toggle(B_BOLD_FACE); states.push_back(body(editor));
    editor.Link("https://example.test/history"); states.push_back(body(editor));
    editor.AddAttachments({image},editor.TextLength()); states.push_back(body(editor));
    editor.RemoveAttachment(0); states.push_back(body(editor));
    auto same=[&](const Draft& expected) {auto actual=body(editor); return draftHtml(actual)==draftHtml(expected) && actual.attachments.size()==expected.attachments.size()
        && (actual.attachments.empty() || actual.attachments[0].data==expected.attachments[0].data);};
    for(std::size_t i=states.size()-1;i>0;--i) {editor.Undo(be_clipboard);check(same(states[i-1]),"multiple Undo steps restore text, styles, links and attachment changes in order");}
    check(!editor.CanUndo() && editor.CanRedo(),"undo boundary leaves redo available"); editor.Undo(be_clipboard); check(same(states[0]),"Undo at the boundary leaves the draft intact");
    for(std::size_t i=1;i<states.size();++i) {editor.Redo();check(same(states[i]),"multiple Redo steps recover the full rich edit sequence");}
    check(editor.CanUndo() && !editor.CanRedo(),"redo boundary restores undo availability");
    editor.Undo(be_clipboard); auto before=body(editor); Attachment invalid=image; invalid.name="bad\nname";
    bool rejected=false; try{editor.AddAttachments({invalid});}catch(const std::exception&){rejected=true;}
    check(rejected && same(before) && editor.CanRedo(),"failed edits retain content and the redo branch");
    editor.Select(editor.TextLength(),editor.TextLength()); editor.ReplaceSelection(""); check(editor.CanRedo(),"no-op edits do not discard redo");
    editor.Redo(); check(same(states.back()),"redo still works after a failed edit and a no-op");
    editor.Undo(be_clipboard); editor.Select(0,0); editor.ReplaceSelection("New branch ");
    check(!editor.CanRedo(),"a new edit discards only the old redo branch"); editor.Undo(be_clipboard);check(same(states[states.size()-2]),"undoing a branched edit retains older history");
    Draft empty;empty.account=1;editor.Load(empty);editor.Toggle(B_BOLD_FACE);
    check(editor.CanUndo(),"a caret style change creates an undo step");
    for(char c:std::string("ABC")) editor.KeyDown(&c,1);
    editor.Undo(be_clipboard);
    check(editor.TextLength()==0 && editor.CanUndo(),"typing undo retains the preceding caret style step");
    editor.Undo(be_clipboard);check(!editor.CanUndo() && editor.CanRedo(),"caret style has its own undo step");
    editor.Redo();editor.Redo();check(body(editor).text=="ABC" && draftHtml(body(editor)).find("<strong>ABC</strong>")!=std::string::npos,"redo restores caret formatting and grouped typing");
    char backspace=B_BACKSPACE;editor.KeyDown(&backspace,1);editor.KeyDown(&backspace,1);
    check(body(editor).text=="A","backspace edits the current group");editor.Undo(be_clipboard);check(body(editor).text=="ABC","consecutive backspaces undo together");
    editor.Undo(be_clipboard);check(editor.TextLength()==0,"backspace and typing remain separate history steps");
    editor.Undo(be_clipboard);editor.KeyDown("p",1);
    check(draftHtml(body(editor)).find("<strong>")==std::string::npos,"undoing the caret style restores plain future typing");
    seed.text="Plain";editor.Load(seed);editor.Select(5,5);editor.Toggle(B_BOLD_FACE);editor.Toggle(B_ITALIC_FACE);editor.KeyDown("X",1);
    check(draftHtml(body(editor)).find("<strong><em>X</em></strong>")!=std::string::npos,"successive caret styles combine at the end of existing text");
    editor.Undo(be_clipboard);editor.Undo(be_clipboard);editor.KeyDown("Y",1);
    check(draftHtml(body(editor)).find("<strong>Y</strong>")!=std::string::npos,"undoing one caret style retains the earlier style for new typing");
    editor.Select(0,0);editor.KeyDown("Z",1);
    check(body(editor).runs[0].bold==false && body(editor).runs[0].italic==false,"moving the caret adopts the destination text style");
    editor.Load(empty);char a='a',b='b';editor.KeyDown(&a,1);editor.MakeFocus(false);editor.MakeFocus(true);editor.KeyDown(&b,1);
    editor.Undo(be_clipboard);check(body(editor).text=="a","changing focus ends a typing group even at the same caret");
    // History shares a large current attachment rather than retaining a new
    // binary copy for every character edit.
    empty.attachments={{"retained.bin","application/octet-stream",{},std::string(8*1024*1024,'r'),false}};
    editor.Load(empty);const auto* original=editor.AttachmentAt(0);empty.attachments.clear();
    for(int i=0;i<130;++i) editor.ReplaceSelection("x");
    int undone=0;while(editor.CanUndo()){editor.Undo(be_clipboard);++undone;}
    check(undone==100 && editor.TextLength()==30 && editor.AttachmentAt(0)==original,"history keeps the latest 100 edits while sharing attachment bytes");
    int redone=0;while(editor.CanRedo()){editor.Redo();++redone;}
    check(redone==100 && editor.TextLength()==130 && editor.AttachmentAt(0)==original,"bounded history remains fully redoable");
    editor.Load(empty);
    for(int i=0;i<8;++i) {Attachment file{"history.bin","application/octet-stream",{},std::string(12*1024*1024,'a'+i),false};editor.AddAttachments({std::move(file)});editor.RemoveAttachment(0);}
    editor.Undo(be_clipboard);check(editor.AttachmentAt(0) && editor.AttachmentAt(0)->data.size()==12*1024*1024 && editor.AttachmentAt(0)->data[0]=='h',"memory trimming retains the most recent removed attachment");
    undone=1;while(editor.CanUndo()){editor.Undo(be_clipboard);++undone;}
    check(undone>=2 && undone<16,"large removed files trigger history memory bounds before the step limit");
    editor.Load(seed);check(!editor.CanUndo() && !editor.CanRedo(),"opening another draft releases both history directions");
}
class ComposerTest : public BApplication {
public:
    explicit ComposerTest(std::string directory):BApplication("application/x-vnd.air-Mail-Composer-Test"),fDirectory(std::move(directory)) {}
    int result=1;
    void ReadyToRun() override
    {
        try {
            fStore=std::make_shared<Store>(fDirectory+"/mail.db"); Account a; a.name="Composer fixture"; a.email="author@example.test"; a.provider=Provider::Gmail; a.id=fStore->saveAccount(a);
            fDraft.account=a.id; fDraft.to="recipient@example.test"; fDraft.subject="Images in a message";
            fDraft.text="Hello café 🎉 — before.\nAfter the image.\n"; fDraft.runs={{0,true,false,false,{}},{5,false,false,false,{}}}; fDraft.id=fStore->saveDraft(fDraft);
            fImage={"garden café.png","image/png","garden@test",base64Decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+a5e8AAAAASUVORK5CYII="),true};
            fOwner=new BWindow(BRect(20,30,700,600),"Composer fixture",B_TITLED_WINDOW,0); fOwner->Show();
            fComposer=new ComposerWindow(fOwner,fStore,fDraft); fComposer->Show();
            BMessage tick('test'); fTimer=std::make_unique<BMessageRunner>(BMessenger(this),&tick,150000);
        } catch(const std::exception& e) { Fail(e.what()); }
    }
    void MessageReceived(BMessage* message) override
    {
        if(message->what==B_COPY_TARGET) { fReplyTo=message->ReturnAddress(); fRequest=*message; return; }
        if(message->what!='test') { BApplication::MessageReceived(message); return; }
        try {
            if(++fTicks>180) throw std::runtime_error("Native composer test timed out");
            if(fStep==0) {
                BAutolock lock(fComposer); auto* editor=dynamic_cast<DraftEditor*>(fComposer->FindView("compose body")); if(!editor) throw std::runtime_error("native editor missing");
                if(!fHistoryDone) {
                    // The memory-limit exercise spans several timer intervals.
                    // Let its queued ticks drain before observing asynchronous
                    // window shortcut dispatch.
                    if(system_time()<fHistoryAfter) return;
                    auto key=[&](bool redo) {BMessage message(B_KEY_DOWN);message.AddString("bytes","z");message.AddInt32("raw_char",'z');message.AddInt32("modifiers",B_COMMAND_KEY|(redo?B_SHIFT_KEY:0));check(BMessenger(nullptr,fComposer).SendMessage(&message)==B_OK,"send native history shortcut");fHistoryAfter=system_time()+150000;};
                    auto* subject=dynamic_cast<BTextControl*>(fComposer->FindView("compose subject"));
                    auto* menu=dynamic_cast<BMenuBar*>(fComposer->FindView("compose menu"));
                    auto* undo=menu->FindItem("Undo");auto* redo=menu->FindItem("Redo");
                    if(fHistoryStep==0) {
                        check(editor,"native editor exists");
                        htmlClipboard(*editor,fImage);
                        history(*editor,fImage);editor->Load(fDraft);editor->Select(editor->TextLength(),editor->TextLength());editor->Insert("A");editor->Insert("B");editor->MakeFocus();
                        fComposer->MenusBeginning();uint32 mods=0;check(redo && redo->Shortcut(&mods)=='Z' && (mods&(B_COMMAND_KEY|B_SHIFT_KEY))==(B_COMMAND_KEY|B_SHIFT_KEY),"Redo has the native Command-Shift-Z shortcut");
                        check(undo->IsEnabled() && !redo->IsEnabled(),"body menu reflects available history");key(false);
                    } else if(fHistoryStep==1) {check(body(*editor).text==fDraft.text+"A","Command-Z undoes the latest body edit");key(false);
                    } else if(fHistoryStep==2) {check(body(*editor).text==fDraft.text,"repeated Command-Z reaches an earlier edit");key(false);
                    } else if(fHistoryStep==3) {check(body(*editor).text==fDraft.text,"disabled Undo does not insert a shortcut character");key(true);
                    } else if(fHistoryStep==4) {check(body(*editor).text==fDraft.text+"A","Command-Shift-Z redoes one body edit");key(true);
                    } else if(fHistoryStep==5) {check(body(*editor).text==fDraft.text+"AB","repeated Command-Shift-Z redoes the next edit");key(true);
                    } else if(fHistoryStep==6) {
                        check(body(*editor).text==fDraft.text+"AB","disabled Redo keeps the body unchanged");subject->MakeFocus();auto* text=subject->TextView();text->Select(text->TextLength(),text->TextLength());text->KeyDown("X",1);key(false);
                    } else if(fHistoryStep==7) {
                        check(std::string(subject->Text())==fDraft.subject && body(*editor).text==fDraft.text+"AB","Undo follows the focused header without changing body history");
                        fComposer->MenusBeginning();check(!undo->IsEnabled() && redo->IsEnabled(),"header menu reflects native single-step redo state");key(true);
                    } else {
                        check(std::string(subject->Text())==fDraft.subject+"X","Redo restores the focused header edit");subject->SetText(fDraft.subject.c_str());editor->Load(fDraft);editor->MakeFocus();fHistoryDone=true;
                    }
                    ++fHistoryStep;if(!fHistoryDone)return;
                }
                editor->Select(editor->TextLength(),editor->TextLength());
                for(const char key:std::string("abc")) editor->KeyDown(&key,1);
                check(body(*editor).text==fDraft.text+"abc","native typing appends consecutive characters");
                editor->Undo(be_clipboard); check(body(*editor).text==fDraft.text,"undo groups consecutive typing");
                editor->Redo(); check(body(*editor).text==fDraft.text+"abc","redo restores a typing group");
                editor->Undo(be_clipboard);
                int32 at=fDraft.text.find("After"); editor->AddAttachments({fImage},at); auto d=body(*editor);
                check(d.images.size()==1 && d.images[0].offset==static_cast<std::size_t>(at) && d.attachments[0].inlineImage,"image inserted at text caret");
                check(draftHtml(d).find("cid:garden@test")<draftHtml(d).find("After the image"),"image precedes following text in composed HTML");
                auto original=d.text; editor->Insert(0,"Prefix ",7); d=body(*editor);
                check(d.images[0].offset==static_cast<std::size_t>(at+7),"insertion moves image range");
                editor->Undo(be_clipboard); check(body(*editor).text==original && body(*editor).images[0].offset==static_cast<std::size_t>(at),"undo restores text and image range together");
                editor->Redo(); check(body(*editor).images[0].offset==static_cast<std::size_t>(at+7),"redo restores image range");
                d=body(*editor); auto image=d.images[0];
                editor->Select(image.offset+2,image.offset+3); editor->Toggle(B_BOLD_FACE); auto formatted=body(*editor);
                check(draftHtml(formatted).find("<strong><img")!=std::string::npos,"formatting a partial placeholder styles the entire image");
                editor->Undo(be_clipboard); check(draftHtml(body(*editor))==draftHtml(d),"format undo restores image styling");
                editor->Select(image.offset,image.offset+image.length); editor->Copy(be_clipboard); editor->Select(editor->TextLength(),editor->TextLength()); editor->Paste(be_clipboard);
                check(body(*editor).images.size()==2 && body(*editor).attachments.size()==1,"copying an image reuses one binary part within the draft");
                editor->Undo(be_clipboard); check(body(*editor).images.size()==1,"undo image paste"); editor->Redo(); check(body(*editor).images.size()==2,"redo image paste");
                editor->Select(image.offset,image.offset+image.length); editor->Cut(be_clipboard); check(body(*editor).images.size()==1,"cut removes only selected placement");
                editor->Undo(be_clipboard); check(body(*editor).images.size()==2,"cut undo restores image placement");
                editor->RemoveAttachment(0); check(body(*editor).images.empty() && body(*editor).attachments.empty(),"removing image removes every placement");
                editor->Undo(be_clipboard); check(body(*editor).images.size()==2 && body(*editor).attachments[0].data==fImage.data,"remove undo restores positions and original binary data");
                editor->ToggleImage(0); d=body(*editor); check(d.images.empty() && !d.attachments[0].inlineImage && draftHtml(d).find("cid:")==std::string::npos,"keep as attachment removes all placements without appending an image");
                editor->Undo(be_clipboard); editor->Select(image.offset+1,image.offset+1); editor->Insert("x");
                check(body(*editor).images.size()==1,"editing inside a marker invalidates only that placement");
                editor->Undo(be_clipboard); check(body(*editor).images.size()==2,"undo restores a damaged placeholder and its mapping");
                auto hello=std::string(editor->Text()).find("Hello"); editor->Select(hello,hello+5); editor->Link("https://example.test/garden");
                auto linked=draftHtml(body(*editor)); check(linked.find("href=\"https://example.test/garden\"")!=std::string::npos,"link added");
                editor->Select(hello+1,hello+3); editor->Link("https://example.test/other");
                check(draftHtml(body(*editor)).find("href=\"https://example.test/garden\"")!=std::string::npos,"link edit preserves portions outside selection");
                editor->Undo(be_clipboard); check(draftHtml(body(*editor))==linked,"undo restores original link spans");
                editor->Select(0,editor->TextLength()); editor->Copy(be_clipboard);
                auto other=std::make_unique<DraftEditor>(); Draft empty; empty.account=fDraft.account; other->Load(empty); other->Paste(be_clipboard); auto copied=body(*other);
                check(copied.images.size()==2 && copied.attachments.size()==1 && copied.attachments[0].data==fImage.data,"cross-editor paste carries image bytes and both positions");
                check(copied.images[0].contentId!=fImage.contentId && draftHtml(copied).find("https://example.test/garden")!=std::string::npos,"cross-editor paste assigns a unique CID and preserves links");
                auto anchor=editor->RememberPosition(image.offset); editor->Insert(0,"ABC",3);
                check(editor->TakePosition(anchor)==static_cast<int32>(image.offset+3),"pending drop follows insertion");
                anchor=editor->RememberPosition(image.offset+3); editor->Undo(be_clipboard);
                check(editor->TakePosition(anchor)==static_cast<int32>(image.offset),"pending drop follows undo");
                auto before=body(*editor); Attachment invalid=fImage; invalid.name="bad\nname"; invalid.contentId="invalid@test";
                bool rejected=false; try { editor->AddAttachments({invalid},0); } catch(const std::exception&) { rejected=true; }
                check(rejected && draftHtml(body(*editor))==draftHtml(before),"failed image insertion rolls back text, metadata and attachments");
                Draft legacy=fDraft; legacy.attachments={fImage}; other->Load(legacy); copied=body(*other);
                check(copied.images.size()==1 && copied.images[0].offset>=legacy.text.size(),"legacy trailing image receives a visible saved placement");
                other->Select(copied.images[0].offset+1,copied.images[0].offset+1); other->Insert("x"); copied=body(*other);
                check(copied.images.empty() && !copied.attachments[0].inlineImage && draftHtml(copied).find("cid:")==std::string::npos,"damaging the final marker keeps a regular file without silently appending an image");
                other->Undo(be_clipboard); check(body(*other).images.size()==1,"undo restores final image marker");
                // Use a delayed negotiated transfer to prove that moving the caret
                // after the drop does not redirect the arriving image.
                editor->Load(fDraft); editor->Select(6,6);
                BMessage offer(B_SIMPLE_DATA),context; context.AddString("token","delayed-fixture"); offer.AddMessage("be:originator-data",&context);
                offer.AddString("be:types",kMailAttachmentType); offer.AddInt32("be:actions",B_COPY_TARGET);
                BMessenger(editor).SendMessage(&offer,BMessenger(this)); fStep=1;
            } else if(fStep==1) {
                if(fRequest.what!=B_COPY_TARGET) return;
                BAutolock lock(fComposer); auto* editor=dynamic_cast<DraftEditor*>(fComposer->FindView("compose body"));
                editor->Insert(0,"New ",4); editor->Select(editor->TextLength(),editor->TextLength());
                BMessage reply(B_MIME_DATA),context; fRequest.FindMessage("be:originator-data",&context); reply.AddMessage("be:originator-data",&context);
                reply.AddString("name",fImage.name.c_str()); reply.AddString("media_type",fImage.mediaType.c_str()); reply.AddInt64("size",fImage.data.size());
                reply.AddData(kMailAttachmentType,B_MIME_TYPE,fImage.data.data(),fImage.data.size()); check(fReplyTo.SendMessage(&reply)==B_OK,"reply to negotiated attachment request"); fStep=2;
            } else if(fStep==2) {
                auto saved=fStore->draft(fDraft.id); if(saved.images.empty()) return;
                check(saved.images.size()==1 && saved.images[0].offset==10,"delayed negotiated drop retains intended position after edits and caret move");
                check(saved.attachments[0].data==fImage.data,"negotiated drop retains bytes"); fSavedHtml=draftHtml(saved);
                fComposer->PostMessage(B_QUIT_REQUESTED); fComposer=nullptr; fStep=3;
            } else if(fStep==3) {
                for(int32 i=0;i<CountWindows();++i) if(dynamic_cast<ComposerWindow*>(WindowAt(i))) return;
                fComposer=new ComposerWindow(fOwner,fStore,fStore->draft(fDraft.id)); fComposer->Show(); fStep=4;
            } else if(fStep==4) {
                BAutolock lock(fComposer); auto* editor=dynamic_cast<DraftEditor*>(fComposer->FindView("compose body"));
                check(draftHtml(body(*editor))==fSavedHtml,"reopening native composer retains image layout"); editor->Select(editor->TextLength(),editor->TextLength()); fStep=5;
            } else if(fStep==5) {
                if(const char* path=std::getenv("MAIL_COMPOSE_SCREENSHOT")) {
                    BBitmap* bitmap=nullptr; check(BScreen().GetBitmap(&bitmap)==B_OK,"capture composer"); BBitmapStream stream(bitmap); BFile file(path,B_WRITE_ONLY|B_CREATE_FILE|B_ERASE_FILE);
                    check(BTranslatorRoster::Default()->Translate(&stream,nullptr,nullptr,&file,B_PNG_FORMAT)==B_OK,"write composer capture");
                }
                fComposer->PostMessage('csnd'); fComposer=nullptr; fStep=6;
            } else if(fStep==6) {
                auto outgoing=fStore->outbox(); if(outgoing.empty()) return;
                auto html=parseMessage(outgoing[0].raw).html; html.erase(std::remove(html.begin(),html.end(),'\r'),html.end());
                check(outgoing.size()==1 && html==fSavedHtml,"native Send serializes exact saved image layout with MIME line endings");
                check(fStore->returnToDraft(outgoing[0].id)==fDraft.id && draftHtml(fStore->draft(fDraft.id))==fSavedHtml,"delivery recovery retains editable native image positions");
                result=0; std::printf("Native composer: %d checks passed (bounded multilevel history, caret styles, native shortcuts, placement, clipboard, delayed drops, reopen and queued MIME)\n",checks); std::fflush(stdout);
                fTimer.reset(); PostMessage(B_QUIT_REQUESTED); fStep=7;
            }
        } catch(const std::exception& e) { Fail(e.what()); }
    }
private:
    void Fail(const char* error) { std::fprintf(stderr,"FAIL composer step %d: %s\n",fStep,error); fTimer.reset(); PostMessage(B_QUIT_REQUESTED); }
    std::string fDirectory,fSavedHtml;
    std::shared_ptr<Store> fStore;
    Draft fDraft; Attachment fImage; BMessage fRequest; BMessenger fReplyTo;
    BWindow* fOwner=nullptr; ComposerWindow* fComposer=nullptr;
    bigtime_t fHistoryAfter=0;
    std::unique_ptr<BMessageRunner> fTimer;
    int fStep=0,fTicks=0,fHistoryStep=0;bool fHistoryDone=false;
};
}
int main()
{
    char directory[]="/tmp/mail-compose-ui-XXXXXX"; if(!mkdtemp(directory)) return 2;
    int result; { ComposerTest app(directory); app.Run(); result=app.result; }
    std::filesystem::remove_all(directory); return result;
}
