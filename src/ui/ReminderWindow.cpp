#include "ReminderWindow.h"
#include "mail/Snooze.h"
#include "Emoji.h"
#include <Button.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <MessageRunner.h>
#include <ScrollView.h>
#include <TextView.h>
#include <algorithm>
#include <ctime>

namespace mail::ui {
namespace {
constexpr uint32 kSelect='srsl',kDismiss='srdi';
class Item : public BStringItem {
public:
    explicit Item(const std::string& text):BStringItem(text.c_str()) {}
    void DrawItem(BView* owner,BRect frame,bool) override
    {
        owner->SetHighColor(ui_color(IsSelected()?B_LIST_SELECTED_BACKGROUND_COLOR:B_LIST_BACKGROUND_COLOR)); owner->FillRect(frame);
        owner->SetLowColor(owner->HighColor()); owner->SetHighColor(ui_color(IsSelected()?B_LIST_SELECTED_ITEM_TEXT_COLOR:B_LIST_ITEM_TEXT_COLOR));
        BFont font; owner->GetFont(&font); DrawEmojiText(*owner,Text(),font,{frame.left+5,frame.top+BaselineOffset()},frame.Width()-10);
    }
};
}
ReminderWindow::ReminderWindow(BWindow* owner,std::shared_ptr<Store> store)
    :BWindow(BRect(0,0,640,400),"Mail reminders",B_TITLED_WINDOW,B_AUTO_UPDATE_SIZE_LIMITS|B_ASYNCHRONOUS_CONTROLS|B_CLOSE_ON_ESCAPE),fStore(std::move(store)),fOwner(owner)
{
    fList=new BListView("reminders list"); fList->SetSelectionMessage(new BMessage(kSelect)); fList->SetInvocationMessage(new BMessage(kReminderOpen));
    auto* scroll=new BScrollView("reminders scroll",fList,0,false,true); scroll->SetExplicitMinSize(BSize(550,180));
    fStatus=new EmojiTextView("reminder details"); fStatus->MakeEditable(false); fStatus->SetWordWrap(true);
    fStatus->SetViewUIColor(B_PANEL_BACKGROUND_COLOR); fStatus->SetLowUIColor(B_PANEL_BACKGROUND_COLOR);
    auto color=ui_color(B_PANEL_TEXT_COLOR); fStatus->SetFontAndColor(be_plain_font,B_FONT_ALL,&color);
    fStatus->SetExplicitMinSize(BSize(550,80)); fStatus->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED,80));
    fOpen=new BButton("reminder open","Open conversation",new BMessage(kReminderOpen));
    fAgain=new BButton("reminder again","Snooze again…",new BMessage(kReminderAgain));
    fDismiss=new BButton("reminder dismiss","Dismiss reminder",new BMessage(kDismiss));
    BLayoutBuilder::Group<>(this,B_VERTICAL).SetInsets(B_USE_WINDOW_INSETS).Add(scroll).Add(fStatus)
        .AddGroup(B_HORIZONTAL).Add(fOpen).Add(fAgain).Add(fDismiss).AddGlue().Add(new BButton("Close",new BMessage(B_QUIT_REQUESTED))).End();
    SetDefaultButton(fOpen); CenterIn(owner->Frame()); Reload();
    BMessage tick(kReminderReload); fTimer=std::make_unique<BMessageRunner>(BMessenger(this),&tick,5000000);
}
ReminderWindow::~ReminderWindow() { while(auto* item=fList->RemoveItem(int32(0))) delete item; }
void ReminderWindow::Reload()
{
    auto selected=fList->CurrentSelection(); Id old=selected>=0 && static_cast<std::size_t>(selected)<fRows.size()?fRows[selected].reminder.thread:0;
    while(auto* item=fList->RemoveItem(int32(0))) delete item;
    fRows.clear();
    for(const auto& reminder:fStore->reminders(std::time(nullptr))) {
        auto found=std::find_if(fRows.begin(),fRows.end(),[&](const Row& row){return row.reminder.thread==reminder.thread;});
        if(found==fRows.end()) fRows.push_back({{reminder.id},reminder}); else found->ids.push_back(reminder.id);
    }
    int32 index=0;
    for(std::size_t i=0;i<fRows.size();++i) {
        const auto& reminder=fRows[i].reminder; if(reminder.thread==old) index=i;
        fList->AddItem(new Item(reminder.subject.empty()?"(No subject)":reminder.subject));
    }
    if(!fRows.empty()) fList->Select(index);
    Select();
}
void ReminderWindow::Select()
{
    auto index=fList->CurrentSelection(); bool valid=index>=0 && static_cast<std::size_t>(index)<fRows.size();
    for(auto* button:{fOpen,fAgain,fDismiss}) button->SetEnabled(valid);
    std::string text="No reminders to review. Snoozed messages return to their current folders at the chosen time.";
    if(valid) { const auto& row=fRows[index]; text=row.reminder.accountName+"\nDue: "+describeSnoozeTime(row.reminder.due)+"\nDismissing this reminder keeps the conversation."; }
    fStatus->SetText(text.c_str());
}
void ReminderWindow::MessageReceived(BMessage* message)
{
    try {
        auto index=fList->CurrentSelection(); bool selected=index>=0 && static_cast<std::size_t>(index)<fRows.size();
        switch(message->what) {
            case kSelect: Select(); break;
            case kReminders: Activate(); Reload(); break;
            case kReminderReload: Reload(); break;
            case kReminderOpen: case kReminderAgain:
                if(selected) { BMessage request(message->what); request.AddInt64("reminder",fRows[index].reminder.id); fOwner.SendMessage(&request); } break;
            case kDismiss:
                if(selected) { fStore->dismissReminders(fRows[index].ids); Reload(); fOwner.SendMessage(kSnoozeChanged); } break;
            default: BWindow::MessageReceived(message); break;
        }
    } catch(const std::exception& e) { fStatus->SetText(e.what()); }
}
}
