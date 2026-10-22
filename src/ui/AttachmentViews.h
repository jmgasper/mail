#pragma once
#include "mail/Model.h"
#include <ListView.h>
#include <ListItem.h>
#include <Looper.h>
#include <Messenger.h>
#include <Window.h>
#include <atomic>
#include <functional>
#include <memory>
#include <thread>
class BBitmap;
class BMessageRunner;
class BStringView;
namespace mail::ui {
constexpr uint32 kAttachmentExported='aexp';
inline constexpr char kMailAttachmentType[]="application/x-vnd.air-Mail-attachment";
bool IsMailAttachmentOffer(const BMessage&);
class AttachmentItem : public BStringItem {
public:
    explicit AttachmentItem(const std::string& text):BStringItem(text.c_str()) {}
    void DrawItem(BView*,BRect,bool) override;
};
// One immutable transfer, independent of the source window's later selection.
// Owns its looper lifetime after Run(); canceled drags expire after five minutes.
class AttachmentExport : public BLooper {
public:
    AttachmentExport(const Attachment&,BMessenger owner);
    ~AttachmentExport() override;
    BMessage Offer() const;
    void MessageReceived(BMessage*) override;
private:
    Attachment fAttachment;
    BMessenger fOwner;
    std::string fToken;
    std::unique_ptr<BMessageRunner> fExpiry;
};
class AttachmentListView : public BListView {
public:
    AttachmentListView(const char* name,std::function<const Attachment*(int32)> attachment,bool receives=false);
    bool InitiateDrag(BPoint,int32,bool) override;
    void MessageReceived(BMessage*) override;
private:
    std::function<const Attachment*(int32)> fAttachment;
    bool fReceives;
};
// Translation output is bounded before allocating a native bitmap.
std::unique_ptr<BBitmap> DecodeAttachmentImage(const Attachment&,const std::atomic<bool>& cancelled);
class AttachmentImageView;
class AttachmentPreviewWindow : public BWindow {
public:
    explicit AttachmentPreviewWindow(Attachment);
    ~AttachmentPreviewWindow() override;
    void MessageReceived(BMessage*) override;
    bool QuitRequested() override;
private:
    AttachmentImageView* fImage;
    BStringView* fStatus;
    std::unique_ptr<BBitmap> fDecoded;
    std::string fError;
    std::thread fWorker;
    std::atomic<bool> fCancel{false};
    bool fLoading=true,fCloseWhenDone=false;
};
}
