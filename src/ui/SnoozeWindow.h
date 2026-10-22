#pragma once
#include "mail/Store.h"
#include <Window.h>
#include <Messenger.h>
#include <memory>

class BMenuField;
class BTextControl;
class BTextView;
class BCheckBox;
class BButton;
namespace mail::ui {
constexpr uint32 kSnoozeChanged='szch',kReminders='szrm',kReminderOpen='szop',kReminderAgain='szag',kReminderReload='szrf';
constexpr uint32 kPollSnoozes='szpl',kSnoozeActivate='szac';
class SnoozeWindow : public BWindow {
public:
    SnoozeWindow(BWindow*,std::shared_ptr<Store>,std::vector<Id>,Timestamp initial=0);
    void MessageReceived(BMessage*) override;
private:
    void Choose();
    Timestamp Target();
    void Preview();
    std::shared_ptr<Store> fStore;
    BMessenger fOwner;
    std::vector<Id> fMessages;
    BMenuField* fWhen;
    BTextControl *fDate,*fTime;
    BTextView* fStatus;
    BCheckBox* fLater;
    BButton* fSave;
    Timestamp fPreset=0;
};
// Returns true when the system notification service accepted the notification.
// In-app reminders remain until the user dismisses or snoozes them again.
bool NotifySnoozes(std::size_t conversations,const std::string& database);
}
