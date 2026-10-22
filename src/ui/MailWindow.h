#pragma once
#include "mail/Store.h"
#include "Credentials.h"
#include <Window.h>
#include <atomic>
#include <memory>
#include <thread>
#include <map>
#include <Messenger.h>

class BListView;
class BTextControl;
class BTextView;
class BStringView;
class BFilePanel;
class BMessageRunner;
class BButton;

namespace mail::ui {
class MailWindow : public BWindow {
public:
    explicit MailWindow(std::shared_ptr<Store> store,GoogleOptions google={},std::string database={});
    ~MailWindow() override;
    void MessageReceived(BMessage*) override;
    bool QuitRequested() override;
private:
    void BuildSidebar();
    void OpenOrganization(int kind,bool create=false);
    void RefreshOrganization();
    void Refresh();
    void ShowConversation();
    void OpenFormatted(Id thread=0,Id message=0);
    void Import(BMessage*);
    void Apply(uint32 action);
    void CheckMail(bool draftsOnly=false);
    void Compose(uint32 action,Id draft=0);
    void SendQueued();
    void RefreshOutbox();
    void RefreshReminders(bool notify=false);
    void OpenSnooze(const std::vector<Id>&);
    void OpenReminder(Id,bool again);
    std::vector<Id> SelectedMessages();
    void Error(const std::string&);
    std::shared_ptr<Store> fStore;
    GoogleOptions fGoogle;
    BListView* fSidebar = nullptr;
    BListView* fConversations = nullptr;
    BListView* fAttachments = nullptr;
    BTextControl* fSearch = nullptr;
    BTextView* fReader = nullptr;
    BStringView* fStatus = nullptr;
    BStringView* fTitle = nullptr;
    BButton* fCheckMail = nullptr;
    BButton* fOutboxButton = nullptr;
    BButton* fMoveButton = nullptr;
    BButton* fReminderButton = nullptr;
    BButton *fAttachmentPreview=nullptr,*fAttachmentSave=nullptr;
    BButton* fFormattedView=nullptr;
    std::map<std::string,Id> fFormattedThreads;
    BMessenger fOutbox;
    BMessenger fMoves;
    BMessenger fDraftReview;
    BMessenger fSnooze,fReminders;
    std::string fDatabase;
    std::map<int,BMessenger> fOrganization;
    std::unique_ptr<BFilePanel> fImportPanel;
    std::unique_ptr<BFilePanel> fSavePanel;
    std::unique_ptr<BMessageRunner> fTimer;
    std::unique_ptr<BMessageRunner> fDraftTimer;
    std::vector<Query> fViews;
    std::vector<std::string> fViewKeys;
    std::vector<Attachment> fFiles;
    Attachment fSaving;
    Query fQuery;
    std::thread fImportWorker;
    std::thread fSyncWorker;
    std::thread fSendWorker;
    std::atomic<bool> fCancel{false};
    std::atomic<bool> fCancelSync{false};
    std::atomic<bool> fCancelSend{false};
    bool fImporting = false;
    bool fSyncing = false;
    bool fAccountsOpen = false;
    bool fSending = false;
    bool fNeedsSentCheck = false;
    std::map<Id,BMessenger> fComposers;
    std::string fSyncError;
    Timestamp fLastSync = 0;
    Timestamp fDraftDue = 0;
    Id fLocalAccount = 0;
};
}
