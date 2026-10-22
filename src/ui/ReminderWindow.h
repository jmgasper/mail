#pragma once
#include "SnoozeWindow.h"
class BListView;
class BMessageRunner;
namespace mail::ui {
class ReminderWindow : public BWindow {
public:
    ReminderWindow(BWindow*,std::shared_ptr<Store>);
    ~ReminderWindow() override;
    void MessageReceived(BMessage*) override;
private:
    void Reload();
    void Select();
    struct Row { std::vector<Id> ids; SnoozeReminder reminder; };
    std::shared_ptr<Store> fStore;
    BMessenger fOwner;
    BListView* fList;
    BTextView* fStatus;
    BButton *fOpen,*fAgain,*fDismiss;
    std::vector<Row> fRows;
    std::unique_ptr<BMessageRunner> fTimer;
};
}
