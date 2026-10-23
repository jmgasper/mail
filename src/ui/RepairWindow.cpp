#include "RepairWindow.h"
#include "mail/Repair.h"
#include <LayoutBuilder.h>
#include <StringView.h>
#include <TextView.h>

namespace mail::ui {
RepairWindow::RepairWindow(std::string database,BMessenger owner)
    :BWindow(BRect(0,0,460,140),"Repairing Mail",B_TITLED_WINDOW,B_NOT_CLOSABLE|B_NOT_ZOOMABLE|B_NOT_RESIZABLE|B_AUTO_UPDATE_SIZE_LIMITS),
     fDatabase(std::move(database)),fOwner(owner)
{
    auto* title=new BStringView("repair title","Repairing your mail database");
    BFont bold(be_bold_font); title->SetFont(&bold);
    auto* about=new BTextView("repair details"); about->MakeEditable(false); about->MakeSelectable(false); about->SetWordWrap(true);
    about->SetViewUIColor(B_PANEL_BACKGROUND_COLOR); about->SetLowUIColor(B_PANEL_BACKGROUND_COLOR);
    auto color=ui_color(B_PANEL_TEXT_COLOR); about->SetFontAndColor(be_plain_font,B_FONT_ALL,&color);
    about->SetText("Mail found damage in its database and is rebuilding it from every readable part. "
        "This can take several minutes for a large mailbox. The damaged copy is kept.");
    about->SetExplicitMinSize(BSize(420,50));
    fStep=new BStringView("repair step","Starting…");
    BLayoutBuilder::Group<>(this,B_VERTICAL).SetInsets(B_USE_WINDOW_INSETS).Add(title).Add(about).Add(fStep);
    CenterOnScreen();
    fWorker=std::thread([this,messenger=BMessenger(this)] {
        BMessage done(kRepairDone);
        try {
            auto report=RepairStore(fDatabase,[&](const std::string& step) {
                BMessage message(kRepairStep); message.AddString("step",step.c_str()); messenger.SendMessage(&message);
            });
            done.AddString("copy",report.damagedCopy.c_str());
            done.AddInt64("messages",report.messages); done.AddInt64("removed",report.removed);
        } catch(const std::exception& error) { done.AddString("error",error.what()); }
        messenger.SendMessage(&done);
    });
}
RepairWindow::~RepairWindow() { if(fWorker.joinable()) fWorker.join(); }
void RepairWindow::MessageReceived(BMessage* message)
{
    switch(message->what) {
        case kRepairStep: fStep->SetText(message->GetString("step","")); return;
        case kRepairDone: fDone=true; fOwner.SendMessage(message); Quit(); return;
    }
    BWindow::MessageReceived(message);
}
bool RepairWindow::QuitRequested() { return fDone; }
}
