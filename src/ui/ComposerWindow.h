#pragma once
#include "mail/Store.h"
#include <Messenger.h>
#include <Window.h>
#include <memory>
#include <map>
class BTextControl;
class BMenuField;
class BListView;
class BStringView;
class BFilePanel;
class BMessageRunner;
class BButton;
class BMenuItem;
namespace mail::ui {
constexpr uint32 kDraftChanged='mdch',kSendQueued='msnd',kComposeActivate='mcac';
class DraftEditor;
class ComposerSession;
class ComposerWindow : public BWindow {
public:
    ComposerWindow(BWindow*,std::shared_ptr<Store>,Draft);
    ~ComposerWindow() override;
    bool QuitRequested() override;
    void MessageReceived(BMessage*) override;
    void MenusBeginning() override;
private:
    void Save();
    void Attach(BMessage*);
    void ReceiveAttachment(BMessage*);
    void RefreshFiles();
    void Error(const std::string&);
    std::shared_ptr<Store> fStore;
    Draft fDraft;
    std::unique_ptr<ComposerSession> fSession;
    std::string fPreviewKey;
    BMessenger fOwner;
    BMenuField* fFrom;
    BTextControl *fTo,*fCc,*fBcc,*fSubject;
    DraftEditor* fEditor;
    BListView* fFiles;
    BStringView* fStatus;
    BButton *fPreview,*fRemove,*fInline;
    BMenuItem *fUndoItem,*fRedoItem;
    std::unique_ptr<BFilePanel> fPanel;
    std::unique_ptr<BMessageRunner> fTimer;
    struct PendingDrop { bool inlineImage; bigtime_t started; int64 position; };
    std::map<std::string,PendingDrop> fPendingDrops;
    bool fDirty=false,fCommitted=false;
    uint64 fSavedEditorChanges=0;
};
}
