#pragma once
#include "mail/Store.h"
#include <Messenger.h>
#include <Window.h>
#include <memory>
class BListView;
class BTextView;
class BButton;
class BMessageRunner;
class BGroupView;
namespace mail::ui {
constexpr uint32 kDraftReview='mdrv',kCheckDrafts='mdck',kDraftReviewChanged='mdrc';
class DraftWindow : public BWindow {
public:
    DraftWindow(BWindow*,std::shared_ptr<Store>);
    ~DraftWindow() override;
    void MessageReceived(BMessage*) override;
private:
    void Refresh();
    void Selection();
    std::shared_ptr<Store> fStore;
    BMessenger fOwner;
    BListView* fList;
    BListView* fMatchesList;
    BGroupView* fMatchesPanel;
    BTextView* fDetails;
    BButton *fCheck,*fKeep,*fUse,*fRetry;
    std::vector<GmailDraftState> fItems;
    std::vector<GmailDraftMatch> fMatches;
    Id fMatchJournal=0,fMatchRevision=0;
    bool fShowingMatches=true;
    std::unique_ptr<BMessageRunner> fTimer;
};
}
