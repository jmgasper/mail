#include "Icons.h"
#include <Application.h>
#include <Autolock.h>
#include <Bitmap.h>
#include <ControlLook.h>
#include <IconUtils.h>
#include <Locker.h>
#include <Resources.h>
#include <map>
#include <memory>
#include <string>

namespace mail::ui {
namespace {
BLocker gIconLock("mail icons");
// Never destroyed: bitmaps can only be freed while the app_server link exists,
// which is gone by the time static objects are destroyed.
auto& gIcons=*new std::map<std::pair<std::string,int32>,std::unique_ptr<BBitmap>>;
}
float IconSize(int32 size)
{
    return be_control_look?be_control_look->ComposeIconSize(size).Width()+1:size;
}
const BBitmap* MailIcon(const char* name,int32 size)
{
    BAutolock lock(gIconLock);
    auto key=std::make_pair(std::string(name),size);
    auto found=gIcons.find(key);
    if(found!=gIcons.end()) return found->second.get();
    auto& icon=gIcons[key];
    std::size_t length=0; auto* resources=BApplication::AppResources();
    auto* data=resources?static_cast<const uint8*>(resources->LoadResource('VICN',(std::string("Mail:icon:")+name).c_str(),&length)):nullptr;
    if(!data) return nullptr;
    float pixels=IconSize(size);
    auto bitmap=std::make_unique<BBitmap>(BRect(0,0,pixels-1,pixels-1),B_RGBA32);
    if(bitmap->InitCheck()==B_OK && BIconUtils::GetVectorIcon(data,length,bitmap.get())==B_OK) icon=std::move(bitmap);
    return icon.get();
}
}
