#pragma once
#include "PreviewTransfer.h"
#include <Window.h>
#include <memory>

class BWebKitContext;
class BWebKitView;
class BStringView;
class BMenuField;
class BButton;
namespace mail::ui {
class MessagePreviewWindow : public BWindow {
public:
    MessagePreviewWindow(TransferredPreview,std::shared_ptr<BWebKitContext>);
    bool QuitRequested() override;
    void MessageReceived(BMessage*) override;
    void SetSnapshot(TransferredPreview);
    const std::string& Key() const { return fSnapshot.snapshot.key; }
private:
    void Serve(BMessage&);
    void Render();
    TransferredPreview fSnapshot;
    std::string fRoot,fHtml,fRenderNotice;
    BWebKitView* fWeb;
    BStringView *fSubject,*fFrom,*fTo,*fCc,*fBcc,*fFiles,*fStatus;
    BStringView* fDate;
    BMenuField* fThread;
    BButton *fEdit,*fPlain;
    bool fPlainText=false;
};
}
