#include "OrganizationWindow.h"
#include "Emoji.h"
#include <Alert.h>
#include <Button.h>
#include <CheckBox.h>
#include <GroupView.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <ScrollView.h>
#include <TextControl.h>
#include <TextView.h>
#include <algorithm>
#include <sstream>

namespace mail::ui {
namespace {
enum:uint32 { kSelect='ogsl',kSave='ogsv',kRemove='ogrm',kChanged='oged',kScope='ogsc',kUp='ogup',kDown='ogdn',kCheck='ogck' };
const char* title(OrganizationKind kind) { return kind==OrganizationKind::Rules?"Mail rules":(kind==OrganizationKind::SmartFolders?"Smart folders":"Tags"); }
void clear(BListView* list) { while(auto* item=list->RemoveItem(int32(0))) delete item; }
void clear(BMenu* menu) { while(auto* item=menu->RemoveItem(int32(0))) delete item; }
class Row : public BStringItem {
public:
    explicit Row(const std::string& text):BStringItem(text.c_str()) {}
    void DrawItem(BView* owner,BRect bounds,bool) override
    {
        owner->SetHighColor(ui_color(IsSelected()?B_LIST_SELECTED_BACKGROUND_COLOR:B_LIST_BACKGROUND_COLOR)); owner->FillRect(bounds);
        owner->SetLowColor(owner->HighColor()); owner->SetHighColor(ui_color(IsSelected()?B_LIST_SELECTED_ITEM_TEXT_COLOR:B_LIST_ITEM_TEXT_COLOR));
        BFont font; owner->GetFont(&font); DrawEmojiText(*owner,Text(),font,{bounds.left+5,bounds.top+BaselineOffset()},bounds.Width()-10);
    }
};
BTextControl* textField(const char* name,const char* label)
{
    auto* field=new BTextControl(name,label,"",nullptr); field->SetModificationMessage(new BMessage(kChanged));
    field->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED,B_SIZE_UNSET)); return field;
}
BCheckBox* checkBox(const char* name,const char* label) { return new BCheckBox(name,label,new BMessage(kChanged)); }
bool on(BCheckBox* box) { return box && box->Value()==B_CONTROL_ON; }
}
OrganizationWindow::OrganizationWindow(BWindow* owner,std::shared_ptr<Store> store,OrganizationKind kind,const Query& seed,bool create)
    :BWindow(BRect(120,80,960,680),title(kind),B_TITLED_WINDOW,B_AUTO_UPDATE_SIZE_LIMITS|B_ASYNCHRONOUS_CONTROLS|B_CLOSE_ON_ESCAPE),
      fStore(std::move(store)),fOwner(owner),fKind(kind),fSeed(seed)
{
    fList=new BListView("organization list"); fList->SetSelectionMessage(new BMessage(kSelect));
    auto* scroll=new BScrollView("organization list scroll",fList,0,false,true); scroll->SetExplicitMinSize(BSize(235,230));
    fName=textField("organization name","Name:");
    fScope=new BMenuField("organization account","Account:",new BPopUpMenu("Account")); BuildScope(seed.account);
    fStatus=new EmojiTextView("organization status"); fStatus->MakeEditable(false); fStatus->SetWordWrap(true);
    fStatus->SetViewUIColor(B_PANEL_BACKGROUND_COLOR); fStatus->SetLowUIColor(B_PANEL_BACKGROUND_COLOR);
    auto color=ui_color(B_PANEL_TEXT_COLOR); fStatus->SetFontAndColor(be_plain_font,B_FONT_ALL,&color);
    fStatus->SetExplicitMinSize(BSize(390,100)); fStatus->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED,100));
    auto* form=new BGroupView("organization form",B_VERTICAL);
    auto layout=BLayoutBuilder::Group<>(form).Add(fScope).Add(fName);
    if (kind!=OrganizationKind::Tags) {
        fTag=textField("organization tag",kind==OrganizationKind::Rules?"Add tag:":"Tag:");
        fFolder=new BMenuField("organization folder",kind==OrganizationKind::Rules?"Move to:":"Folder:",new BPopUpMenu("Folder"));
        fRead=checkBox("organization read",kind==OrganizationKind::Rules?"Mark as read":"Unread only");
        fStar=checkBox("organization star",kind==OrganizationKind::Rules?"Add star":"Starred only");
        if (kind==OrganizationKind::Rules) {
            fSender=textField("rule sender","From contains:"); fSubject=textField("rule subject","Subject contains:");
            fStop=checkBox("rule stop","Stop after this rule matches"); fEnabled=checkBox("rule enabled","Enable this rule");
            layout.Add(fSender).Add(fSubject).Add(fTag).Add(fFolder).AddGroup(B_HORIZONTAL).Add(fRead).Add(fStar).AddGlue().End().Add(fStop).Add(fEnabled);
        } else {
            fSearch=textField("smart search","Search words:");
            fAttachments=checkBox("smart attachments","Has attachments"); fSnoozed=checkBox("smart snoozed","Snoozed only");
            fIncludeSnoozed=checkBox("smart include snoozed","Include snoozed messages");
            layout.Add(fFolder).Add(fSearch).Add(fTag).AddGroup(B_HORIZONTAL).Add(fRead).Add(fStar).AddGlue().End()
                .Add(fAttachments).Add(fSnoozed).Add(fIncludeSnoozed);
        }
    }
    auto* statusScroll=new BScrollView("organization status scroll",fStatus,0,false,true,B_NO_BORDER);
    statusScroll->SetExplicitMinSize(BSize(390,110)); statusScroll->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED,110));
    layout.Add(statusScroll).AddGlue();
    fSave=new BButton("organization save",kind==OrganizationKind::Tags?"Rename":"Save",new BMessage(kSave));
    fRemove=new BButton("organization remove","Remove…",new BMessage(kRemove));
    auto* left=new BGroupView("organization navigation",B_VERTICAL); auto navigation=BLayoutBuilder::Group<>(left).Add(scroll);
    if (kind!=OrganizationKind::Tags) navigation.Add(new BButton("organization new","New",new BMessage(kOrganizationNew)));
    if (kind==OrganizationKind::Rules) {
        fUp=new BButton("rule up","Move up",new BMessage(kUp)); fDown=new BButton("rule down","Move down",new BMessage(kDown));
        navigation.AddGroup(B_HORIZONTAL).Add(fUp).Add(fDown).End();
    }
    auto root=BLayoutBuilder::Group<>(this,B_VERTICAL).SetInsets(B_USE_WINDOW_INSETS)
        .AddGroup(B_HORIZONTAL).Add(left).Add(form,2).End().AddGroup(B_HORIZONTAL);
    root.Add(new BButton("organization refresh","Refresh",new BMessage(kOrganizationReload))).Add(fRemove);
    if (kind==OrganizationKind::Tags) root.Add(new BButton("organization check","Check mail",new BMessage(kCheck)));
    root.AddGlue().Add(new BButton("organization close","Close",new BMessage(B_QUIT_REQUESTED))).Add(fSave);
    SetDefaultButton(fSave); CenterIn(owner->Frame());
    if (kind==OrganizationKind::Tags) fTagAccount=Scope();
    Reload(); if (create) New();
}
OrganizationWindow::~OrganizationWindow() { clear(fList); }
Id OrganizationWindow::Scope() const
{
    auto* marked=fScope->Menu()->FindMarked(); int64 account=0;
    if (marked && marked->Message()) marked->Message()->FindInt64("account",&account);
    return account;
}
std::string OrganizationWindow::Folder() const
{
    if (!fFolder) return {};
    auto* marked=fFolder->Menu()->FindMarked(); const char* folder="";
    if (marked && marked->Message()) marked->Message()->FindString("folder",&folder);
    return folder;
}
void OrganizationWindow::BuildScope(Id selected)
{
    auto* menu=fScope->Menu(); clear(menu); bool found=false;
    auto add=[&](Id id,const std::string& label) { auto* message=new BMessage(kScope); message->AddInt64("account",id);
        auto* item=new BMenuItem(label.c_str(),message); item->SetTarget(this); menu->AddItem(item); if (id==selected) { item->SetMarked(true); found=true; } };
    if (fKind!=OrganizationKind::Tags) add(0,fKind==OrganizationKind::SmartFolders?"Accounts in All inboxes":"All accounts");
    for (const auto& account:fStore->accounts()) add(account.id,account.name);
    if (!found && selected && fKind!=OrganizationKind::Tags) add(selected,"Removed account — choose another");
    if (!menu->FindMarked() && menu->CountItems()) menu->ItemAt(0)->SetMarked(true);
}
void OrganizationWindow::BuildFolders(const std::string& selected)
{
    auto* menu=fFolder->Menu(); clear(menu); bool found=false;
    std::vector<std::string> values;
    auto add=[&](const std::string& value,const std::string& label) {
        if (std::find(values.begin(),values.end(),value)!=values.end()) return;
        values.push_back(value);
        auto* message=new BMessage(kChanged); message->AddString("folder",value.c_str()); auto* item=new BMenuItem(label.c_str(),message); item->SetTarget(this); menu->AddItem(item);
        if (selected==value) { item->SetMarked(true); found=true; }
    };
    add("",fKind==OrganizationKind::Rules?"Keep current folder":"All mail except Trash and Junk");
    for (const auto& entry:std::vector<std::pair<std::string,std::string>>{{"inbox","Inbox"},{"archive","Archive"},{"sent","Sent"},{"drafts","Drafts"},{"trash","Trash"},{"junk","Junk"}}) add(entry.first,entry.second);
    if (Scope()) for (const auto& folder:fStore->remoteFolders(Scope())) add(folder.localName(),folder.name);
    if (!found) add(selected,"Unavailable folder: "+selected);
}
std::string OrganizationWindow::Signature() const
{
    std::ostringstream text; text<<Scope()<<';';
    for (auto* field:{fName,fSender,fSubject,fTag,fSearch}) if(field) { auto value=std::string(field->Text()); text<<value.size()<<':'<<value; }
    auto folder=Folder(); text<<folder.size()<<':'<<folder;
    for (auto* box:{fRead,fStar,fStop,fEnabled,fAttachments,fSnoozed,fIncludeSnoozed}) text<<on(box);
    return text.str();
}
bool OrganizationWindow::CanLeave()
{
    if (Signature()==fBaseline) return true;
    auto* alert=new BAlert("Unsaved changes","Discard the changes in this editor?","Cancel","Discard",nullptr,B_WIDTH_AS_USUAL,B_WARNING_ALERT);
    alert->SetShortcut(0,B_ESCAPE); alert->SetDefaultButton(alert->ButtonAt(0)); return alert->Go()==1;
}
bool OrganizationWindow::QuitRequested() { return CanLeave(); }
void OrganizationWindow::Reload()
{
    clear(fList); fSelection=-1;
    if (fKind==OrganizationKind::Rules) {
        fRules=fStore->filters();
        for (std::size_t i=0;i<fRules.size();++i) {
            const auto& rule=fRules[i]; fList->AddItem(new Row(std::to_string(i+1)+". "+(rule.enabled?"":"[Off] ")+rule.name));
            if (rule.id==fId) fSelection=i;
        }
    } else if (fKind==OrganizationKind::SmartFolders) {
        fSmart=fStore->smartFolders();
        for (std::size_t i=0;i<fSmart.size();++i) { fList->AddItem(new Row(fSmart[i].name)); if(fSmart[i].id==fId) fSelection=i; }
    } else {
        BuildScope(fTagAccount); fTagAccount=Scope(); fTags=fTagAccount?fStore->accountTags(fTagAccount):std::vector<TagInfo>{};
        for (std::size_t i=0;i<fTags.size();++i) {
            const auto& tag=fTags[i]; fList->AddItem(new Row(tag.name+(tag.pending.empty()?"":(tag.pending=="delete"?" · removal pending":" · rename pending"))));
            if(tag.name==fTagKey) fSelection=i;
        }
    }
    if (fSelection<0 && fList->CountItems()) fSelection=0;
    Load(fSelection); if (fSelection>=0) fList->Select(fSelection);
}
void OrganizationWindow::Load(int32 index)
{
    fSelection=index; fId=0; fTagKey.clear();
    if (fKind==OrganizationKind::Rules) {
        Filter rule; rule.account=fSeed.account;
        if (index>=0 && static_cast<std::size_t>(index)<fRules.size()) rule=fRules[index];
        fId=rule.id; BuildScope(rule.account); fName->SetText(rule.name.c_str()); fSender->SetText(rule.senderContains.c_str()); fSubject->SetText(rule.subjectContains.c_str());
        fTag->SetText(rule.addTag.c_str()); BuildFolders(rule.moveTo); fRead->SetValue(rule.markRead); fStar->SetValue(rule.star); fStop->SetValue(rule.stop); fEnabled->SetValue(rule.enabled);
    } else if (fKind==OrganizationKind::SmartFolders) {
        SmartFolder folder; folder.query=fSeed;
        if (index>=0 && static_cast<std::size_t>(index)<fSmart.size()) folder=fSmart[index];
        fId=folder.id; BuildScope(folder.query.account); fName->SetText(folder.name.c_str()); BuildFolders(folder.query.folder);
        fSearch->SetText(folder.query.text.c_str()); fTag->SetText(folder.query.tag.c_str()); fRead->SetValue(folder.query.unread); fStar->SetValue(folder.query.starred);
        fAttachments->SetValue(folder.query.attachments); fSnoozed->SetValue(folder.query.snoozedOnly); fIncludeSnoozed->SetValue(folder.query.includeSnoozed);
    } else {
        if (index>=0 && static_cast<std::size_t>(index)<fTags.size()) fTagKey=fTags[index].name;
        fName->SetText(fTagKey.c_str());
    }
    fBaseline=Signature(); UpdateButtons(); UpdateStatus();
}
void OrganizationWindow::New() { fList->DeselectAll(); Load(-1); fName->MakeFocus(); }
void OrganizationWindow::UpdateButtons()
{
    bool selected=fSelection>=0;
    bool deleting=fKind==OrganizationKind::Tags && selected && fTags[fSelection].pending=="delete";
    fSave->SetEnabled(fKind==OrganizationKind::Tags?(selected && !deleting && Signature()!=fBaseline):(!fId || Signature()!=fBaseline));
    fRemove->SetEnabled(selected && !deleting);
    fName->SetEnabled(fKind!=OrganizationKind::Tags || (selected && !deleting));
    if (fUp) { fUp->SetEnabled(fSelection>0); fDown->SetEnabled(selected && fSelection+1<fList->CountItems()); }
    if (fIncludeSnoozed) fIncludeSnoozed->SetEnabled(!on(fSnoozed));
}
void OrganizationWindow::UpdateStatus()
{
    std::string text;
    if (fKind==OrganizationKind::Rules) text="Both match fields must match; leave a field blank to match any text. Rules run from top to bottom on new inbox mail and imported messages. Changes do not move existing mail.";
    else if (fKind==OrganizationKind::SmartFolders) text="A smart folder is a saved view. All selected conditions must match. Editing or removing this view keeps your messages.";
    else if (fSelection>=0) {
        const auto& tag=fTags[fSelection];
        text=std::to_string(tag.messages)+(tag.messages==1?" cached message. ":" cached messages. ")+(tag.label.empty()?"This tag is stored on this computer.":"This is a Gmail label; changes are sent to Google on the next mail check.");
        text+="\nRules and smart folders limited to this account follow a rename. Views and rules for all accounts keep their current tags.";
        if (!tag.pending.empty()) text+="\n"+(tag.pending=="delete"?std::string("Removal pending."):std::string("Rename pending."));
        if (!tag.error.empty()) text+=" "+tag.error;
    } else text="No tags in this account. Use Message → Add tag to label selected conversations.";
    fStatus->SetText(text.c_str());
}
void OrganizationWindow::Save()
{
    if (fKind==OrganizationKind::Rules) {
        Filter rule; rule.id=fId; rule.account=Scope(); rule.name=fName->Text(); rule.senderContains=fSender->Text(); rule.subjectContains=fSubject->Text(); rule.addTag=fTag->Text(); rule.moveTo=Folder();
        rule.markRead=on(fRead); rule.star=on(fStar); rule.stop=on(fStop); rule.enabled=on(fEnabled); fId=fStore->saveFilter(rule);
    } else if (fKind==OrganizationKind::SmartFolders) {
        SmartFolder smart; smart.id=fId; smart.name=fName->Text(); smart.query.account=Scope(); smart.query.folder=Folder(); smart.query.text=fSearch->Text(); smart.query.tag=fTag->Text();
        smart.query.unread=on(fRead); smart.query.starred=on(fStar); smart.query.attachments=on(fAttachments); smart.query.snoozedOnly=on(fSnoozed); smart.query.includeSnoozed=on(fIncludeSnoozed);
        fId=fStore->saveSmartFolder(smart);
    } else {
        if (fTagKey.empty()) return;
        fStore->renameTag(fTagAccount,fTagKey,fName->Text()); fTagKey=fName->Text();
    }
    Reload(); fOwner.SendMessage(kOrganizationChanged);
}
void OrganizationWindow::Remove()
{
    if (fSelection<0 || !CanLeave()) return;
    std::string text;
    if (fKind==OrganizationKind::Rules) text="Remove this rule? Previously organized messages keep their current folders, tags and read state.";
    else if (fKind==OrganizationKind::SmartFolders) text="Remove this smart folder? Your messages will be kept.";
    else text="Remove “"+fTagKey+"” from this account? Messages will be kept. For Gmail, the label is also removed from Google and other devices. Rules limited to this account stop adding the tag; saved views keep their criteria.";
    auto* alert=new BAlert("Remove",text.c_str(),"Cancel","Remove",nullptr,B_WIDTH_AS_USUAL,B_WARNING_ALERT); alert->SetShortcut(0,B_ESCAPE); alert->SetDefaultButton(alert->ButtonAt(0));
    if (alert->Go()!=1) return;
    if (fKind==OrganizationKind::Rules) fStore->removeFilter(fId);
    else if (fKind==OrganizationKind::SmartFolders) fStore->removeSmartFolder(fId);
    else fStore->removeTag(fTagAccount,fTagKey);
    fId=0; fTagKey.clear(); Reload(); fOwner.SendMessage(kOrganizationChanged);
}
void OrganizationWindow::MessageReceived(BMessage* message)
{
    try {
        switch(message->what) {
            case kSelect: {
                auto selected=fList->CurrentSelection(); if(selected==fSelection) break;
                if (!CanLeave()) { if(fSelection>=0) fList->Select(fSelection); else fList->DeselectAll(); break; }
                Load(selected); break;
            }
            case kChanged:
                for (auto* field:{fName,fSender,fSubject,fTag,fSearch}) if(field) ApplyEmojiFonts(*field->TextView());
                UpdateButtons(); break;
            case kScope:
                if (fKind==OrganizationKind::Tags) {
                    auto selected=Scope(); BuildScope(fTagAccount);
                    if (!CanLeave()) break;
                    fTagAccount=selected; fTagKey.clear(); Reload();
                } else { auto folder=Folder(); BuildFolders(folder); UpdateButtons(); }
                break;
            case kOrganizationNew:
                if(CanLeave()) {
                    int64 account;
                    if(message->FindInt64("account",&account)==B_OK) {
                        fSeed.account=account; fSeed.folder=message->FindString("folder"); fSeed.text=message->FindString("search"); fSeed.tag=message->FindString("tag");
                        fSeed.unread=message->FindBool("unread"); fSeed.starred=message->FindBool("starred"); fSeed.attachments=message->FindBool("attachments");
                        fSeed.snoozedOnly=message->FindBool("snoozed"); fSeed.includeSnoozed=message->FindBool("include snoozed");
                    }
                    New(); Activate();
                }
                break;
            case kSave: Save(); break;
            case kRemove: Remove(); break;
            case kUp: case kDown:
                if (fId && CanLeave()) { fStore->moveFilter(fId,message->what==kUp?-1:1); Reload(); fOwner.SendMessage(kOrganizationChanged); } break;
            case kOrganizationReload:
                if (message->FindBool("automatic")) { if(Signature()==fBaseline) Reload(); }
                else if(CanLeave()) Reload();
                break;
            case kOrganizationActivate: Activate(); break;
            case kCheck: fOwner.SendMessage('mcsc'); break; // Request a check without cancelling an active one.
            default: BWindow::MessageReceived(message); break;
        }
    } catch (const std::exception& error) { (new BAlert(title(fKind),error.what(),"OK",nullptr,nullptr,B_WIDTH_AS_USUAL,B_WARNING_ALERT))->Go(); }
}
}
