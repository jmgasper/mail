#include "MailWindow.h"
#include "SnoozeWindow.h"
#include "ReminderWindow.h"
#include "mail/Snooze.h"
#include "Emoji.h"
#include "AccountWindow.h"
#include "Credentials.h"
#include "ComposerWindow.h"
#include "OutboxWindow.h"
#include "MoveWindow.h"
#include "DraftWindow.h"
#include "OrganizationWindow.h"
#include "AttachmentViews.h"
#include "PreviewTransfer.h"
#include "mail/Outbox.h"
#include "mail/Mime.h"
#include "mail/Sync.h"
#include <Alert.h>
#include <Application.h>
#include <Button.h>
#include <CheckBox.h>
#include <Entry.h>
#include <File.h>
#include <FilePanel.h>
#include <GroupView.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <Menu.h>
#include <MenuBar.h>
#include <MenuItem.h>
#include <MessageRunner.h>
#include <Messenger.h>
#include <Path.h>
#include <ScrollView.h>
#include <SplitView.h>
#include <String.h>
#include <StringView.h>
#include <TextControl.h>
#include <TextView.h>
#include <algorithm>
#include <ctime>
#include <fstream>
#include <set>
#include <stdexcept>

namespace mail::ui {
namespace {
enum : uint32 {
    kImport='mimp', kImported='mimD', kView='mviw', kSelect='msel', kSearch='msch',
    kArchive='marc', kTrash='mtrs', kInbox='minb', kRead='mred', kUnread='munr',
    kStar='mstr', kSnooze='msnz',kQuickSnooze='ms1h', kWake='mwak', kTick='mtik', kTag='mtag',
    kSaveSmart='msmt', kSaveFilter='mflt', kEditDone='medt', kSaveAttachment='matt', kAttachmentSaved='masv',
    kAccounts='macc', kCheckMail='mchk', kSyncProgress='msyp', kSyncDone='msyd',kDraftTick='mdtk',
    kCompose='mnew',kReply='mrpl',kReplyAll='mrpa',kForward='mfwd',kOutbox='mout',kSendDone='msdd',
    kAttachmentSelected='masl',kPreviewAttachment='mapr',kManageRules='mmrl',kManageSmart='mmsf',kManageTags='mmtg',kRemoveTag='mrtg',kFormatted='mfmt'
};
Timestamp now() { return static_cast<Timestamp>(std::time(nullptr)); }
void clear(BListView* list) { while (auto* item=list->RemoveItem(int32(0))) delete item; }
std::string date(Timestamp time)
{
    std::time_t value=static_cast<std::time_t>(time); std::tm tm{}; localtime_r(&value,&tm);
    char text[80]; std::strftime(text,sizeof(text),"%d %b %Y, %H:%M",&tm); return text;
}
std::string singleLine(std::string s)
{
    for (char& c : s) if (c=='\r' || c=='\n' || c=='\t') c=' ';
    return s;
}
class MailboxItem : public BStringItem {
public:
    explicit MailboxItem(const char* text):BStringItem(text) {}
    void DrawItem(BView* owner,BRect bounds,bool) override
    {
        owner->SetHighColor(ui_color(IsSelected()?B_LIST_SELECTED_BACKGROUND_COLOR:B_LIST_BACKGROUND_COLOR)); owner->FillRect(bounds);
        owner->SetLowColor(owner->HighColor()); owner->SetHighColor(ui_color(IsSelected()?B_LIST_SELECTED_ITEM_TEXT_COLOR:B_LIST_ITEM_TEXT_COLOR));
        BFont font; owner->GetFont(&font); DrawEmojiText(*owner,singleLine(Text()),font,{bounds.left+5,bounds.top+BaselineOffset()},bounds.Width()-10);
    }
};
class ConversationItem : public BListItem {
public:
    explicit ConversationItem(Conversation value) : conversation(std::move(value)) {}
    void Update(BView* owner,const BFont* font) override
    {
        BListItem::Update(owner,font); SetHeight(font->Size()*5.5f); SetWidth(200);
    }
    void DrawItem(BView* owner,BRect frame,bool) override
    {
        auto background=ui_color(IsSelected()?B_LIST_SELECTED_BACKGROUND_COLOR:B_LIST_BACKGROUND_COLOR);
        auto foreground=ui_color(IsSelected()?B_LIST_SELECTED_ITEM_TEXT_COLOR:B_LIST_ITEM_TEXT_COLOR);
        owner->SetHighColor(background); owner->FillRect(frame); owner->SetLowColor(background);
        owner->SetHighColor(foreground);
        BFont plain; owner->GetFont(&plain); BFont title=plain;
        title.SetFace(conversation.unread?B_BOLD_FACE:B_REGULAR_FACE);
        float inset=plain.Size()*0.7f, y=frame.top+plain.Size()*1.5f;
        auto line=[&](const std::string& text,const BFont& font) {
            DrawEmojiText(*owner,singleLine(text),font,BPoint(frame.left+inset,y),frame.Width()-inset*2);
            y+=plain.Size()*1.35f;
        };
        std::string label=conversation.sender;
        if (conversation.starred) label="★ "+label;
        if (conversation.count>1) label+="  ("+std::to_string(conversation.count)+")";
        line(label,title);
        line(conversation.subject.empty()?"(No subject)":conversation.subject,plain);
        auto muted=tint_color(foreground,0.7f); owner->SetHighColor(muted);
        line(conversation.preview,plain);
        owner->SetFont(&plain);
    }
    Conversation conversation;
};
// Small native forms share the same commit/cancel behavior and send results
// to the mailbox looper rather than retaining a raw owner pointer.
class EditWindow : public BWindow {
public:
    EditWindow(BWindow* owner,uint32 action,const char* title,const char* prompt)
        : BWindow(BRect(0,0,440,180),title,B_TITLED_WINDOW_LOOK,B_MODAL_SUBSET_WINDOW_FEEL,
            B_AUTO_UPDATE_SIZE_LIMITS|B_CLOSE_ON_ESCAPE),fOwner(owner),fAction(action)
    {
        AddToSubset(owner);
        fName=new BTextControl("name",prompt,"",nullptr);
        fName->SetModificationMessage(new BMessage('etxt'));
        auto* save=new BButton(action==kRemoveTag?"Remove tag":"Add tag",new BMessage(kEditDone));
        BLayoutBuilder::Group<>(this,B_VERTICAL).SetInsets(B_USE_WINDOW_INSETS)
            .Add(fName)
            .AddGroup(B_HORIZONTAL).AddGlue().Add(new BButton("Cancel",new BMessage(B_QUIT_REQUESTED))).Add(save).End();
        SetDefaultButton(save); CenterIn(owner->Frame()); fName->MakeFocus();
    }
    void MessageReceived(BMessage* message) override
    {
        if (message->what=='etxt') { ApplyEmojiFonts(*fName->TextView()); return; }
        if (message->what==kEditDone) {
            if (fName->Text()[0]=='\0') { fName->MakeFocus(); return; }
            BMessage reply(fAction); reply.AddString("value",fName->Text()); fOwner.SendMessage(&reply); Quit(); return;
        }
        BWindow::MessageReceived(message);
    }
private:
    BMessenger fOwner; uint32 fAction; BTextControl* fName;
};

}

MailWindow::MailWindow(std::shared_ptr<Store> store,GoogleOptions google,std::string database)
    : BWindow(BRect(70,60,1180,780),"Mail",B_DOCUMENT_WINDOW,B_AUTO_UPDATE_SIZE_LIMITS|B_ASYNCHRONOUS_CONTROLS),fStore(std::move(store)),fGoogle(std::move(google)),fDatabase(std::move(database))
{
    for (const auto& a : fStore->accounts()) if (a.provider==Provider::Local) { fLocalAccount=a.id; break; }
    if (!fLocalAccount) { Account local; local.name="On this computer"; fLocalAccount=fStore->saveAccount(local); }
    fStore->recoverOutgoing();
    auto* menuBar=new BMenuBar("menu");
    auto* file=new BMenu("File");
    file->AddItem(new BMenuItem("New message",new BMessage(kCompose),'N'));
    file->AddItem(new BMenuItem("Outbox…",new BMessage(kOutbox)));
    file->AddItem(new BMenuItem("Folder moves…",new BMessage(kFolderMoves)));
    file->AddItem(new BMenuItem("Draft synchronization…",new BMessage(kDraftReview)));
    file->AddItem(new BMenuItem("Reminders…",new BMessage(kReminders)));
    file->AddItem(new BMenuItem("Check mail",new BMessage(kCheckMail),'M'));
    file->AddItem(new BMenuItem("Accounts…",new BMessage(kAccounts)));
    file->AddSeparatorItem();
    file->AddItem(new BMenuItem("Import mail…",new BMessage(kImport),'O'));
    file->AddSeparatorItem(); file->AddItem(new BMenuItem("Quit",new BMessage(B_QUIT_REQUESTED),'Q')); menuBar->AddItem(file);
    auto* messages=new BMenu("Message");
    messages->AddItem(new BMenuItem("Reply",new BMessage(kReply),'R'));
    messages->AddItem(new BMenuItem("Reply all",new BMessage(kReplyAll),'R',B_COMMAND_KEY|B_SHIFT_KEY));
    messages->AddItem(new BMenuItem("Forward",new BMessage(kForward),'F'));
    messages->AddItem(new BMenuItem("View formatted conversation",new BMessage(kFormatted),'P'));
    messages->AddSeparatorItem();
    messages->AddItem(new BMenuItem("Archive",new BMessage(kArchive),'E'));
    messages->AddItem(new BMenuItem("Move to Trash",new BMessage(kTrash)));
    messages->AddItem(new BMenuItem("Move to Inbox",new BMessage(kInbox)));
    messages->AddSeparatorItem();
    messages->AddItem(new BMenuItem("Mark as read",new BMessage(kRead),'K'));
    messages->AddItem(new BMenuItem("Mark as unread",new BMessage(kUnread),'U'));
    messages->AddItem(new BMenuItem("Toggle star",new BMessage(kStar),'L'));
    messages->AddItem(new BMenuItem("Add tag…",new BMessage(kTag),'T'));
    messages->AddItem(new BMenuItem("Remove tag…",new BMessage(kRemoveTag)));
    messages->AddSeparatorItem();
    messages->AddItem(new BMenuItem("Snooze…",new BMessage(kSnooze),'H'));
    messages->AddItem(new BMenuItem("Snooze for one hour",new BMessage(kQuickSnooze)));
    messages->AddItem(new BMenuItem("End snooze",new BMessage(kWake)));
    menuBar->AddItem(messages);
    auto* organize=new BMenu("Organize");
    organize->AddItem(new BMenuItem("Save view as smart folder…",new BMessage(kSaveSmart)));
    organize->AddItem(new BMenuItem("New rule…",new BMessage(kSaveFilter)));
    organize->AddSeparatorItem();
    organize->AddItem(new BMenuItem("Rules…",new BMessage(kManageRules)));
    organize->AddItem(new BMenuItem("Smart folders…",new BMessage(kManageSmart)));
    organize->AddItem(new BMenuItem("Tags…",new BMessage(kManageTags)));
    menuBar->AddItem(organize);

    fSidebar=new BListView("mailboxes"); fSidebar->SetSelectionMessage(new BMessage(kView));
    auto* sidebarScroll=new BScrollView("mailbox scroll",fSidebar,0,false,true,B_NO_BORDER);
    sidebarScroll->SetExplicitMinSize(BSize(150,200));
    fConversations=new BListView("conversations",B_MULTIPLE_SELECTION_LIST);
    fConversations->SetSelectionMessage(new BMessage(kSelect));
    fConversations->SetInvocationMessage(new BMessage(kOpenDraft));
    auto* conversationScroll=new BScrollView("conversation scroll",fConversations,0,false,true,B_NO_BORDER);
    conversationScroll->SetExplicitMinSize(BSize(260,200));
    fSearch=new BTextControl("search","Search:","",new BMessage(kSearch));
    fSearch->SetModificationMessage(new BMessage(kSearch));
    fReader=new EmojiTextView("message reader"); fReader->MakeEditable(false);
    fReader->SetWordWrap(true); fReader->SetInsets(16,12,16,12);
    auto* readerScroll=new BScrollView("reader scroll",fReader,0,false,true,B_NO_BORDER);
    readerScroll->SetExplicitMinSize(BSize(300,200));
    fTitle=new EmojiStringView("subject","Welcome to Mail"); fTitle->SetFont(be_bold_font);
    fTitle->SetExplicitMinSize(BSize(50,B_SIZE_UNSET));
    fTitle->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED,B_SIZE_UNSET));
    fAttachments=new AttachmentListView("attachments",[this](int32 index)->const Attachment* {
        return index>=0 && static_cast<std::size_t>(index)<fFiles.size()?&fFiles[index]:nullptr;
    });
    fAttachments->SetInvocationMessage(new BMessage(kSaveAttachment)); fAttachments->SetSelectionMessage(new BMessage(kAttachmentSelected));
    auto* attachmentScroll=new BScrollView("attachments scroll",fAttachments,0,false,true);
    attachmentScroll->SetExplicitMinSize(BSize(100,55)); attachmentScroll->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED,85));
    auto* attachmentTitle=new BStringView("attachment title","Attachments — drag to copy");
    attachmentTitle->SetExplicitMinSize(BSize(80,B_SIZE_UNSET));
    attachmentTitle->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED,B_SIZE_UNSET));
    fAttachmentPreview=new BButton("preview attachment","Preview",new BMessage(kPreviewAttachment)); fAttachmentPreview->SetEnabled(false);
    fAttachmentSave=new BButton("save attachment","Save…",new BMessage(kSaveAttachment)); fAttachmentSave->SetEnabled(false);
    auto* reader=new BGroupView(B_VERTICAL,0);
    fFormattedView=new BButton("formatted conversation","Formatted view",new BMessage(kFormatted)); fFormattedView->SetEnabled(false);
    BLayoutBuilder::Group<>(reader)
        .AddGroup(B_HORIZONTAL).SetInsets(16,12,16,12).Add(fTitle,1).Add(fFormattedView).End()
        .Add(readerScroll,1)
        .AddGroup(B_VERTICAL).SetInsets(12,8,12,12)
            .AddGroup(B_HORIZONTAL).Add(attachmentTitle,1).Add(fAttachmentPreview).Add(fAttachmentSave).End()
            .Add(attachmentScroll).End();
    auto* split=new BSplitView(B_HORIZONTAL,0);
    split->AddChild(sidebarScroll,0.16f); split->AddChild(conversationScroll,0.32f); split->AddChild(reader,0.52f);
    split->SetCollapsible(false);
    fStatus=new BStringView("status",""); fStatus->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED,B_SIZE_UNSET));
    fStatus->SetExplicitMinSize(BSize(50,B_SIZE_UNSET));
    fOutboxButton=new BButton("outbox","Outbox (0)",new BMessage(kOutbox));
    fMoveButton=new BButton("folder moves","Folder moves (0)",new BMessage(kFolderMoves));
    fReminderButton=new BButton("reminders","Reminders (0)",new BMessage(kReminders));
    fCheckMail=new BButton("check mail","Check mail",new BMessage(kCheckMail));
    BLayoutBuilder::Group<>(this,B_VERTICAL,0)
        .Add(menuBar)
        .AddGroup(B_HORIZONTAL).SetInsets(12,8,12,8)
            .Add(fCheckMail)
            .Add(new BButton("New message",new BMessage(kCompose)))
            .Add(new BButton("Archive",new BMessage(kArchive)))
            .Add(new BButton("Snooze…",new BMessage(kSnooze)))
            .AddGlue().Add(fSearch).End()
        .Add(split,1)
        .AddGroup(B_HORIZONTAL).SetInsets(12,6,12,6).Add(fStatus,1).Add(fMoveButton).Add(fReminderButton).Add(fOutboxButton).End();
    BMessage tick(kTick);
    fTimer=std::make_unique<BMessageRunner>(BMessenger(this),&tick,30*1000000);
    BMessage draftTick(kDraftTick); fDraftTimer=std::make_unique<BMessageRunner>(BMessenger(this),&draftTick,5*1000000);
    BuildSidebar(); fSidebar->Select(0); Refresh();
    PostMessage(kPollSnoozes);
    PostMessage(kCheckMail);
}
MailWindow::~MailWindow()
{
    fCancel=true;
    fCancelSync=true;
    fCancelSend=true;
    if (fImportWorker.joinable()) fImportWorker.join();
    if (fSyncWorker.joinable()) fSyncWorker.join();
    if (fSendWorker.joinable()) fSendWorker.join();
    clear(fSidebar); clear(fConversations); clear(fAttachments);
}
bool MailWindow::QuitRequested() { be_app->PostMessage(B_QUIT_REQUESTED); return true; }
void MailWindow::Error(const std::string& message) { (new BAlert("Mail",message.c_str(),"OK",nullptr,nullptr,B_WIDTH_AS_USUAL,B_WARNING_ALERT))->Go(); }

void MailWindow::BuildSidebar()
{
    auto oldIndex=fSidebar->CurrentSelection();
    std::string selected=oldIndex>=0 && static_cast<std::size_t>(oldIndex)<fViewKeys.size()?fViewKeys[oldIndex]:"";
    clear(fSidebar); fViews.clear(); fViewKeys.clear();
    int32 selection=-1;
    auto add=[&](const std::string& label,Query q,std::string key="") {
        if(key.empty()) key="builtin:"+label;
        if (key==selected) selection=fSidebar->CountItems();
        fSidebar->AddItem(new MailboxItem(label.c_str())); fViews.push_back(std::move(q)); fViewKeys.push_back(std::move(key));
    };
    Query q; add("All inboxes",q);
    q.folder=""; q.unread=true; add("Unread",q);
    q.unread=false; q.starred=true; add("Starred",q);
    q.starred=false; q.snoozedOnly=true; add("Snoozed",q);
    q.snoozedOnly=false; q.attachments=true; add("Attachments",q);
    q.attachments=false; q.folder="sent"; add("Sent",q);
    q.folder="drafts"; add("Drafts",q);
    q.folder="archive"; add("Archive",q);
    q.folder="trash"; q.includeSnoozed=true; add("Trash",q);
    for (const auto& a : fStore->accounts()) {
        Query account; account.account=a.id; add(a.name,account,"account:"+std::to_string(a.id));
        account.folder=""; add("    All mail",account,"all:"+std::to_string(a.id));
        account.snoozedOnly=true; add("    Snoozed",account,"snoozed:"+std::to_string(a.id));
        for (const auto& folder : fStore->remoteFolders(a.id)) {
            Query view; view.account=a.id; view.folder=folder.localName(); view.includeSnoozed=true;
            add("    "+a.name+" / "+folder.name,view,"folder:"+std::to_string(a.id)+":"+folder.id);
        }
    }
    for (const auto& folder : fStore->smartFolders()) add("◆ "+folder.name,folder.query,"smart:"+std::to_string(folder.id));
    for (const auto& tag : fStore->tags()) { Query tagged; tagged.folder=""; tagged.tag=tag; add("# "+tag,tagged,"tag:"+tag); }
    if (selection<0) selection=0;
    fQuery=fViews[selection]; fSearch->SetText(fQuery.text.c_str()); fSidebar->Select(selection);
}
void MailWindow::OpenOrganization(int kind,bool create)
{
    auto& messenger=fOrganization[kind];
    if (messenger.IsValid()) {
        BMessage request(create?kOrganizationNew:kOrganizationActivate);
        if (create) {
            request.AddInt64("account",fQuery.account); request.AddString("folder",fQuery.folder.c_str()); request.AddString("search",fQuery.text.c_str()); request.AddString("tag",fQuery.tag.c_str());
            request.AddBool("unread",fQuery.unread); request.AddBool("starred",fQuery.starred); request.AddBool("attachments",fQuery.attachments);
            request.AddBool("snoozed",fQuery.snoozedOnly); request.AddBool("include snoozed",fQuery.includeSnoozed);
        }
        messenger.SendMessage(&request); return;
    }
    auto* window=new OrganizationWindow(this,fStore,static_cast<OrganizationKind>(kind),fQuery,create);
    messenger=BMessenger(window); window->Show();
}
void MailWindow::RefreshOrganization()
{
    BMessage refresh(kOrganizationReload); refresh.AddBool("automatic",true);
    for (auto& item:fOrganization) if(item.second.IsValid()) item.second.SendMessage(&refresh);
}
void MailWindow::Refresh()
{
    RefreshOutbox();
    RefreshReminders();
    std::set<Id> selected;
    for (int32 i=0,index;(index=fConversations->CurrentSelection(i))>=0;++i)
        selected.insert(static_cast<ConversationItem*>(fConversations->ItemAt(index))->conversation.id);
    auto rows=fStore->conversations(fQuery,now());
    if (fQuery.folder=="drafts" && !fQuery.unread && !fQuery.starred && fQuery.tag.empty() && !fQuery.snoozedOnly) for (const auto& d:fStore->drafts(fQuery.account)) {
        if (fQuery.attachments && d.attachments.empty()) continue;
        if (!fQuery.text.empty() && (d.subject+" "+d.to+" "+d.text).find(fQuery.text)==std::string::npos) continue;
        rows.push_back({-d.id,d.account,d.subject.empty()?"(No subject)":d.subject,d.to.empty()?"Draft":d.to,d.text,d.modified,1,0,false,!d.attachments.empty()});
    }
    clear(fConversations);
    for (const auto& row : rows) {
        fConversations->AddItem(new ConversationItem(row));
        if (selected.count(row.id)) fConversations->Select(fConversations->CountItems()-1,true);
    }
    if (fConversations->CurrentSelection()<0) {
        fReader->SetText(rows.empty()?"No messages in this view.\n\nUse File → Import mail to open saved email messages.":"Select a conversation to read it.");
        fTitle->SetText("Mail"); clear(fAttachments); fFiles.clear();
        fFormattedView->SetEnabled(false);
    }
    std::string status=std::to_string(rows.size())+(rows.size()==1?" conversation":" conversations");
    if (fImporting) status+=" · Importing mail…";
    if (!fSyncing) fStatus->SetText(fSyncError.empty()?status.c_str():fSyncError.c_str());
}
void MailWindow::CheckMail(bool draftsOnly)
{
    if (fAccountsOpen) return;
    SendQueued();
    if (fSyncing) { if (!draftsOnly) { fCancelSync=true; fStatus->SetText("Stopping mail check…"); } return; }
    if (fSyncWorker.joinable()) fSyncWorker.join();
    auto accounts=fStore->accounts();
    accounts.erase(std::remove_if(accounts.begin(),accounts.end(),[&](const Account& a){return a.provider==Provider::Local || (draftsOnly && a.provider!=Provider::Gmail);}),accounts.end());
    if (!draftsOnly) fLastSync=now();
    fDraftDue=0;
    if (draftsOnly && accounts.empty()) return;
    if (accounts.empty()) { fStatus->SetText("Add an account from File → Accounts to receive mail."); return; }
    fSyncError.clear(); fStatus->SetToolTip("");
    fCancelSync=false; fSyncing=true; fCheckMail->SetLabel("Stop"); fStatus->SetText("Checking mail…");
    auto store=fStore; BMessenger target(this);
    fSyncWorker=std::thread([this,store,target,accounts,draftsOnly] {
        int32 downloaded=0; std::string errors;
        for (const auto& account : accounts) {
            if (fCancelSync) break;
            try {
                auto progress=[&](const std::string& text) {
                    BMessage progress(kSyncProgress); progress.AddString("text",text.c_str()); target.SendMessage(&progress);
                };
                SyncReport report;
                if (account.provider==Provider::Gmail) {
                    auto api=ConnectGmail(account,fCancelSync,fGoogle.endpoints);
                    report=draftsOnly?syncGmailDrafts(*store,account,*api,fCancelSync):syncGmailAccount(*store,account,*api,fCancelSync,progress);
                } else report=syncAccount(*store,account,LoadCredentials(account).incoming,fCancelSync,progress);
                downloaded+=static_cast<int32>(report.downloaded);
                for (const auto& error : report.errors) errors+=account.name+": "+error+'\n';
            } catch (const std::exception& error) { if (!fCancelSync) errors+=account.name+": "+error.what()+'\n'; }
        }
        BMessage done(kSyncDone); done.AddInt32("downloaded",downloaded); done.AddString("errors",errors.c_str());
        done.AddBool("cancelled",fCancelSync); target.SendMessage(&done);
    });
}
void MailWindow::ShowConversation()
{
    auto* item=static_cast<ConversationItem*>(fConversations->ItemAt(fConversations->CurrentSelection()));
    fFormattedView->SetEnabled(item && item->conversation.id>0);
    if (!item) return;
    if (item->conversation.id<0) {
        auto d=fStore->draft(-item->conversation.id); fTitle->SetText(d.subject.empty()?"Draft":d.subject.c_str());
        fReader->SetText(("Draft — double-click to continue editing\nTo: "+d.to+"\nCc: "+d.cc+"\nBcc: "+d.bcc+"\n\n"+d.text).c_str());
        clear(fAttachments); fFiles=d.attachments;
        for (const auto& a:fFiles) fAttachments->AddItem(new AttachmentItem(a.name));
        return;
    }
    auto messages=fStore->thread(item->conversation.id);
    std::string text; clear(fAttachments); fFiles.clear();
    for (const auto& m : messages) {
        text+=m.sender+"\nTo: "+m.to;
        if (!m.cc.empty()) text+="\nCc: "+m.cc;
        text+='\n'+date(m.received);
        if(m.snoozedUntil>now()) text+="\nSnoozed until: "+describeSnoozeTime(m.snoozedUntil);
        if (!m.tags.empty()) { text+="\nTags: "; for (const auto& t : m.tags) text+=t+"  "; }
        text+="\n\n"+m.text+"\n\n\n";
        for (const auto& a : m.attachments) {
            std::string label=a.name+"  ("+std::to_string(a.data.size())+" bytes)";
            fAttachments->AddItem(new AttachmentItem(label)); fFiles.push_back(a);
        }
    }
    // Read state is changed explicitly with the Message menu. Merely inspecting
    // a multi-selection must not silently mark hidden messages read.
    fTitle->SetText(item->conversation.subject.empty()?"(No subject)":item->conversation.subject.c_str());
    fReader->SetText(text.c_str()); fReader->ScrollTo(0,0);
}
void MailWindow::OpenFormatted(Id thread,Id message)
{
    if(!thread) {
        auto* item=static_cast<ConversationItem*>(fConversations->ItemAt(fConversations->CurrentSelection()));
        if(!item || item->conversation.id<=0) return;
        thread=item->conversation.id;
    }
    auto messages=fStore->thread(thread); if(messages.empty()) throw std::runtime_error("This conversation is no longer available.");
    auto found=message?std::find_if(messages.begin(),messages.end(),[&](const auto& m){return m.id==message;}):std::prev(messages.end());
    if(found==messages.end()) throw std::runtime_error("This message is no longer in the conversation.");
    const auto& m=*found; PreviewSnapshot snapshot; snapshot.received.emplace(); auto& received=*snapshot.received;
    received.id=m.id; received.from=m.sender; received.to=m.to; received.cc=m.cc; received.subject=m.subject;
    received.date=date(m.received); received.text=m.text; received.html=m.html; received.files=m.attachments;
    for(const auto& entry:messages) received.thread.push_back({entry.id,entry.sender,date(entry.received)});
    for(const auto& [key,id]:fFormattedThreads) if(id==thread) {snapshot.key=key;break;}
    if(snapshot.key.empty()) snapshot.key=newMessageId();
    LaunchPreview(snapshot,BMessenger(this)); fFormattedThreads[snapshot.key]=thread;
}
std::vector<Id> MailWindow::SelectedMessages()
{
    std::vector<Id> ids;
    for (int32 i=0,index;(index=fConversations->CurrentSelection(i))>=0;++i) {
        auto* item=static_cast<ConversationItem*>(fConversations->ItemAt(index));
        for (const auto& m : fStore->thread(item->conversation.id)) ids.push_back(m.id);
    }
    return ids;
}
void MailWindow::Apply(uint32 action)
{
    if(action==kQuickSnooze || action==kWake) {
        fStore->snooze(SelectedMessages(),action==kWake?0:snoozePreset(now(),SnoozePreset::Hour)); Refresh(); ShowConversation(); return;
    }
    for (Id id : SelectedMessages()) {
        switch (action) {
            case kArchive: fStore->move(id,"archive"); break;
            case kTrash: fStore->move(id,"trash"); break;
            case kInbox: fStore->move(id,"inbox"); break;
            case kRead: fStore->setRead(id,true); break;
            case kUnread: fStore->setRead(id,false); break;
            case kStar: fStore->setStarred(id,!fStore->message(id).starred); break;
        }
    }
    Refresh(); ShowConversation();
}
void MailWindow::RefreshReminders(bool notify)
{
    auto reminders=fStore->reminders(now()); std::set<Id> conversations,pending; std::vector<Id> ids;
    for(const auto& reminder:reminders) {
        conversations.insert(reminder.thread);
        if(!reminder.notified) { pending.insert(reminder.thread); ids.push_back(reminder.id); }
    }
    fReminderButton->SetLabel(("Reminders ("+std::to_string(conversations.size())+")").c_str());
    fReminderButton->SetEnabled(!conversations.empty());
    if(notify && !ids.empty() && NotifySnoozes(pending.size(),fDatabase)) fStore->markRemindersNotified(ids);
    if(fReminders.IsValid()) fReminders.SendMessage(kReminderReload);
}
void MailWindow::OpenSnooze(const std::vector<Id>& ids)
{
    if(ids.empty()) { fStatus->SetText("Select a conversation to snooze."); return; }
    if(fAccountsOpen) { fStatus->SetText("Close Accounts before choosing a snooze time."); return; }
    if(fSnooze.IsValid()) { fSnooze.SendMessage(kSnoozeActivate); return; }
    auto* window=new SnoozeWindow(this,fStore,ids,fStore->message(ids[0]).snoozedUntil); fSnooze=BMessenger(window); window->Show();
}
void MailWindow::OpenReminder(Id id,bool again)
{
    auto reminders=fStore->reminders(now());
    auto found=std::find_if(reminders.begin(),reminders.end(),[&](const SnoozeReminder& reminder){return reminder.id==id;});
    if(found==reminders.end()) { RefreshReminders(); return; }
    auto message=fStore->message(found->message); // Resolve current thread after any merge.
    if(again) {
        std::vector<Id> ids; for(const auto& member:fStore->thread(message.thread)) ids.push_back(member.id);
        OpenSnooze(ids); return;
    }
    auto key="all:"+std::to_string(message.account); auto view=std::find(fViewKeys.begin(),fViewKeys.end(),key);
    if(view==fViewKeys.end()) return;
    auto index=view-fViewKeys.begin(); fSidebar->Select(index); fQuery=fViews[index]; fSearch->SetText(""); Refresh();
    for(int32 i=0;i<fConversations->CountItems();++i) if(static_cast<ConversationItem*>(fConversations->ItemAt(i))->conversation.id==message.thread) { fConversations->Select(i); ShowConversation(); break; }
    Activate();
}
void MailWindow::Import(BMessage* message)
{
    if (fImporting) { Error("Mail is still importing the previous files."); return; }
    std::vector<std::string> paths; entry_ref ref;
    for (int32 i=0;message->FindRef("refs",i,&ref)==B_OK;++i) {
        BEntry entry(&ref,true); BPath path;
        if (entry.GetPath(&path)==B_OK && entry.IsFile()) paths.emplace_back(path.Path());
    }
    if (paths.empty()) return;
    if (fImportWorker.joinable()) fImportWorker.join();
    fImporting=true; fStatus->SetText("Importing mail…");
    auto store=fStore; auto account=fLocalAccount; BMessenger target(this);
    fImportWorker=std::thread([this,store,account,target,paths] {
        int32 imported=0,failed=0; std::string firstError;
        for (const auto& path : paths) {
            if (fCancel) break;
            try {
                std::ifstream file(path,std::ios::binary|std::ios::ate);
                auto size=file.tellg();
                if (!file || size<0 || size>32*1024*1024) throw std::runtime_error("Could not read the file, or it exceeds 32 MiB.");
                std::string raw(static_cast<std::size_t>(size),'\0'); file.seekg(0);
                if (!file.read(raw.data(),size)) throw std::runtime_error("Could not read the complete message.");
                auto m=parseMessage(raw); m.account=account; m.remoteKey="file:"+path;
                if (!m.received) m.received=now();
                store->ingest(m); ++imported;
            } catch (const std::exception& error) { ++failed; if (firstError.empty()) firstError=path+"\n"+error.what(); }
        }
        BMessage done(kImported); done.AddInt32("count",imported); done.AddInt32("failed",failed);
        done.AddString("error",firstError.c_str()); target.SendMessage(&done);
    });
}
void MailWindow::Compose(uint32 action,Id id)
{
    auto* selected=static_cast<ConversationItem*>(fConversations->ItemAt(fConversations->CurrentSelection()));
    if (action==kOpenDraft) {
        if (!id && selected && selected->conversation.id<0) id=-selected->conversation.id;
        if (!id) return;
        auto found=fComposers.find(id); if (found!=fComposers.end() && found->second.IsValid()) { found->second.SendMessage(kComposeActivate); return; }
    }
    Draft d;
    if (id) d=fStore->draft(id);
    else {
        auto accounts=fStore->accounts(); Account account=accounts.front();
        for (const auto& a:accounts) if (a.provider!=Provider::Local) { account=a; break; }
        if (action==kReply || action==kReplyAll || action==kForward) {
            if (!selected || selected->conversation.id<=0) { Error("Select a received conversation first."); return; }
            auto thread=fStore->thread(selected->conversation.id); if (thread.empty()) return;
            auto m=thread.back();
            for (const auto& a:accounts) if (a.id==m.account && a.provider!=Provider::Local) account=a;
            d=action==kForward?forwardDraft(m,account):replyDraft(m,account,action==kReplyAll);
        } else { d.account=account.id; d.messageId=newMessageId(); }
        d.modified=now(); d.id=fStore->saveDraft(d);
    }
    auto* window=new ComposerWindow(this,fStore,d); fComposers[d.id]=BMessenger(window); window->Show(); Refresh();
    fDraftDue=now()+10;
}
void MailWindow::SendQueued()
{
    if (fSending || fAccountsOpen) return;
    if (!fStore->readyOutgoing()) return;
    if (fSendWorker.joinable()) fSendWorker.join();
    fSending=true; fCancelSend=false; fStatus->SetText("Sending mail…"); auto store=fStore; BMessenger target(this);
    fSendWorker=std::thread([this,store,target] {
        std::string status; bool sent=false;
        try {
            auto report=deliverQueued(*store,[](const Account& a){return LoadCredentials(a).outgoing;},fCancelSend,
                [&](const Account& a){return ConnectGmail(a,fCancelSend,fGoogle.endpoints);});
            sent=report.sent>0; status=std::to_string(report.sent)+" messages sent.";
            if (report.failed || report.uncertain) status+=" Open Outbox to review delivery problems.";
        } catch (const std::exception& error) { status=error.what(); }
        BMessage done(kSendDone); done.AddBool("sent",sent); done.AddString("status",status.c_str()); target.SendMessage(&done);
    });
}
void MailWindow::RefreshOutbox()
{
    auto moves=fStore->moveReviews();
    fMoveButton->SetLabel(("Folder moves ("+std::to_string(moves.size())+")").c_str());
    fMoveButton->SetToolTip("Review queued or interrupted IMAP folder moves.");
    auto summary=fStore->outboxSummary();
    std::string label="Outbox ("+std::to_string(summary.total());
    if (summary.attention()) label+=" · "+std::to_string(summary.attention())+(summary.attention()==1?" needs attention":" need attention");
    label+=")";
    auto copies=fStore->sentCopies();
    if (!copies.empty()) label+=" · "+std::to_string(copies.size())+(copies.size()==1?" Sent copy":" Sent copies");
    fOutboxButton->SetLabel(label.c_str());
    std::string details=std::to_string(summary.queued)+" waiting to send\n"+std::to_string(summary.sending)+" sending\n"
        +std::to_string(summary.failed)+" failed\n"+std::to_string(summary.uncertain)+" with uncertain delivery";
    if (summary.attention()) details+="\nOpen Outbox to review. These messages will not be retried automatically.";
    if (!copies.empty()) details+="\n"+std::to_string(copies.size())+" delivered messages await server Sent copies. Open Outbox to review.";
    fOutboxButton->SetToolTip(details.c_str());
}
void MailWindow::MessageReceived(BMessage* message)
{
    try {
        switch (message->what) {
            case kCompose: case kReply: case kReplyAll: case kForward: Compose(message->what); break;
            case kOpenDraft: {
                int64 id=0; message->FindInt64("draft",&id);
                auto* item=static_cast<ConversationItem*>(fConversations->ItemAt(fConversations->CurrentSelection()));
                if(!id && item && item->conversation.id>0) OpenFormatted(item->conversation.id);
                else Compose(kOpenDraft,id);
                break;
            }
            case kFormatted: OpenFormatted(); break;
            case kPreviewRefresh: case kPreviewSelect: {
                auto found=fFormattedThreads.find(message->GetString("preview-key",""));
                if(found!=fFormattedThreads.end()) OpenFormatted(found->second,message->GetInt64("message",0));
                break;
            }
            case kDraftChanged: Refresh(); fDraftDue=now()+10; break;
            case kDraftReview:
                if (fDraftReview.IsValid()) fDraftReview.SendMessage(kDraftReview);
                else { auto* window=new DraftWindow(this,fStore); fDraftReview=BMessenger(window); window->Show(); }
                break;
            case kDraftReviewChanged: Refresh(); fDraftDue=now(); break;
            case kCheckDrafts:
                fDraftDue=now(); if (!fSyncing && !fAccountsOpen) CheckMail(true);
                break;
            case kDraftTick:
                if (fDraftDue && now()>=fDraftDue && !fSyncing && !fAccountsOpen) CheckMail(true);
                break;
            case kOutbox:
                if (fOutbox.IsValid()) fOutbox.SendMessage(kOutboxActivate);
                else { auto* window=new OutboxWindow(this,fStore); fOutbox=BMessenger(window); window->Show(); }
                break;
            case kFolderMoves:
                if (fMoves.IsValid()) fMoves.SendMessage(kFolderMoves);
                else { auto* window=new MoveWindow(this,fStore); fMoves=BMessenger(window); window->Show(); }
                break;
            case kMovesChanged: BuildSidebar(); Refresh(); ShowConversation(); break;
            case kOutboxChanged: Refresh(); break;
            case kSendQueued: Refresh(); SendQueued(); break;
            case kSendDone: {
                fSending=false; Refresh(); const char* status=""; message->FindString("status",&status); fStatus->SetText(status);
                // A composer may have queued another message while the previous worker was finishing.
                bool sent=false; message->FindBool("sent",&sent);
                if (sent && !fStore->sentCopies().empty()) fNeedsSentCheck=true;
                SendQueued();
                if (fNeedsSentCheck && !fSyncing && !fSending) { fNeedsSentCheck=false; CheckMail(); }
                break;
            }
            case kCheckSentCopies: case kCheckMoves:
                if (fSyncing) fNeedsSentCheck=true;
                else CheckMail();
                break;
            case kOrganizationChanged: BuildSidebar(); Refresh(); ShowConversation(); RefreshOrganization(); break;
            case kSnooze: OpenSnooze(SelectedMessages()); break;
            case kSnoozeChanged: Refresh(); ShowConversation(); break;
            case kPollSnoozes: RefreshReminders(true); break;
            case kReminders:
                if(fReminders.IsValid()) fReminders.SendMessage(kReminders);
                else { auto* window=new ReminderWindow(this,fStore); fReminders=BMessenger(window); window->Show(); }
                break;
            case kReminderOpen: case kReminderAgain: {
                int64 id=0; message->FindInt64("reminder",&id); OpenReminder(id,message->what==kReminderAgain); break;
            }
            case kManageRules: OpenOrganization(static_cast<int>(OrganizationKind::Rules)); break;
            case kManageSmart: OpenOrganization(static_cast<int>(OrganizationKind::SmartFolders)); break;
            case kManageTags: OpenOrganization(static_cast<int>(OrganizationKind::Tags)); break;
            case kAccounts:
                if(fSnooze.IsValid()) fStatus->SetText("Finish choosing a snooze time before changing accounts.");
                else if (std::any_of(fOrganization.begin(),fOrganization.end(),[](const auto& item){return item.second.IsValid();}))
                    fStatus->SetText("Close the organization editors before changing accounts.");
                else if (fSyncing || fSending) fStatus->SetText("Wait for sending to finish and stop the mail check before editing accounts.");
                else if (std::any_of(fComposers.begin(),fComposers.end(),[](const auto& item){return item.second.IsValid();}))
                    fStatus->SetText("Close your message drafts before editing accounts. They will be saved in Drafts.");
                else if (!fAccountsOpen) { (new AccountWindow(this,fStore,fGoogle))->Show(); fAccountsOpen=true; }
                break;
            case kAccountsChanged: BuildSidebar(); Refresh(); break;
            case kAccountsClosed: fAccountsOpen=false; CheckMail(); break;
            case kCheckMail: CheckMail(); break;
            case kSyncProgress: {
                const char* text=nullptr; if (message->FindString("text",&text)==B_OK) fStatus->SetText(text); break;
            }
            case kSyncDone: {
                fSyncing=false; fCheckMail->SetLabel("Check mail"); BuildSidebar(); Refresh(); RefreshOrganization();
                int32 downloaded=0; bool cancelled=false; const char* errors="";
                message->FindInt32("downloaded",&downloaded); message->FindBool("cancelled",&cancelled); message->FindString("errors",&errors);
                std::string status=cancelled?"Mail check stopped.":"Mail checked. "+std::to_string(downloaded)+" new messages.";
                if (errors && *errors) { status=errors; status=status.substr(0,status.find('\n')); fSyncError=status; }
                fStatus->SetText(status.c_str()); fStatus->SetToolTip(errors?errors:"");
                SendQueued();
                if (fNeedsSentCheck && !fSending) {
                    fNeedsSentCheck=false; if (!cancelled) CheckMail();
                }
                break;
            }
            case kImport:
                if (!fImportPanel) { BMessenger target(this); fImportPanel=std::make_unique<BFilePanel>(B_OPEN_PANEL,&target,nullptr,B_FILE_NODE,true); }
                fImportPanel->Show(); break;
            case B_REFS_RECEIVED: case B_SIMPLE_DATA: Import(message); break;
            case kImported: {
                fImporting=false; BuildSidebar(); Refresh();
                int32 count=0,failed=0; message->FindInt32("count",&count); message->FindInt32("failed",&failed);
                auto status="Imported "+std::to_string(count)+" messages";
                fStatus->SetText(status.c_str());
                if (failed) Error(std::to_string(failed)+" files could not be imported.\n\n"+message->FindString("error"));
                break;
            }
            case kView: {
                auto index=fSidebar->CurrentSelection();
                if (index>=0 && static_cast<std::size_t>(index)<fViews.size()) {
                    fQuery=fViews[index]; fSearch->SetText(fQuery.text.c_str()); Refresh();
                }
                break;
            }
            case kSelect: ShowConversation(); break;
            case kSearch: fQuery.text=fSearch->Text(); Refresh(); break;
            case kTick:
                RefreshReminders(true);
                if (!fImporting) Refresh();
                if (!fSyncing && now()-fLastSync>=300) CheckMail();
                break;
            case kArchive: case kTrash: case kInbox: case kRead: case kUnread:
            case kStar: case kQuickSnooze: case kWake: Apply(message->what); break;
            case kTag: case kRemoveTag: {
                const char* value=nullptr;
                if (message->FindString("value",&value)==B_OK) {
                    for (Id id : SelectedMessages()) fStore->tag(id,value,message->what==kTag);
                    BuildSidebar(); Refresh(); ShowConversation(); RefreshOrganization();
                } else if (!SelectedMessages().empty()) (new EditWindow(this,message->what,message->what==kTag?"Add tag":"Remove tag","Tag:"))->Show();
                break;
            }
            case kSaveSmart: {
                const char* value=nullptr;
                if (message->FindString("value",&value)==B_OK) {
                    fStore->saveSmartFolder({0,value,fQuery}); BuildSidebar();
                } else OpenOrganization(static_cast<int>(OrganizationKind::SmartFolders),true);
                break;
            }
            case kSaveFilter: {
                const char* value=nullptr;
                if (message->FindString("value",&value)==B_OK) {
                    Filter f; f.name=value; f.account=fQuery.account; f.senderContains=message->FindString("sender");
                    f.subjectContains=message->FindString("subject"); f.addTag=message->FindString("tag");
                    bool archive=false; message->FindBool("archive",&archive); if (archive) f.moveTo="archive";
                    message->FindBool("read",&f.markRead); fStore->saveFilter(f);
                    fStatus->SetText("Filter saved. It will apply to new messages.");
                } else OpenOrganization(static_cast<int>(OrganizationKind::Rules),true);
                break;
            }
            case kAttachmentSelected: {
                auto index=fAttachments->CurrentSelection(); bool selected=index>=0 && static_cast<std::size_t>(index)<fFiles.size();
                fAttachmentSave->SetEnabled(selected); fAttachmentPreview->SetEnabled(selected && fFiles[index].mediaType.rfind("image/",0)==0); break;
            }
            case kPreviewAttachment: {
                auto index=fAttachments->CurrentSelection();
                if (index>=0 && static_cast<std::size_t>(index)<fFiles.size() && fFiles[index].mediaType.rfind("image/",0)==0)
                    (new AttachmentPreviewWindow(fFiles[index]))->Show();
                break;
            }
            case kAttachmentExported: {
                const char* text=nullptr; if (message->FindString("text",&text)==B_OK) {
                    fStatus->SetText(text); if (message->GetBool("failed",false)) Error(text);
                } break;
            }
            case kSaveAttachment: {
                auto index=fAttachments->CurrentSelection();
                if (index<0 || static_cast<std::size_t>(index)>=fFiles.size()) break;
                fSaving=fFiles[index]; BMessenger target(this);
                BMessage save(kAttachmentSaved);
                fSavePanel=std::make_unique<BFilePanel>(B_SAVE_PANEL,&target,nullptr,B_FILE_NODE,false,&save);
                fSavePanel->SetSaveText(fSaving.name.c_str()); fSavePanel->Show(); break;
            }
            case kAttachmentSaved: {
                entry_ref directory; const char* name=nullptr;
                if (message->FindRef("directory",&directory)!=B_OK || message->FindString("name",&name)!=B_OK) break;
                BPath path(&directory); path.Append(name);
                BFile file(path.Path(),B_WRITE_ONLY|B_CREATE_FILE|B_ERASE_FILE);
                if (file.InitCheck()!=B_OK || file.Write(fSaving.data.data(),fSaving.data.size())!=static_cast<ssize_t>(fSaving.data.size()) || file.Sync()!=B_OK)
                    throw std::runtime_error("Could not save the attachment. Check free disk space and folder permissions.");
                fStatus->SetText("Attachment saved."); break;
            }
            default: BWindow::MessageReceived(message); break;
        }
    } catch (const std::exception& error) { Error(error.what()); }
}
}
