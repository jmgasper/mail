#include "ComposerWindow.h"
#include "DraftWindow.h"
#include "DraftEditor.h"
#include "PreviewTransfer.h"
#include "Emoji.h"
#include "AttachmentViews.h"
#include "mail/Mime.h"
#include <Alert.h>
#include <Button.h>
#include <Clipboard.h>
#include <Entry.h>
#include <FilePanel.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <MenuBar.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <MessageRunner.h>
#include <MimeType.h>
#include <NodeInfo.h>
#include <Path.h>
#include <PopUpMenu.h>
#include <ScrollView.h>
#include <StringView.h>
#include <TextControl.h>
#include <TextView.h>
#include <algorithm>
#include <ctime>
#include <fstream>
#include <set>

namespace mail::ui {
class ComposerSession {
public:
    ComposerSession(Store& store,Id id):fStore(store),fId(id),fToken(newMessageId()),snapshot(store.openDraft(id,fToken)) {}
    ~ComposerSession() { try { fStore.closeDraft(fId,fToken); } catch (...) { /* Startup clears an interrupted editor lease. */ } }
private:
    Store& fStore; Id fId; std::string fToken;
public:
    Draft snapshot;
};
namespace {
enum:uint32 { kChanged='cedt',kSave='csav',kSend='csnd',kAttach='catt',kRemove='crem',kInline='cinl',kDiscard='cdsc',
    kBold='cbld',kItalic='cita',kUnderline='cund',kEmoji='cemo',kLink='clnk',kLinkDone='clnd',kCut='ccut',kCopy='ccpy',kPaste='cpst',
    kFileSelected='casl',kPreview='capr' };
void clear(BListView* list) { while (auto* item=list->RemoveItem(int32(0))) delete item; }
class LinkWindow : public BWindow {
public:
    explicit LinkWindow(BWindow* owner):BWindow(BRect(0,0,450,110),"Insert link",B_TITLED_WINDOW_LOOK,B_MODAL_SUBSET_WINDOW_FEEL,B_AUTO_UPDATE_SIZE_LIMITS|B_CLOSE_ON_ESCAPE),fOwner(owner)
    {
        AddToSubset(owner); fURL=new BTextControl("Link:","https://",nullptr);
        auto* button=new BButton("Insert",new BMessage(kLinkDone));
        BLayoutBuilder::Group<>(this,B_VERTICAL).SetInsets(B_USE_WINDOW_INSETS).Add(fURL)
            .AddGroup(B_HORIZONTAL).AddGlue().Add(new BButton("Cancel",new BMessage(B_QUIT_REQUESTED))).Add(button).End();
        SetDefaultButton(button); CenterIn(owner->Frame()); fURL->MakeFocus();
    }
    void MessageReceived(BMessage* message) override {
        if (message->what==kLinkDone) { BMessage reply(kLinkDone); reply.AddString("url",fURL->Text()); fOwner.SendMessage(&reply); Quit(); }
        else BWindow::MessageReceived(message);
    }
private: BTextControl* fURL; BMessenger fOwner;
};
}
ComposerWindow::ComposerWindow(BWindow* owner,std::shared_ptr<Store> store,Draft draft)
    :BWindow(BRect(120,90,900,740),"New message",B_DOCUMENT_WINDOW,B_AUTO_UPDATE_SIZE_LIMITS|B_ASYNCHRONOUS_CONTROLS),fStore(std::move(store)),fDraft(std::move(draft)),fOwner(owner)
{
    if (!fDraft.id) fDraft.id=fStore->saveDraft(fDraft);
    fSession=std::make_unique<ComposerSession>(*fStore,fDraft.id); fDraft=std::move(fSession->snapshot);
    fPreviewKey=newMessageId();
    auto* bar=new BMenuBar("compose menu"); auto* file=new BMenu("Message");
    file->AddItem(new BMenuItem("Send",new BMessage(kSend),B_ENTER,B_COMMAND_KEY));
    file->AddItem(new BMenuItem("Save draft",new BMessage(kSave),'S'));
    file->AddItem(new BMenuItem("Preview message",new BMessage(kPreviewRefresh),'P'));
    file->AddItem(new BMenuItem("Attach files…",new BMessage(kAttach),'A',B_COMMAND_KEY|B_SHIFT_KEY));
    file->AddItem(new BMenuItem("Discard draft…",new BMessage(kDiscard))); bar->AddItem(file);
    auto* edit=new BMenu("Edit");
    edit->AddItem(fUndoItem=new BMenuItem("Undo",new BMessage(kEditorUndo),'Z'));
    edit->AddItem(fRedoItem=new BMenuItem("Redo",new BMessage(kEditorRedo),'Z',B_COMMAND_KEY|B_SHIFT_KEY));
    edit->AddSeparatorItem(); edit->AddItem(new BMenuItem("Cut",new BMessage(kCut),'X'));
    edit->AddItem(new BMenuItem("Copy",new BMessage(kCopy),'C')); edit->AddItem(new BMenuItem("Paste",new BMessage(kPaste),'V')); bar->AddItem(edit);
    auto* format=new BMenu("Format"); format->AddItem(new BMenuItem("Bold",new BMessage(kBold),'B'));
    format->AddItem(new BMenuItem("Italic",new BMessage(kItalic),'I')); format->AddItem(new BMenuItem("Underline",new BMessage(kUnderline),'U'));
    format->AddItem(new BMenuItem("Insert link…",new BMessage(kLink),'K')); bar->AddItem(format);
    auto* emoji=new BMenu("Emoji");
    for (const char* value:{"😀","😊","🎉","👍","❤️","🙏","🌿","✅","📅","☕","🚀","👋"}) { auto* message=new BMessage(kEmoji); message->AddString("emoji",value); emoji->AddItem(new BMenuItem(value,message)); }
    BFont emojiFont=EmojiFont(*be_plain_font); emoji->SetFont(&emojiFont);
    bar->AddItem(emoji);
    auto* identities=new BPopUpMenu("Sending account");
    for (const auto& a:fStore->accounts()) if (a.provider!=Provider::Local || a.id==fDraft.account) {
        auto* change=new BMessage(kChanged); change->AddInt64("account",a.id);
        auto* item=new BMenuItem((a.email.empty()?a.name:a.name+" <"+a.email+">").c_str(),change); identities->AddItem(item); item->SetMarked(a.id==fDraft.account);
    }
    fFrom=new BMenuField("compose from","From:",identities);
    auto field=[&](const char* name,const char* label,const std::string& value) { auto* f=new BTextControl(name,label,value.c_str(),new BMessage(kChanged)); f->SetModificationMessage(new BMessage(kChanged)); f->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED,B_SIZE_UNSET)); return f; };
    fTo=field("compose to","To:",fDraft.to); fCc=field("compose cc","Cc:",fDraft.cc); fBcc=field("compose bcc","Bcc:",fDraft.bcc); fSubject=field("compose subject","Subject:",fDraft.subject);
    for (auto* field:{fTo,fCc,fBcc,fSubject}) { field->TextView()->SetStylable(true); ApplyEmojiFonts(*field->TextView()); }
    fEditor=new DraftEditor; fEditor->Load(fDraft); auto* scroll=new BScrollView("compose scroll",fEditor,0,false,true);
    scroll->SetExplicitMinSize(BSize(500,240));
    fFiles=new AttachmentListView("compose attachments",[this](int32 index)->const Attachment* {
        return fEditor->AttachmentAt(index);
    },true);
    fFiles->SetSelectionMessage(new BMessage(kFileSelected)); fFiles->SetInvocationMessage(new BMessage(kPreview));
    auto* files=new BScrollView("compose attachment scroll",fFiles,0,false,true);
    files->SetExplicitMinSize(BSize(150,45)); files->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED,75));
    fStatus=new BStringView("compose status","Draft saved on this computer"); fStatus->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED,B_SIZE_UNSET));
    fPreview=new BButton("compose preview","Preview image",new BMessage(kPreview));
    fRemove=new BButton("remove attachment","Remove attachment",new BMessage(kRemove));
    fInline=new BButton("inline attachment","Toggle inline image",new BMessage(kInline));
    fPreview->SetEnabled(false); fRemove->SetEnabled(false); fInline->SetEnabled(false);
    BLayoutBuilder::Group<>(this,B_VERTICAL,0).Add(bar)
        .AddGroup(B_VERTICAL).SetInsets(12,8,12,8).Add(fFrom).Add(fTo).Add(fCc).Add(fBcc).Add(fSubject)
            .AddGroup(B_HORIZONTAL).Add(new BButton("Send",new BMessage(kSend))).Add(new BButton("Attach…",new BMessage(kAttach)))
                .Add(new BButton("Preview message",new BMessage(kPreviewRefresh)))
                .AddGlue().Add(new BButton("Bold",new BMessage(kBold))).Add(new BButton("Italic",new BMessage(kItalic))).Add(new BButton("Link…",new BMessage(kLink))).End()
            .Add(scroll,1).Add(files)
            .AddGroup(B_HORIZONTAL).Add(fPreview).Add(fRemove).Add(fInline).AddGlue().End()
            .AddGroup(B_HORIZONTAL).Add(fStatus,1).Add(new BButton("review drafts","Review saved versions…",new BMessage(kDraftReview))).End().End();
    RefreshFiles(); if (!fDraft.subject.empty()) SetTitle(fDraft.subject.c_str());
    BMessage tick(kSave); fTimer=std::make_unique<BMessageRunner>(BMessenger(this),&tick,2000000);
    fTo->MakeFocus();
    Save();
}
ComposerWindow::~ComposerWindow() { fSession.reset(); clear(fFiles); fOwner.SendMessage(kDraftChanged); }
void ComposerWindow::Error(const std::string& text) { (new BAlert("Mail",text.c_str(),"OK",nullptr,nullptr,B_WIDTH_AS_USUAL,B_WARNING_ALERT))->Go(); }
void ComposerWindow::Save()
{
    if (fCommitted) return;
    // Native modification messages can still be queued when Close or Preview
    // arrives. Observe edits synchronously without normalizing an untouched draft.
    int64 account=fDraft.account;
    if (auto* chosen=fFrom->Menu()->FindMarked()) if (chosen->Message()) chosen->Message()->FindInt64("account",&account);
    fDirty=fDirty || fSavedEditorChanges!=fEditor->ChangeCount() || account!=fDraft.account
        || fDraft.to!=fTo->Text() || fDraft.cc!=fCc->Text() || fDraft.bcc!=fBcc->Text() || fDraft.subject!=fSubject->Text();
    if (!fDirty) { fStatus->SetText(fStore->draftStatus(fDraft.id).c_str()); return; }
    fDraft.to=fTo->Text(); fDraft.cc=fCc->Text(); fDraft.bcc=fBcc->Text(); fDraft.subject=fSubject->Text(); fEditor->Save(fDraft);
    if (account!=fDraft.account) fDraft.providerThread.clear();
    fDraft.account=account;
    fDraft.modified=std::time(nullptr); fDraft.id=fStore->saveDraft(fDraft); fDirty=false;
    fSavedEditorChanges=fEditor->ChangeCount();
    fStatus->SetText(fStore->draftStatus(fDraft.id).c_str()); SetTitle(fDraft.subject.empty()?"New message":fDraft.subject.c_str()); fOwner.SendMessage(kDraftChanged);
}
bool ComposerWindow::QuitRequested()
{
    try { Save(); return true; } catch (const std::exception& error) { Error(error.what()); return false; }
}
void ComposerWindow::RefreshFiles()
{
    auto selected=fFiles->CurrentSelection();
    clear(fFiles);
    for (int32 i=0;const auto* file=fEditor->AttachmentAt(i);++i) fFiles->AddItem(new AttachmentItem(file->name+" · "+std::to_string(file->data.size())+" bytes"+(fEditor->IsInline(i)?" · in message":"")));
    if (selected>=0 && fFiles->CountItems()) fFiles->Select(std::min(selected,fFiles->CountItems()-1));
    PostMessage(kFileSelected);
}
void ComposerWindow::Attach(BMessage* message)
{
    bool inlineImage=false; message->FindBool("inline",&inlineImage); entry_ref ref;
    if (IsMailAttachmentOffer(*message)) {
        auto position=message->GetInt64("mail:position",0);
        BMessage context; const char* token=nullptr;
        if (message->FindMessage("be:originator-data",&context)!=B_OK || context.FindString("token",&token)!=B_OK) { fEditor->ForgetPosition(position); return; }
        for (auto it=fPendingDrops.begin();it!=fPendingDrops.end();) {
            if (system_time()-it->second.started>300000000) { fEditor->ForgetPosition(it->second.position); it=fPendingDrops.erase(it); } else ++it;
        }
        if (fPendingDrops.size()>=8 || fPendingDrops.count(token)) { fEditor->ForgetPosition(position); throw std::runtime_error("Wait for the pending attachment copies to finish."); }
        if(inlineImage && !position) { int32 begin,end; fEditor->GetSelection(&begin,&end); position=fEditor->RememberPosition(begin); }
        fPendingDrops[token]={inlineImage,system_time(),position};
        BMessage request(B_COPY_TARGET); request.AddString("be:types",kMailAttachmentType); request.AddMessage("be:originator-data",&context);
        if (message->SendReply(&request,BMessenger(this))!=B_OK) { fPendingDrops.erase(token); fEditor->ForgetPosition(position); throw std::runtime_error("The dragged attachment is no longer available."); }
        return;
    }
    std::vector<Attachment> attachments;
    int32 position=fEditor->TakePosition(message->GetInt64("mail:position",0));
    for (int32 i=0;message->FindRef("refs",i,&ref)==B_OK;++i) {
        BEntry entry(&ref,true); BPath path;
        if (!entry.IsFile() || entry.GetPath(&path)!=B_OK) throw std::runtime_error("Choose regular files to attach.");
        std::ifstream file(path.Path(),std::ios::binary|std::ios::ate); auto size=file.tellg();
        if (!file || size<0 || size>22*1024*1024) throw std::runtime_error("An attachment could not be read or exceeds 22 MiB.");
        Attachment a; a.name=path.Leaf(); a.data.resize(static_cast<std::size_t>(size)); file.seekg(0);
        if (!file.read(a.data.data(),size)) throw std::runtime_error("Could not read the complete attachment.");
        BMimeType type; if (BMimeType::GuessMimeType(&ref,&type)==B_OK) a.mediaType=type.Type();
        a.inlineImage=inlineImage && a.mediaType.rfind("image/",0)==0;
        if (a.inlineImage) { a.contentId=newMessageId(); a.contentId=a.contentId.substr(1,a.contentId.size()-2); }
        attachments.push_back(std::move(a));
    }
    fEditor->AddAttachments(std::move(attachments),position); fDirty=true; RefreshFiles(); Save();
    if (fFiles->CountItems()) fFiles->Select(fFiles->CountItems()-1);
}
void ComposerWindow::ReceiveAttachment(BMessage* message)
{
    BMessage context; const char* token=nullptr;
    if (message->FindMessage("be:originator-data",&context)!=B_OK || context.FindString("token",&token)!=B_OK) return;
    auto pending=fPendingDrops.find(token); if (pending==fPendingDrops.end()) return;
    bool inlineImage=pending->second.inlineImage; bool expired=system_time()-pending->second.started>300000000;
    auto position=fEditor->TakePosition(pending->second.position); fPendingDrops.erase(pending);
    if (expired) return;
    if (message->what!=B_MIME_DATA) { const char* error="Could not copy the dragged attachment."; message->FindString("text",&error); throw std::runtime_error(error); }
    Attachment attachment; const char *name=nullptr,*type=nullptr; int64 size=0;
    const void* data=nullptr; ssize_t length=0;
    if (message->FindString("name",&name)!=B_OK || message->FindString("media_type",&type)!=B_OK || message->FindInt64("size",&size)!=B_OK
        || size<0 || size>22*1024*1024 || (size && (message->FindData(kMailAttachmentType,B_MIME_TYPE,&data,&length)!=B_OK || length!=size)))
        throw std::runtime_error("The dragged attachment is incomplete or exceeds 22 MiB.");
    attachment.name=name; attachment.mediaType=type; if (size) attachment.data.assign(static_cast<const char*>(data),size);
    attachment.inlineImage=inlineImage && attachment.mediaType.rfind("image/",0)==0;
    if (attachment.inlineImage) { auto id=newMessageId(); attachment.contentId=id.substr(1,id.size()-2); }
    fEditor->AddAttachments({std::move(attachment)},position); fDirty=true; RefreshFiles(); Save(); fFiles->Select(fFiles->CountItems()-1);
}
void ComposerWindow::MessageReceived(BMessage* message)
{
    try {
        switch (message->what) {
            case kComposeActivate: Activate(); break;
            case kChanged:
                for (auto* field:{fTo,fCc,fBcc,fSubject}) ApplyEmojiFonts(*field->TextView());
                fDirty=true; RefreshFiles(); fStatus->SetText("Saving draft…"); break;
            case kEditorError: { const char* text=nullptr; if(message->FindString("text",&text)==B_OK) throw std::runtime_error(text); break; }
            case kSave: Save(); break;
            case kDraftReview: Save(); fOwner.SendMessage(kDraftReview); break;
            case kPreviewRefresh: {
                Save(); std::string from;
                for(const auto& account:fStore->accounts()) if(account.id==fDraft.account) from=account.email.empty()?account.name:account.email;
                LaunchPreview({fDraft,from,fPreviewKey},BMessenger(this)); break;
            }
            case kPreviewEdit: Activate(); fEditor->MakeFocus(); break;
            case kSend:
                Save(); fStore->enqueueDraft(fDraft.id,std::time(nullptr),fDraft.revision); fCommitted=true;
                fOwner.SendMessage(kSendQueued); PostMessage(B_QUIT_REQUESTED); break;
            case kAttach: {
                if (!fPanel) { BMessenger target(this); fPanel=std::make_unique<BFilePanel>(B_OPEN_PANEL,&target,nullptr,B_FILE_NODE,true); }
                fPanel->Show(); break;
            }
            case B_SIMPLE_DATA: case B_REFS_RECEIVED: Attach(message); break;
            case B_MIME_DATA: case B_REPLY: ReceiveAttachment(message); break;
            case kFileSelected: {
                auto index=fFiles->CurrentSelection(); const auto* file=fEditor->AttachmentAt(index);
                bool image=file && file->mediaType.rfind("image/",0)==0;
                fInline->SetLabel(fEditor->IsInline(index)?"Keep as attachment":"Insert in message");
                fPreview->SetEnabled(image); fInline->SetEnabled(image); fRemove->SetEnabled(file); break;
            }
            case kPreview: {
                auto index=fFiles->CurrentSelection();
                const auto* file=fEditor->AttachmentAt(index);
                if (file && file->mediaType.rfind("image/",0)==0) (new AttachmentPreviewWindow(*file))->Show();
                break;
            }
            case kAttachmentExported: {
                const char* text=nullptr; if (message->FindString("text",&text)==B_OK) {
                    fStatus->SetText(text); if (message->GetBool("failed",false)) Error(text);
                } break;
            }
            case kRemove:
                fEditor->RemoveAttachment(fFiles->CurrentSelection()); fDirty=true; RefreshFiles(); Save(); break;
            case kInline:
                fEditor->ToggleImage(fFiles->CurrentSelection()); fDirty=true; RefreshFiles(); Save(); break;
            case kDiscard:
                if ((new BAlert("Discard draft","Discard this draft and its attachments?","Cancel","Discard",nullptr,B_WIDTH_AS_USUAL,B_WARNING_ALERT))->Go()==1) {
                    fStore->removeDraft(fDraft.id,fDraft.revision); fCommitted=true; PostMessage(B_QUIT_REQUESTED);
                } break;
            case kBold: fEditor->Toggle(B_BOLD_FACE); break;
            case kItalic: fEditor->Toggle(B_ITALIC_FACE); break;
            case kUnderline: fEditor->Toggle(B_UNDERSCORE_FACE); break;
            case kLink: (new LinkWindow(this))->Show(); break;
            case kLinkDone: { const char* url=nullptr; if (message->FindString("url",&url)==B_OK) fEditor->Link(url); break; }
            case kEmoji: { const char* emoji=nullptr; if (message->FindString("emoji",&emoji)==B_OK) { fEditor->ReplaceSelection(emoji); } break; }
            case kCut: case kCopy: case kPaste: case kEditorUndo: case kEditorRedo: {
                auto* text=dynamic_cast<BTextView*>(CurrentFocus()); if (!text) text=fEditor;
                if (message->what==kCut) text->Cut(be_clipboard); else if (message->what==kCopy) text->Copy(be_clipboard);
                else if (message->what==kPaste) text->Paste(be_clipboard);
                else if(text==fEditor) { if(message->what==kEditorRedo) fEditor->Redo(); else fEditor->Undo(be_clipboard); }
                else {
                    bool redo=false; auto state=text->UndoState(&redo);
                    if(state!=B_UNDO_UNAVAILABLE && redo==(message->what==kEditorRedo)) text->Undo(be_clipboard);
                }
                break;
            }
            default: BWindow::MessageReceived(message);
        }
    } catch (const std::exception& error) { fStatus->SetText(error.what()); if (message->what!=kSave) Error(error.what()); }
}
void ComposerWindow::MenusBeginning()
{
    auto* text=dynamic_cast<BTextView*>(CurrentFocus()); if(!text) text=fEditor;
    bool undo=false,redo=false;
    if(text==fEditor) { undo=fEditor->CanUndo(); redo=fEditor->CanRedo(); }
    else { bool isRedo=false; if(text->UndoState(&isRedo)!=B_UNDO_UNAVAILABLE) {undo=!isRedo; redo=isRedo;} }
    fUndoItem->SetEnabled(undo); fRedoItem->SetEnabled(redo);
    BWindow::MenusBeginning();
}
}
