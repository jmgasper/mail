#include "AttachmentViews.h"
#include "Emoji.h"
#include "mail/Compose.h"
#include <Bitmap.h>
#include <Button.h>
#include <ByteOrder.h>
#include <ControlLook.h>
#include <DataIO.h>
#include <Entry.h>
#include <LayoutBuilder.h>
#include <MessageRunner.h>
#include <MimeType.h>
#include <Path.h>
#include <StringView.h>
#include <TranslatorFormats.h>
#include <TranslatorRoster.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <fs_attr.h>
#include <mutex>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace mail::ui {
namespace {
constexpr std::size_t kBitmapLimit=64*1024*1024;
constexpr uint32 kImageReady='aird';
std::mutex exportMutex;
std::size_t exportBytes=0,exportCount=0;
void require(bool ok,const char* why) { if (!ok) throw std::runtime_error(why); }
std::string mime(const Attachment& attachment)
{
    BMimeType type(attachment.mediaType.c_str());
    return type.IsValid()?attachment.mediaType:"application/octet-stream";
}
bool hasString(const BMessage& message,const char* field,const std::string& value)
{
    const char* item=nullptr;
    for (int32 i=0;message.FindString(field,i,&item)==B_OK;++i) if (item==value) return true;
    return false;
}
struct Descriptor {
    explicit Descriptor(int fd):value(fd) {}
    ~Descriptor() { if (value>=0) close(value); }
    int value;
};
entry_ref exportFile(const Attachment& attachment,const entry_ref& directory,const std::string& name)
{
    require(!name.empty() && name!="." && name!=".." && name.size()<B_FILE_NAME_LENGTH
        && name.find('/')==std::string::npos && name.find('\0')==std::string::npos,"Choose a valid attachment filename.");
    BPath path(&directory); require(path.InitCheck()==B_OK,"The destination folder is no longer available.");
    Descriptor folder(open(path.Path(),O_RDONLY|O_DIRECTORY)); require(folder.value>=0,"Could not open the destination folder.");
    int fd=openat(folder.value,name.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_NONBLOCK,0600);
    if (fd<0 && errno==EEXIST) fd=openat(folder.value,name.c_str(),O_WRONLY|O_NOFOLLOW|O_NONBLOCK);
    Descriptor output(fd); require(fd>=0,"Could not create the attachment at the drop location.");
    struct stat stat{};
    // Tracker reserves an empty file before negotiating the copy. Never replace
    // a populated file, a symlink, or another user's reservation.
    require(fstat(fd,&stat)==0 && S_ISREG(stat.st_mode) && stat.st_size==0 && stat.st_nlink==1 && stat.st_uid==geteuid(),
        "The destination already contains a file. Choose another name or location.");
    require(fchmod(fd,0600)==0,"Could not set attachment file permissions.");
    std::size_t written=0;
    while (written<attachment.data.size()) {
        auto count=write(fd,attachment.data.data()+written,std::min<std::size_t>(65536,attachment.data.size()-written));
        if (count<0 && errno==EINTR) continue;
        if (count<=0) { ftruncate(fd,0); throw std::runtime_error("Could not save the complete attachment. Check free disk space and folder permissions."); }
        written+=count;
    }
    auto type=mime(attachment);
    require(fs_write_attr(fd,"BEOS:TYPE",B_MIME_STRING_TYPE,0,type.c_str(),type.size()+1)==static_cast<ssize_t>(type.size()+1)
        && fsync(fd)==0,"Could not finish saving the attachment.");
    path.Append(name.c_str()); entry_ref result;
    require(get_ref_for_path(path.Path(),&result)==B_OK,"Could not locate the saved attachment."); return result;
}
class BitmapOutput : public BMallocIO {
public:
    explicit BitmapOutput(const std::atomic<bool>& cancelled):fCancelled(cancelled) {}
    ssize_t WriteAt(off_t position,const void* bytes,size_t size) override
    {
        if (fCancelled) return B_CANCELED;
        if (position<0 || static_cast<uint64>(position)>kBitmapLimit || size>kBitmapLimit-static_cast<size_t>(position)) return B_NO_MEMORY;
        return BMallocIO::WriteAt(position,bytes,size);
    }
    status_t SetSize(off_t size) override
    {
        if (fCancelled) return B_CANCELED;
        return size<0 || static_cast<uint64>(size)>kBitmapLimit?B_NO_MEMORY:BMallocIO::SetSize(size);
    }
private: const std::atomic<bool>& fCancelled;
};
}
bool IsMailAttachmentOffer(const BMessage& message)
{
    if (message.what!=B_SIMPLE_DATA || !hasString(message,"be:types",kMailAttachmentType)) return false;
    int32 action=0;
    for (int32 i=0;message.FindInt32("be:actions",i,&action)==B_OK;++i) if (action==B_COPY_TARGET) return true;
    return false;
}
void AttachmentItem::DrawItem(BView* owner,BRect frame,bool)
{
    auto low=owner->LowColor(),high=owner->HighColor();
    auto background=ui_color(IsSelected()?B_LIST_SELECTED_BACKGROUND_COLOR:B_LIST_BACKGROUND_COLOR);
    owner->SetLowColor(background); owner->SetHighColor(background); owner->FillRect(frame);
    owner->SetHighColor(ui_color(IsSelected()?B_LIST_SELECTED_ITEM_TEXT_COLOR:B_LIST_ITEM_TEXT_COLOR));
    BFont font; owner->GetFont(&font); float inset=be_control_look->DefaultLabelSpacing();
    std::string label=Text()?Text():""; for (char& c:label) if (c=='\n' || c=='\r' || c=='\t') c=' ';
    DrawEmojiText(*owner,label,font,BPoint(frame.left+inset,frame.top+BaselineOffset()),frame.Width()-inset*2);
    owner->SetLowColor(low); owner->SetHighColor(high);
}
AttachmentExport::AttachmentExport(const Attachment& attachment,BMessenger owner)
    :BLooper("attachment copy"),fAttachment(attachment),fOwner(owner),fToken(newMessageId())
{
    BMessage expire(B_QUIT_REQUESTED); fExpiry=std::make_unique<BMessageRunner>(BMessenger(this),&expire,300000000,1);
    std::lock_guard<std::mutex> guard(exportMutex);
    require(exportCount<8 && fAttachment.data.size()<=32*1024*1024 && exportBytes+fAttachment.data.size()<=128*1024*1024,
        "Finish pending attachment drags before starting another.");
    ++exportCount; exportBytes+=fAttachment.data.size();
}
AttachmentExport::~AttachmentExport()
{
    std::lock_guard<std::mutex> guard(exportMutex); --exportCount; exportBytes-=fAttachment.data.size();
}
BMessage AttachmentExport::Offer() const
{
    BMessage offer(B_SIMPLE_DATA); offer.AddString("be:types",kMailAttachmentType); offer.AddString("be:types",B_FILE_MIME_TYPE);
    auto type=mime(fAttachment); offer.AddString("be:filetypes",type.c_str()); offer.AddString("be:type_descriptions",type.c_str());
    offer.AddInt32("be:actions",B_COPY_TARGET); offer.AddString("be:clip_name",fAttachment.name.empty()?"attachment":fAttachment.name.c_str());
    BMessage context; context.AddString("token",fToken.c_str()); offer.AddMessage("be:originator-data",&context); return offer;
}
void AttachmentExport::MessageReceived(BMessage* message)
{
    if (message->what==B_CANCEL) { Quit(); return; }
    if (message->what!=B_COPY_TARGET) { BLooper::MessageReceived(message); return; }
    BMessage context; const char* token=nullptr;
    if (message->FindMessage("be:originator-data",&context)!=B_OK) {
        auto* previous=message->Previous(); if (previous) previous->FindMessage("be:originator-data",&context);
    }
    if (context.FindString("token",&token)!=B_OK || token!=fToken) { BMessage reply(B_MESSAGE_NOT_UNDERSTOOD); message->SendReply(&reply); return; }
    BMessage report(kAttachmentExported),reply(B_REPLY); reply.AddMessage("be:originator-data",&context);
    try {
        if (hasString(*message,"be:types",kMailAttachmentType)) {
            reply.what=B_MIME_DATA; reply.AddString("name",fAttachment.name.c_str()); reply.AddString("media_type",mime(fAttachment).c_str());
            reply.AddInt64("size",fAttachment.data.size());
            if (!fAttachment.data.empty()) require(reply.AddData(kMailAttachmentType,B_MIME_TYPE,fAttachment.data.data(),fAttachment.data.size())==B_OK,"Could not copy the attachment into the draft.");
            require(message->SendReply(&reply)==B_OK,"The receiving draft is no longer available."); Quit(); return;
        }
        entry_ref directory; const char* name=nullptr;
        require(hasString(*message,"be:types",B_FILE_MIME_TYPE) && hasString(*message,"be:filetypes",mime(fAttachment)),"The drop target requested an unsupported attachment format.");
        require(message->FindRef("directory",&directory)==B_OK && message->FindString("name",&name)==B_OK,"The drop target did not provide a destination.");
        auto ref=exportFile(fAttachment,directory,name); reply.AddRef("refs",&ref); reply.AddInt32("error",B_OK);
        report.AddString("text",("Saved attachment: "+std::string(name)).c_str());
    } catch (const std::exception& error) { reply.what=B_REPLY; reply.AddInt32("error",B_ERROR); reply.AddString("text",error.what()); report.AddString("text",error.what()); report.AddBool("failed",true); }
    message->SendReply(&reply); fOwner.SendMessage(&report); Quit();
}
AttachmentListView::AttachmentListView(const char* name,std::function<const Attachment*(int32)> attachment,bool receives)
    :BListView(name),fAttachment(std::move(attachment)),fReceives(receives) {}
void AttachmentListView::MessageReceived(BMessage* message)
{
    if (fReceives && (((message->what==B_SIMPLE_DATA || message->what==B_REFS_RECEIVED) && message->HasRef("refs")) || IsMailAttachmentOffer(*message))) {
        BMessenger(Window()).SendMessage(message,message->ReturnAddress()); return;
    }
    BListView::MessageReceived(message);
}
bool AttachmentListView::InitiateDrag(BPoint,int32 index,bool)
{
    const auto* attachment=fAttachment(index); if (!attachment) return false;
    try {
        auto transfer=std::make_unique<AttachmentExport>(*attachment,BMessenger(Window())); auto offer=transfer->Offer();
        require(transfer->Run()>=0,"Could not start the attachment copy.");
        auto* target=transfer.release(); DragMessage(&offer,ItemFrame(index),target); return true;
    } catch (const std::exception& error) { BMessage report(kAttachmentExported); report.AddString("text",error.what()); report.AddBool("failed",true); Window()->PostMessage(&report); return false; }
}
std::unique_ptr<BBitmap> DecodeAttachmentImage(const Attachment& attachment,const std::atomic<bool>& cancelled)
{
    require(!cancelled,"Image preview canceled.");
    require(attachment.data.size()<=32*1024*1024,"This image is too large to preview.");
    BMemoryIO input(attachment.data.data(),attachment.data.size()); BitmapOutput output(cancelled);
    require(BTranslatorRoster::Default()->Translate(&input,nullptr,nullptr,&output,B_TRANSLATOR_BITMAP)==B_OK,
        "This image could not be previewed. It may be damaged, too large, or need an additional image translator.");
    require(!cancelled && output.BufferLength()>=sizeof(TranslatorBitmap),"Image preview canceled or incomplete.");
    struct { uint32 magic; float left,top,right,bottom; uint32 rowBytes,colors,dataSize; } header;
    static_assert(sizeof(header)==sizeof(TranslatorBitmap)); std::memcpy(&header,output.Buffer(),sizeof(header));
    header.magic=B_BENDIAN_TO_HOST_INT32(header.magic); header.rowBytes=B_BENDIAN_TO_HOST_INT32(header.rowBytes);
    header.dataSize=B_BENDIAN_TO_HOST_INT32(header.dataSize); header.colors=B_BENDIAN_TO_HOST_INT32(header.colors);
    float left=B_BENDIAN_TO_HOST_FLOAT(header.left),top=B_BENDIAN_TO_HOST_FLOAT(header.top);
    float width=B_BENDIAN_TO_HOST_FLOAT(header.right)-left+1,height=B_BENDIAN_TO_HOST_FLOAT(header.bottom)-top+1;
    require(header.magic==B_TRANSLATOR_BITMAP && std::isfinite(width) && std::isfinite(height) && width>=1 && height>=1
        && width<=8192 && height<=8192 && width==std::floor(width) && height==std::floor(height),"This image has unsupported dimensions.");
    require(header.rowBytes && uint64(header.rowBytes)*static_cast<uint64>(height)==header.dataSize
        && header.dataSize<=kBitmapLimit-sizeof(header) && output.BufferLength()==sizeof(header)+header.dataSize,"The image translator returned incomplete bitmap data.");
    // Supply the already bounded stride: BBitmap rejects strides too small for
    // the color space before allocating, and cannot infer a larger allocation
    // from an inconsistent width/height header.
    auto bitmap=std::make_unique<BBitmap>(BRect(0,0,width-1,height-1),0,static_cast<color_space>(header.colors),header.rowBytes);
    require(bitmap->InitCheck()==B_OK && bitmap->BitsLength()>0 && static_cast<std::size_t>(bitmap->BitsLength())<=kBitmapLimit
        && bitmap->BytesPerRow()==static_cast<int32>(header.rowBytes),"This image format cannot be previewed.");
    require(static_cast<uint64>(bitmap->BitsLength())==header.dataSize,"The image bitmap has inconsistent dimensions.");
    std::memcpy(bitmap->Bits(),static_cast<const char*>(output.Buffer())+sizeof(header),header.dataSize); return bitmap;
}
class AttachmentImageView : public BView {
public:
    AttachmentImageView():BView("attachment image",B_WILL_DRAW|B_FULL_UPDATE_ON_RESIZE) { SetViewUIColor(B_PANEL_BACKGROUND_COLOR); SetExplicitMinSize(BSize(300,200)); }
    void SetImage(std::unique_ptr<BBitmap> image) { fImage=std::move(image); Invalidate(); }
    void Draw(BRect) override
    {
        SetHighUIColor(B_PANEL_BACKGROUND_COLOR); FillRect(Bounds()); if (!fImage) return;
        auto source=fImage->Bounds(); float scale=std::min({1.0f,(Bounds().Width()-24)/(source.Width()+1),(Bounds().Height()-24)/(source.Height()+1)});
        float width=(source.Width()+1)*scale,height=(source.Height()+1)*scale;
        BRect target(0,0,width-1,height-1); target.OffsetTo((Bounds().Width()-width)/2,(Bounds().Height()-height)/2);
        SetDrawingMode(B_OP_ALPHA); SetBlendingMode(B_PIXEL_ALPHA,B_ALPHA_OVERLAY);
        DrawBitmap(fImage.get(),source,target,B_FILTER_BITMAP_BILINEAR); SetDrawingMode(B_OP_COPY);
    }
private: std::unique_ptr<BBitmap> fImage;
};
AttachmentPreviewWindow::AttachmentPreviewWindow(Attachment attachment)
    :BWindow(BRect(230,130,930,660),"Attachment preview",B_TITLED_WINDOW,B_AUTO_UPDATE_SIZE_LIMITS|B_CLOSE_ON_ESCAPE)
{
    auto* title=new EmojiStringView("preview filename",attachment.name.c_str()); title->SetFont(be_bold_font);
    title->SetExplicitMinSize(BSize(100,B_SIZE_UNSET)); title->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED,B_SIZE_UNSET));
    fImage=new AttachmentImageView; fStatus=new BStringView("preview status","Loading image…");
    fStatus->SetExplicitMinSize(BSize(100,B_SIZE_UNSET)); fStatus->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED,B_SIZE_UNSET));
    BLayoutBuilder::Group<>(this,B_VERTICAL).SetInsets(B_USE_WINDOW_INSETS).Add(title).Add(fImage,1)
        .AddGroup(B_HORIZONTAL).Add(fStatus,1).Add(new BButton("Close",new BMessage(B_QUIT_REQUESTED))).End();
    fWorker=std::thread([this,attachment=std::move(attachment)] {
        try { fDecoded=DecodeAttachmentImage(attachment,fCancel); } catch (const std::exception& error) { fError=error.what(); }
        BMessenger(this).SendMessage(kImageReady);
    });
}
AttachmentPreviewWindow::~AttachmentPreviewWindow() { fCancel=true; if (fWorker.joinable()) fWorker.join(); }
bool AttachmentPreviewWindow::QuitRequested()
{
    if (!fLoading) return true;
    fCancel=true; fCloseWhenDone=true; Hide(); return false;
}
void AttachmentPreviewWindow::MessageReceived(BMessage* message)
{
    if (message->what!=kImageReady) { BWindow::MessageReceived(message); return; }
    if (fWorker.joinable()) fWorker.join();
    fLoading=false;
    if (fCloseWhenDone) { PostMessage(B_QUIT_REQUESTED); return; }
    if (fDecoded) {
        auto bounds=fDecoded->Bounds(); std::string size=std::to_string(bounds.IntegerWidth()+1)+" × "+std::to_string(bounds.IntegerHeight()+1)+" pixels";
        fStatus->SetText(size.c_str()); fImage->SetImage(std::move(fDecoded));
    } else { fStatus->SetText(fError.c_str()); fStatus->SetToolTip(fError.c_str()); }
}
}
