#include "SnoozeWindow.h"
#include "Emoji.h"
#include "mail/Snooze.h"
#include <Application.h>
#include <Bitmap.h>
#include <Button.h>
#include <CheckBox.h>
#include <LayoutBuilder.h>
#include <IconUtils.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <Notification.h>
#include <PopUpMenu.h>
#include <Roster.h>
#include <Resources.h>
#include <TextControl.h>
#include <TextView.h>
#include <ctime>

namespace mail::ui {
namespace { constexpr uint32 kChoose='szwh',kEdit='szed',kSave='szsv'; }
SnoozeWindow::SnoozeWindow(BWindow* owner,std::shared_ptr<Store> store,std::vector<Id> messages,Timestamp initial)
    :BWindow(BRect(0,0,530,360),"Snooze mail",B_TITLED_WINDOW_LOOK,B_MODAL_SUBSET_WINDOW_FEEL,
        B_AUTO_UPDATE_SIZE_LIMITS|B_ASYNCHRONOUS_CONTROLS|B_CLOSE_ON_ESCAPE),fStore(std::move(store)),fOwner(owner),fMessages(std::move(messages))
{
    AddToSubset(owner);
    auto* menu=new BPopUpMenu("When");
    for(const char* label:{"In one hour","Tomorrow at 09:00","Next Monday at 09:00","Choose date and time"}) {
        auto* item=new BMenuItem(label,new BMessage(kChoose)); item->SetTarget(this); menu->AddItem(item);
    }
    menu->ItemAt(initial>std::time(nullptr)?3:0)->SetMarked(true);
    fWhen=new BMenuField("snooze when","When:",menu);
    fDate=new BTextControl("snooze date","Date (YYYY-MM-DD):","",nullptr);
    fTime=new BTextControl("snooze time","Time (HH:MM):","",nullptr);
    for(auto* field:{fDate,fTime}) field->SetModificationMessage(new BMessage(kEdit));
    fLater=new BCheckBox("snooze later","Use the second occurrence when the clock repeats",new BMessage(kEdit));
    fStatus=new EmojiTextView("snooze status"); fStatus->MakeEditable(false); fStatus->SetWordWrap(true);
    fStatus->SetViewUIColor(B_PANEL_BACKGROUND_COLOR); fStatus->SetLowUIColor(B_PANEL_BACKGROUND_COLOR);
    auto color=ui_color(B_PANEL_TEXT_COLOR); fStatus->SetFontAndColor(be_plain_font,B_FONT_ALL,&color);
    fStatus->SetExplicitMinSize(BSize(450,100)); fStatus->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED,100));
    auto* help=new EmojiTextView("snooze help"); help->MakeEditable(false); help->SetWordWrap(true);
    help->SetViewUIColor(B_PANEL_BACKGROUND_COLOR); help->SetLowUIColor(B_PANEL_BACKGROUND_COLOR); help->SetFontAndColor(be_plain_font,B_FONT_ALL,&color);
    help->SetText("Messages return to their current folders. Snooze is stored on this computer. Keep Mail running for on-time reminders; overdue reminders appear when you reopen Mail.");
    help->SetExplicitMinSize(BSize(450,85)); help->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED,85));
    fSave=new BButton("snooze save","Snooze",new BMessage(kSave));
    BLayoutBuilder::Group<>(this,B_VERTICAL).SetInsets(B_USE_WINDOW_INSETS)
        .Add(fWhen).Add(fDate).Add(fTime).Add(fLater).Add(fStatus).Add(help)
        .AddGroup(B_HORIZONTAL).AddGlue().Add(new BButton("snooze cancel","Cancel",new BMessage(B_QUIT_REQUESTED))).Add(fSave).End();
    SetDefaultButton(fSave); CenterIn(owner->Frame());
    if(initial>std::time(nullptr)) { fDate->SetText(localSnoozeDate(initial).c_str()); fTime->SetText(localSnoozeClock(initial).c_str()); auto times=localSnoozeTimes(fDate->Text(),fTime->Text()); fLater->SetValue(times.size()==2 && initial>=times.back()); }
    Choose();
}
void SnoozeWindow::Choose()
{
    auto* menu=fWhen->Menu(); int32 index=menu->IndexOf(menu->FindMarked()); bool custom=index==3;
    fDate->SetEnabled(custom); fTime->SetEnabled(custom);
    if(!custom) {
        auto preset=index==0?SnoozePreset::Hour:(index==1?SnoozePreset::Tomorrow:SnoozePreset::Monday);
        fPreset=snoozePreset(std::time(nullptr),preset);
        fDate->SetText(localSnoozeDate(fPreset).c_str()); fTime->SetText(localSnoozeClock(fPreset).c_str()); fLater->SetValue(B_CONTROL_OFF);
    }
    Preview();
}
Timestamp SnoozeWindow::Target()
{
    bool custom=fWhen->Menu()->IndexOf(fWhen->Menu()->FindMarked())==3;
    auto candidates=localSnoozeTimes(fDate->Text(),fTime->Text()); fLater->SetEnabled(custom && candidates.size()==2);
    auto value=custom?(fLater->Value()==B_CONTROL_ON?candidates.back():candidates.front()):fPreset;
    if(value<=std::time(nullptr)) throw std::invalid_argument("Choose a time in the future.");
    return value;
}
void SnoozeWindow::Preview()
{
    try { auto value=Target(); fStatus->SetText(("Return: "+describeSnoozeTime(value)).c_str()); fSave->SetEnabled(true); }
    catch(const std::exception& e) { fStatus->SetText(e.what()); fSave->SetEnabled(false); fLater->SetEnabled(false); }
}
void SnoozeWindow::MessageReceived(BMessage* message)
{
    try {
        switch(message->what) {
            case kSnoozeActivate: Activate(); break;
            case kChoose: Choose(); break;
            case kEdit: Preview(); break;
            case kSave: fStore->snooze(fMessages,Target()); fOwner.SendMessage(kSnoozeChanged); Quit(); break;
            default: BWindow::MessageReceived(message); break;
        }
    } catch(const std::exception& e) { fStatus->SetText(e.what()); }
}
bool NotifySnoozes(std::size_t count,const std::string& database)
{
    BNotification notification(B_INFORMATION_NOTIFICATION);
    if(auto* resources=BApplication::AppResources()) {
        size_t size=0; const void* icon=resources->LoadResource('VICN',"BEOS:ICON",&size);
        if(icon && size) {
            BBitmap bitmap(BRect(0,0,31,31),B_RGBA32);
            if(bitmap.InitCheck()==B_OK && BIconUtils::GetVectorIcon(static_cast<const uint8*>(icon),size,&bitmap)==B_OK) notification.SetIcon(&bitmap);
        }
    }
    notification.SetGroup("Mail"); notification.SetTitle("Snoozed mail is ready");
    auto text=std::to_string(count)+(count==1?" conversation is ready. Open Mail to review it.":" conversations are ready. Open Mail to review them.");
    notification.SetContent(text.c_str()); notification.SetMessageID(("mail-snooze:"+database).c_str());
    app_info info; if(be_app->GetAppInfo(&info)!=B_OK) return false;
    // Resolve the running executable directly, including portable copies that
    // have not yet been registered in the system MIME database by Tracker.
    if(notification.SetOnClickFile(&info.ref)!=B_OK) return false;
    notification.AddOnClickArg("--reminders");
    if(!database.empty()) { notification.AddOnClickArg("--database"); notification.AddOnClickArg(database.c_str()); }
    return notification.Send()==B_OK;
}
}
