#include "InlineImages.h"
#include "AttachmentViews.h"
#include <Application.h>
#include <DataIO.h>
#include <Message.h>
#include <OS.h>
#include <Resources.h>
#include <TranslatorRoster.h>
#include <TranslatorFormats.h>
#include <View.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <stdexcept>

namespace mail::ui {
namespace {
uint16 read16(const std::string& s,std::size_t p) {return (uint16(uint8(s.at(p)))<<8)|uint8(s.at(p+1));}
uint32 read32(const std::string& s,std::size_t p) {return (uint32(read16(s,p))<<16)|read16(s,p+2);}
void put16(std::string& s,std::size_t p,uint16 v) {s.at(p)=v>>8;s.at(p+1)=v;}
void put32(std::string& s,std::size_t p,uint32 v) {put16(s,p,v>>16);put16(s,p+2,v);}
uint32 checksum(const std::string& s,std::size_t at,std::size_t length)
{
    uint32 sum=0;for(std::size_t i=0;i<length;i+=4) {uint32 word=0;for(std::size_t j=0;j<4;++j)word=(word<<8)|(i+j<length?uint8(s.at(at+i+j)):0);sum+=word;}return sum;
}
std::unique_ptr<BBitmap> thumbnail(const Attachment& file,const std::atomic<bool>& canceled)
{
    // Automatic composition previews accept self-contained raster containers.
    // Do not pass SVG, documents or an arbitrary named image to a translator
    // that might follow references. MIME labels alone are not sufficient.
    const auto& data=file.data;uint32 type=0;
    if(data.compare(0,8,"\x89PNG\r\n\x1a\n",8)==0)type=B_PNG_FORMAT;
    else if(data.compare(0,3,"\xff\xd8\xff",3)==0)type=B_JPEG_FORMAT;
    else if(data.compare(0,6,"GIF87a")==0 || data.compare(0,6,"GIF89a")==0)type=B_GIF_FORMAT;
    else if(data.compare(0,2,"BM")==0)type=B_BMP_FORMAT;
    else if(data.compare(0,4,"II*\0",4)==0 || data.compare(0,4,"MM\0*",4)==0)type=B_TIFF_FORMAT;
    else if(data.size()>=12 && data.compare(0,4,"RIFF")==0 && data.compare(8,4,"WEBP")==0)type=B_WEBP_FORMAT;
    else if(data.compare(0,4,"\0\0\1\0",4)==0 || data.compare(0,4,"\0\0\2\0",4)==0)type='ICO ';
    if(!type)throw std::runtime_error("This image retains its named marker.");
    BMemoryIO input(data.data(),data.size());translator_info info{};
    if(BTranslatorRoster::Default()->Identify(&input,nullptr,&info,type,nullptr,B_TRANSLATOR_BITMAP)!=B_OK || info.type!=type)
        throw std::runtime_error("This raster format is unavailable.");
    auto source=DecodeAttachmentImage(file,canceled);
    auto bounds=source->Bounds();float w=bounds.Width()+1,h=bounds.Height()+1;
    float scale=std::min({1.f,512.f/w,320.f/h});
    int width=std::max(1,int(std::floor(w*scale))),height=std::max(1,int(std::floor(h*scale)));
    auto result=std::make_unique<BBitmap>(BRect(0,0,width-1,height-1),B_BITMAP_ACCEPTS_VIEWS,B_RGBA32);
    if(result->InitCheck()!=B_OK || !result->Lock())throw std::runtime_error("Could not allocate image thumbnail.");
    auto* view=new BView(result->Bounds(),"image thumbnail",B_FOLLOW_NONE,B_WILL_DRAW);
    result->AddChild(view);view->SetDrawingMode(B_OP_COPY);view->SetHighColor(0,0,0,0);view->FillRect(view->Bounds());
    view->DrawBitmap(source.get(),bounds,view->Bounds(),B_FILTER_BITMAP_BILINEAR);view->Sync();
    result->RemoveChild(view);delete view;result->Unlock();return result;
}
}
InlineObjectFont::InlineObjectFont(uint16 advance)
{
    std::size_t length=0;auto* resources=BApplication::AppResources();
    const void* data=resources?resources->LoadResource('RAWT',"Mail:InlineObject",&length):nullptr;
    if(!data || length<64 || length>65536)throw std::runtime_error("Inline image font is unavailable.");
    std::string bytes(static_cast<const char*>(data),length);
    static std::atomic<uint64> next{0};char identity[17];std::snprintf(identity,sizeof(identity),"%016llx",static_cast<unsigned long long>(++next));
    std::size_t head=0;int count=read16(bytes,4);
    for(int i=0;i<count;++i) {
        auto row=12+i*16;auto at=read32(bytes,row+8),size=read32(bytes,row+12);
        if(at>bytes.size() || size>bytes.size()-at)throw std::runtime_error("Invalid inline image font.");
        auto tag=bytes.substr(row,4);
        if(tag=="head") {head=at;put32(bytes,at+8,0);}
        else if(tag=="hmtx")put16(bytes,at+8,advance);
        else if(tag=="hhea")put16(bytes,at+10,advance);
        else if(tag=="name") {
            for(const bool wide:{false,true}) {
                std::string from,to;for(int j=0;j<16;++j){if(wide){from+='\0';to+='\0';}from+='0';to+=identity[j];}
                for(std::size_t p=at;(p=bytes.find(from,p))!=std::string::npos && p+from.size()<=at+size;p+=to.size())bytes.replace(p,from.size(),to);
            }
        }
        put32(bytes,row+4,checksum(bytes,at,size));
    }
    if(!head)throw std::runtime_error("Invalid inline image metrics.");
    put32(bytes,head+8,0xb1b0afba-checksum(bytes,0,bytes.size()));
    void* memory=nullptr;auto rounded=(bytes.size()+B_PAGE_SIZE-1)&~std::size_t(B_PAGE_SIZE-1);
    auto area=create_area("Mail inline image metrics",&memory,B_ANY_ADDRESS,rounded,B_NO_LOCK,B_READ_AREA|B_WRITE_AREA|B_CLONEABLE_AREA);
    if(area<0)throw std::runtime_error("Could not allocate inline image metrics.");
    std::memcpy(memory,bytes.data(),bytes.size());auto status=fFont.LoadFont(area,bytes.size(),0);delete_area(area);
    if(status!=B_OK)throw std::runtime_error("Could not load inline image metrics.");
    fLoaded=true;
}
InlineObjectFont::~InlineObjectFont() {if(fLoaded)fFont.UnloadFont();}
InlineImageCache::~InlineImageCache()
{
    {std::lock_guard<std::mutex> lock(fMutex);fStop=true;for(auto& item:fEntries)item.second->canceled=true;}
    fWake.notify_all();if(fWorker.joinable())fWorker.join();
}
void InlineImageCache::Attach(BMessenger target)
{
    if(fAttached)return;
    fTarget=target;fAttached=true;fWorker=std::thread([this]{Work();});
}
void InlineImageCache::Sync(const std::vector<std::shared_ptr<const Attachment>>& files,const std::vector<std::shared_ptr<const Attachment>>& retained)
{
    if(!fAttached)return;
    std::set<const Attachment*> keep;for(const auto& file:retained)keep.insert(file.get());
    std::lock_guard<std::mutex> lock(fMutex);
    for(auto it=fEntries.begin();it!=fEntries.end();) {
        if(!keep.count(it->first)){it->second->canceled=true;it=fEntries.erase(it);}else ++it;
    }
    fPending.erase(std::remove_if(fPending.begin(),fPending.end(),[](const auto& entry){return entry->canceled.load();}),fPending.end());
    for(const auto& file:files)if(!fEntries.count(file.get())) {
        auto entry=std::make_shared<Entry>();entry->file=file;fEntries[file.get()]=entry;fPending.push_back(entry);
    }
    fWake.notify_one();
}
const BBitmap* InlineImageCache::Bitmap(const Attachment* file) const
{
    auto found=fEntries.find(file);return found!=fEntries.end() && found->second->ready?found->second->bitmap.get():nullptr;
}
void InlineImageCache::Work()
{
    for(;;) {
        std::shared_ptr<Entry> entry;
        {std::unique_lock<std::mutex> lock(fMutex);fWake.wait(lock,[&]{return fStop || !fPending.empty();});if(fStop)return;entry=fPending.front();fPending.pop_front();}
        if(entry->canceled)continue;
        try {entry->bitmap=thumbnail(*entry->file,entry->canceled);}catch(const std::exception&) { /* Keep the named marker when decoding is unavailable. */ }
        entry->ready=true;
        if(!entry->canceled){BMessage ready(kInlineImagesReady);fTarget.SendMessage(&ready,static_cast<BHandler*>(nullptr),0);}
    }
}
}
