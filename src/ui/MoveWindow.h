#pragma once
#include "mail/Store.h"
#include <Messenger.h>
#include <Window.h>
#include <memory>
class BListView;
class BTextView;
class BMessageRunner;
class BButton;
namespace mail::ui {
constexpr uint32 kFolderMoves='mfmv',kMovesChanged='mvch',kCheckMoves='mvck';
class MoveWindow : public BWindow {
public:
    MoveWindow(BWindow*,std::shared_ptr<Store>);
    ~MoveWindow() override;
    void MessageReceived(BMessage*) override;
private:
    void Refresh();
    void UpdateSelection();
    std::shared_ptr<Store> fStore;
    BMessenger fOwner;
    BListView* fList;
    BTextView* fDetails;
    BButton *fCheck,*fRetry,*fKeep;
    std::vector<PendingMove> fItems;
    std::unique_ptr<BMessageRunner> fTimer;
};
}
