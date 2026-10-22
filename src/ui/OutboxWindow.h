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
constexpr uint32 kOpenDraft='mopd';
constexpr uint32 kOutboxChanged='moch',kOutboxActivate='oact',kCheckSentCopies='mcsc';
class OutboxWindow : public BWindow {
public:
    OutboxWindow(BWindow*,std::shared_ptr<Store>);
    ~OutboxWindow() override;
    void MessageReceived(BMessage*) override;
private:
    void Refresh();
    void UpdateSelection();
    std::shared_ptr<Store> fStore;
    BMessenger fOwner;
    BListView* fList;
    BTextView* fDetails;
    BButton *fRetry,*fEdit;
    std::vector<Outgoing> fItems;
    std::vector<SentCopy> fCopies;
    std::unique_ptr<BMessageRunner> fTimer;
};
}
