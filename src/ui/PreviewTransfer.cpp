#include "PreviewTransfer.h"
#include <Application.h>
#include <Directory.h>
#include <DataIO.h>
#include <Entry.h>
#include <File.h>
#include <FindDirectory.h>
#include <Path.h>
#include <Roster.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <memory>
#include <stdexcept>

namespace mail::ui {
namespace {
void require(bool ok,const char* why) { if(!ok) throw std::runtime_error(why); }
std::string directory()
{
    BPath path; require(find_directory(B_USER_CACHE_DIRECTORY,&path)==B_OK,"Could not find the preview cache."); path.Append("Mail/previews");
    require(create_directory(path.Path(),0700)==B_OK,"Could not create the preview cache.");
    struct stat st{}; require(lstat(path.Path(),&st)==0 && S_ISDIR(st.st_mode) && st.st_uid==geteuid(),"The preview cache is not a private folder.");
    require(chmod(path.Path(),0700)==0,"Could not protect the preview cache."); return path.Path();
}
}
std::string WritePreviewTransfer(const PreviewSnapshot& snapshot,const BMessenger& owner)
{
    auto json=encodePreview(snapshot); BMessage message('mpv1'); message.AddString("snapshot",json.c_str()); message.AddMessenger("owner",owner);
    BMallocIO bytes; require(message.Flatten(&bytes)==B_OK,"Could not prepare the message preview.");
    auto pattern=directory()+"/draft-XXXXXX"; int fd=mkstemp(pattern.data()); require(fd>=0,"Could not create the message preview.");
    struct Close { int fd; ~Close(){close(fd);} } closeFd{fd};
    try {
        std::size_t at=0;
        while(at<bytes.BufferLength()) {
            auto size=write(fd,static_cast<const char*>(bytes.Buffer())+at,bytes.BufferLength()-at);
            if(size<0 && errno==EINTR) continue;
            require(size>0,"Could not save the complete message preview."); at+=size;
        }
        require(fsync(fd)==0,"Could not save the complete message preview.");
    } catch(...) { unlink(pattern.c_str()); throw; }
    return pattern;
}
TransferredPreview ReadPreviewTransfer(const std::string& path)
{
    auto prefix=directory()+"/draft-";
    require(path.rfind(prefix,0)==0 && path.size()==prefix.size()+6 && path.find('/',prefix.size())==std::string::npos,"Invalid preview snapshot path.");
    int fd=open(path.c_str(),O_RDONLY|O_NOFOLLOW); require(fd>=0,"This preview snapshot is no longer available.");
    struct Close { int fd; ~Close(){close(fd);} } closeFd{fd};
    struct stat st{}; require(fstat(fd,&st)==0 && S_ISREG(st.st_mode) && st.st_uid==geteuid() && (st.st_mode&0077)==0
        && st.st_size>0 && static_cast<uint64>(st.st_size)<=kPreviewSnapshotLimit+4096,"Invalid preview snapshot file.");
    std::string data(st.st_size,'\0'); std::size_t at=0;
    while(at<data.size()) { auto size=read(fd,data.data()+at,data.size()-at); if(size<0 && errno==EINTR) continue; require(size>0,"Could not read the complete preview."); at+=size; }
    // The bounded snapshot is now in memory. No draft content remains in cache
    // while the renderer is open, and closing it never touches the mail database.
    unlink(path.c_str()); BMemoryIO input(data.data(),data.size()); BMessage message;
    require(message.Unflatten(&input)==B_OK && message.what=='mpv1',"Invalid preview snapshot format.");
    const char* json=nullptr; require(message.FindString("snapshot",&json)==B_OK,"The preview snapshot is incomplete.");
    TransferredPreview result; result.snapshot=decodePreview(json); message.FindMessenger("owner",&result.owner); return result;
}
void LaunchPreview(const PreviewSnapshot& snapshot,const BMessenger& owner)
{
    app_info info; require(be_app && be_app->GetAppInfo(&info)==B_OK,"Could not locate Mail.");
    BEntry executable(&info.ref),parent; BPath path; require(executable.GetParent(&parent)==B_OK && parent.GetPath(&path)==B_OK,"Could not locate Mail's preview application.");
    path.Append("MailPreview"); entry_ref ref; require(get_ref_for_path(path.Path(),&ref)==B_OK,"Install MailPreview beside Mail to preview messages.");
    auto transfer=WritePreviewTransfer(snapshot,owner); const char* args[]={transfer.c_str()};
    auto status=be_roster->Launch(&ref,1,args);
    if(status!=B_OK && status!=B_ALREADY_RUNNING) { unlink(transfer.c_str()); throw std::runtime_error("Could not open the message preview. Check that the air/OS WebKit runtime is installed."); }
}
}
