#include "AttachmentViews.h"
#include "MailWindow.h"
#include "ComposerWindow.h"
#include "mail/Mime.h"
#include <Application.h>
#include <Autolock.h>
#include <Bitmap.h>
#include <BitmapStream.h>
#include <Button.h>
#include <Entry.h>
#include <File.h>
#include <ListView.h>
#include <MessageRunner.h>
#include <MimeType.h>
#include <NodeInfo.h>
#include <Screen.h>
#include <StringView.h>
#include <TextView.h>
#include <TranslatorRoster.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

namespace {
int checks=0;
void check(bool ok,const char* why) { ++checks; if (!ok) throw std::runtime_error(why); }
std::string read(const std::string& path) { std::ifstream file(path,std::ios::binary); return {(std::istreambuf_iterator<char>(file)),{}}; }
template<class F> void rejects(F fn,const char* why) { bool rejected=false; try { fn(); } catch (const std::exception&) { rejected=true; } check(rejected,why); }
template<class T> T* view(BWindow* window,const char* name)
{
    auto* result=dynamic_cast<T*>(window->FindView(name)); check(result,name); return result;
}
class Attachments : public BApplication {
public:
    Attachments(std::string directory,std::string png):BApplication("application/x-vnd.air-Mail-Attachments-Test"),fDirectory(std::move(directory)),fPng(std::move(png)) {}
    int result=1;
    void ReadyToRun() override
    {
        try {
            fImage={"Color blocks 🎉.png","image/png",{},read(fPng),false};
            fFile={"Mail drag "+std::to_string(getpid())+" café.bin","application/octet-stream",{},std::string("Binary\0attachment\xff",18),false};
            decoderChecks(); exportChecks();
            fStore=std::make_shared<mail::Store>(fDirectory+"/mail.db"); fMail=new mail::ui::MailWindow(fStore); fMail->Show();
            mail::Message message=mail::parseMessage("From: Ada <ada@example.test>\nTo: user@example.test\nSubject: Attachment preview and drag\nMessage-ID: <attachment-fixture@example.test>\n\nReceived image and binary attachment. Drag either file to Tracker or a message draft.");
            message.account=fStore->accounts()[0].id; message.remoteKey="attachment-fixture"; message.attachments={fImage,fFile}; fStore->ingest(message);
            fMail->PostMessage(mail::ui::kDraftChanged);
            BMessage tick('test'); fTimer=std::make_unique<BMessageRunner>(BMessenger(this),&tick,200000);
        } catch (const std::exception& e) { fail(e.what()); }
    }
    void MessageReceived(BMessage* message) override
    {
        if (message->what==mail::ui::kAttachmentExported) return;
        if (message->what!='test') { BApplication::MessageReceived(message); return; }
        try { if (++fTicks>600) throw std::runtime_error("attachment UI or physical Tracker drop timed out"); step(); }
        catch (const std::exception& e) { fail(e.what()); }
    }
private:
    template<class T> T* window()
    {
        for (int32 i=0;i<CountWindows();++i) if (auto* candidate=dynamic_cast<T*>(WindowAt(i))) return candidate;
        return nullptr;
    }
    void next() { ++fStep; std::printf("Attachment stage %d\n",fStep); std::fflush(stdout); }
    void fail(const char* why) { std::fprintf(stderr,"FAIL at attachment stage %d: %s\n",fStep,why); fTimer.reset(); PostMessage(B_QUIT_REQUESTED); }
    void decoderChecks()
    {
        std::atomic<bool> cancel{false}; auto bitmap=mail::ui::DecodeAttachmentImage(fImage,cancel);
        check(bitmap->Bounds()==BRect(0,0,479,269),"PNG decoder preserves dimensions");
        check(bitmap->ColorSpace()==B_RGBA32,"PNG decoder preserves alpha");
        auto* pixel=static_cast<const uint8*>(bitmap->Bits());
        check(pixel[0]==50 && pixel[1]==70 && pixel[2]==230 && pixel[3]==255,"PNG pixels decode without channel swaps");
        pixel+=200*bitmap->BytesPerRow()+300*4;
        check(pixel[0]==40 && pixel[1]==180 && pixel[2]==240 && pixel[3]==100,"partially transparent pixels retain alpha");
        auto bad=fImage; bad.data="not an image";
        rejects([&]{mail::ui::DecodeAttachmentImage(bad,cancel);},"malformed image returns a controlled preview error");
        bad.data=fImage.data.substr(0,40); rejects([&]{mail::ui::DecodeAttachmentImage(bad,cancel);},"truncated image cannot produce a misleading preview");
        cancel=true; rejects([&]{mail::ui::DecodeAttachmentImage(fImage,cancel);},"closing a preview cancels decoding");
    }
    BMessage exportRequest(const mail::Attachment& attachment,const char* name,bool authorized=true)
    {
        auto source=attachment; auto* exporter=new mail::ui::AttachmentExport(source,BMessenger(this)); auto offer=exporter->Offer();
        source.data="Changed after the drag began";
        check(offer.what==B_SIMPLE_DATA && mail::ui::IsMailAttachmentOffer(offer),"drag offers file and native Mail transfer formats");
        exporter->Run(); BMessenger target(exporter);
        entry_ref directory; check(get_ref_for_path(fDirectory.c_str(),&directory)==B_OK,"destination directory ref");
        BMessage request(B_COPY_TARGET); request.AddRef("directory",&directory); request.AddString("name",name);
        request.AddString("be:types",B_FILE_MIME_TYPE); request.AddString("be:filetypes",attachment.mediaType.c_str());
        BMessage context; offer.FindMessage("be:originator-data",&context);
        if (authorized) request.AddMessage("be:originator-data",&context);
        BMessage reply; check(target.SendMessage(&request,&reply,5000000,5000000)==B_OK,"negotiated file copy completes");
        if (!authorized) { check(reply.what==B_MESSAGE_NOT_UNDERSTOOD,"unrelated messages cannot request attachment writes"); target.SendMessage(B_CANCEL); }
        return reply;
    }
    void exportChecks()
    {
        auto reply=exportRequest(fFile,"new.bin"); check(reply.GetInt32("error",B_ERROR)==B_OK && read(fDirectory+"/new.bin")==fFile.data,"new-file export preserves the immutable binary snapshot");
        std::ofstream(fDirectory+"/reserved.bin"); reply=exportRequest(fFile,"reserved.bin");
        check(reply.GetInt32("error",B_ERROR)==B_OK && read(fDirectory+"/reserved.bin")==fFile.data,"Tracker's empty reservation receives the complete attachment");
        BFile saved((fDirectory+"/reserved.bin").c_str(),B_READ_ONLY); BNodeInfo info(&saved); char type[B_MIME_TYPE_LENGTH];
        check(info.GetType(type)==B_OK && type==fFile.mediaType,"saved attachment has its native MIME attribute");
        struct stat stat{}; check(::stat((fDirectory+"/reserved.bin").c_str(),&stat)==0 && (stat.st_mode&0777)==0600,"saved files have private permissions");
        std::ofstream(fDirectory+"/existing.bin")<<"Keep existing bytes"; reply=exportRequest(fFile,"existing.bin");
        check(reply.GetInt32("error",B_OK)!=B_OK && read(fDirectory+"/existing.bin")=="Keep existing bytes","drag never overwrites a populated file");
        check(symlink((fDirectory+"/existing.bin").c_str(),(fDirectory+"/link.bin").c_str())==0,"create destination symlink fixture");
        reply=exportRequest(fFile,"link.bin"); check(reply.GetInt32("error",B_OK)!=B_OK && read(fDirectory+"/existing.bin")=="Keep existing bytes","drag does not follow destination symlinks");
        if (mkfifo((fDirectory+"/pipe.bin").c_str(),0600)==0) {
            reply=exportRequest(fFile,"pipe.bin"); check(reply.GetInt32("error",B_OK)!=B_OK,"special-file destinations cannot block the attachment worker");
        } else {
            check(errno==ENOSYS || errno==EOPNOTSUPP || errno==ENOTSUP || errno==B_UNSUPPORTED,"FIFO fixture creation fails only when the OS lacks support");
            std::puts("Named FIFO destination check skipped: unsupported by this Haiku build.");
        }
        reply=exportRequest(fFile,"../outside.bin"); check(reply.GetInt32("error",B_OK)!=B_OK,"negotiated names cannot escape the chosen directory");
        exportRequest(fFile,"unauthorized.bin",false); check(!std::filesystem::exists(fDirectory+"/unauthorized.bin"),"unrelated request creates no file");
    }
    void offerTo(BHandler* destination,const mail::Attachment& attachment)
    {
        auto* exporter=new mail::ui::AttachmentExport(attachment,BMessenger(this)); auto offer=exporter->Offer(); exporter->Run();
        check(BMessenger(destination).SendMessage(&offer,BMessenger(exporter))==B_OK,"send a real drag offer through native message negotiation");
    }
    bool previewReady()
    {
        auto* preview=window<mail::ui::AttachmentPreviewWindow>(); if (!preview) return false;
        BAutolock lock(preview); return std::string(view<BStringView>(preview,"preview status")->Text())=="480 × 270 pixels";
    }
    void step()
    {
        auto* composer=window<mail::ui::ComposerWindow>(); auto* preview=window<mail::ui::AttachmentPreviewWindow>();
        switch (fStep) {
            case 0: {
                BAutolock lock(fMail); auto* conversations=view<BListView>(fMail,"conversations"); if (!conversations->CountItems()) return;
                conversations->Select(0); next(); break;
            }
            case 1: {
                BAutolock lock(fMail); auto* files=view<mail::ui::AttachmentListView>(fMail,"attachments"); if (files->CountItems()!=2) return;
                check(!view<BButton>(fMail,"preview attachment")->IsEnabled(),"reader preview starts disabled without a selected attachment");
                files->Select(0); next(); break;
            }
            case 2: {
                BAutolock lock(fMail); auto* button=view<BButton>(fMail,"preview attachment"); check(button->IsEnabled(),"reader enables preview for an image"); button->Invoke(); next(); break;
            }
            case 3:
                if (!previewReady()) return;
                preview->PostMessage(B_QUIT_REQUESTED); fMail->PostMessage('mnew'); next(); break;
            case 4: {
                if (!composer || preview) return;
                BAutolock lock(composer); offerTo(composer->FindView("compose body"),fImage); next(); break;
            }
            case 5: {
                auto drafts=fStore->drafts(); if (drafts.empty() || drafts[0].attachments.empty()) return;
                fDraft=drafts[0].id; const auto& image=drafts[0].attachments[0];
                check(image.data==fImage.data && image.inlineImage && !image.contentId.empty(),"received image dropped into the body becomes an inline attachment with unchanged bytes");
                BAutolock lock(composer); check(view<BButton>(composer,"compose preview")->IsEnabled(),"composer preview follows the selected image");
                view<BButton>(composer,"compose preview")->Invoke(); next(); break;
            }
            case 6: {
                if (!previewReady()) return;
                if (const char* path=std::getenv("MAIL_ATTACHMENT_SCREENSHOT")) {
                    BBitmap* bitmap=nullptr; check(BScreen().GetBitmap(&bitmap)==B_OK,"capture image preview"); BBitmapStream stream(bitmap);
                    BFile file(path,B_WRITE_ONLY|B_CREATE_FILE|B_ERASE_FILE);
                    check(BTranslatorRoster::Default()->Translate(&stream,nullptr,nullptr,&file,B_PNG_FORMAT)==B_OK,"save preview evidence");
                }
                BAutolock lock(composer); view<BButton>(composer,"remove attachment")->Invoke(); next(); break;
            }
            case 7: {
                if (!fStore->draft(fDraft).attachments.empty()) return;
                check(previewReady(),"open preview survives removing its attachment from the draft");
                BAutolock lock(composer); offerTo(composer->FindView("compose attachments"),fFile); next(); break;
            }
            case 8: {
                auto draft=fStore->draft(fDraft); if (draft.attachments.empty()) return;
                check(draft.attachments[0].data==fFile.data && !draft.attachments[0].inlineImage,"drag into the composer attachment list preserves binary data as a normal attachment");
                BAutolock lock(composer); check(!view<BButton>(composer,"compose preview")->IsEnabled(),"binary attachments do not enable image preview");
                preview->PostMessage(B_QUIT_REQUESTED); composer->PostMessage(B_QUIT_REQUESTED); next(); break;
            }
            case 9: {
                if (composer || preview) return;
                BAutolock lock(fMail); fMail->MoveTo(20,40); fMail->ResizeTo(980,700); fMail->Activate();
                view<BListView>(fMail,"attachments")->Select(1); next(); break;
            }
            case 10: {
                BAutolock lock(fMail); check(!view<BButton>(fMail,"preview attachment")->IsEnabled(),"reader disables preview for a binary attachment");
                auto* files=view<mail::ui::AttachmentListView>(fMail,"attachments");
                auto frame=files->ItemFrame(1); BPoint source=files->ConvertToScreen(BPoint(frame.left+60,(frame.top+frame.bottom)/2));
                fDesktop="/boot/home/Desktop/"+fFile.name;
                const char* path=std::getenv("MAIL_ATTACHMENT_DRAG"); check(path,"physical drag fixture needs a coordinate output path");
                std::ofstream file(path); auto screen=BScreen().Frame();
                file<<"{\"x\":"<<source.x<<",\"y\":"<<source.y<<",\"target_x\":"<<screen.right-20<<",\"target_y\":"<<screen.bottom-180
                    <<",\"width\":"<<screen.Width()+1<<",\"height\":"<<screen.Height()+1<<"}";
                next(); break;
            }
            case 11:
                if (!std::filesystem::exists(fDesktop) || read(fDesktop).size()!=fFile.data.size()) return;
                check(read(fDesktop)==fFile.data,"physical pointer drag to Tracker writes the exact received attachment");
                check(fStore->draft(fDraft).attachments[0].data==fFile.data,"export leaves the cached draft intact");
                unlink(fDesktop.c_str()); result=0; fTimer.reset();
                std::printf("Native attachments: %d checks passed (PNG/alpha, malformed/canceled decode, immutable export, Tracker reservation, MIME, permissions, collisions/symlinks, native cross-window drops, previews and physical Tracker drag)\n",checks);
                std::fflush(stdout); PostMessage(B_QUIT_REQUESTED); next(); break;
        }
    }
    std::string fDirectory,fPng,fDesktop;
    mail::Attachment fImage,fFile;
    std::shared_ptr<mail::Store> fStore;
    mail::ui::MailWindow* fMail=nullptr;
    mail::Id fDraft=0;
    std::unique_ptr<BMessageRunner> fTimer;
    int fStep=0,fTicks=0;
};
}
int main(int argc,char** argv)
{
    if (argc!=2) return 2;
    char directory[]="/tmp/mail-attachments-XXXXXX"; if (!mkdtemp(directory)) return 2;
    int result; { Attachments app(directory,argv[1]); app.Run(); result=app.result; }
    // Remove the symlink before its target; this Haiku libstdc++ remove_all
    // leaves dangling symlinks behind and then throws on the nonempty directory.
    for (const char* name:{"link.bin","pipe.bin","new.bin","reserved.bin","existing.bin","mail.db-wal","mail.db-shm","mail.db"})
        unlink((std::string(directory)+"/"+name).c_str());
    if (rmdir(directory)!=0) { std::perror("attachment fixture cleanup"); return 1; }
    return result;
}
