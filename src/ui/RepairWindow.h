#pragma once
#include <Messenger.h>
#include <Window.h>
#include <string>
#include <thread>

class BStringView;
namespace mail::ui {
enum : uint32 { kRepairStep='mrst', kRepairDone='mrdn' };
// Repairs a damaged database on its own thread, showing each step. When
// done it sends kRepairDone to the owner with "copy", "messages" and
// "removed", or with "error", and closes.
class RepairWindow : public BWindow {
public:
    RepairWindow(std::string database,BMessenger owner);
    ~RepairWindow() override;
    void MessageReceived(BMessage*) override;
    bool QuitRequested() override;
private:
    std::string fDatabase;
    BMessenger fOwner;
    BStringView* fStep;
    std::thread fWorker;
    bool fDone=false;
};
}
