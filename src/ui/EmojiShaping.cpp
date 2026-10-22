#include "EmojiShaping.h"
#include <Application.h>
#include <OS.h>
#include <Resources.h>
#include <hb.h>
#include <hb-ot.h>
#include <hb-subset.h>
#include <unicode/uchar.h>
#include <unicode/utf8.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mail::ui {
namespace {
template<class T,void(*Destroy)(T*)>using Owned=std::unique_ptr<T,decltype(Destroy)>;
using Blob=Owned<hb_blob_t,hb_blob_destroy>;
using Face=Owned<hb_face_t,hb_face_destroy>;
using Font=Owned<hb_font_t,hb_font_destroy>;
using Input=Owned<hb_subset_input_t,hb_subset_input_destroy>;
using Plan=Owned<hb_subset_plan_t,hb_subset_plan_destroy>;
uint16 get16(const std::string& s,std::size_t p){return (uint16(uint8(s.at(p)))<<8)|uint8(s.at(p+1));}
void put16(std::string& s,std::size_t p,uint16 v){s.at(p)=v>>8;s.at(p+1)=v;}
void put32(std::string& s,std::size_t p,uint32 v){put16(s,p,v>>16);put16(s,p+2,v);}
std::string table(hb_face_t* face,hb_tag_t tag)
{
    Blob blob(hb_face_reference_table(face,tag),hb_blob_destroy);unsigned length=0;
    const auto* bytes=hb_blob_get_data(blob.get(),&length);return length?std::string(bytes,length):std::string();
}
void addTable(hb_face_t* face,hb_tag_t tag,const std::string& data)
{
    Blob blob(hb_blob_create(data.data(),data.size(),HB_MEMORY_MODE_DUPLICATE,nullptr,nullptr),hb_blob_destroy);
    if(hb_blob_get_length(blob.get())!=data.size() || !hb_face_builder_add_table(face,tag,blob.get()))
        throw std::runtime_error("Could not build an emoji font table.");
}
std::string renamed(const std::string& source,const std::string& style)
{
    auto count=get16(source,2),strings=get16(source,4);std::string result(6+12*count,'\0'),values;
    put16(result,2,count);put16(result,4,result.size());
    const std::string family="Mail Emoji Layout";
    for(uint16 i=0;i<count;++i){
        std::size_t row=6+12*i;auto platform=get16(source,row),encoding=get16(source,row+2),id=get16(source,row+6);
        auto size=get16(source,row+8),at=get16(source,row+10);
        if(std::size_t(strings)+at+size>source.size())throw std::runtime_error("Invalid embedded emoji names.");
        std::string value=source.substr(strings+at,size),replacement;
        if(id==1 || id==16)replacement=family;
        else if(id==3 || id==4 || id==6 || id==18)replacement=family+' '+style;
        else if(id==2 || id==17)replacement=style;
        if(!replacement.empty()){
            if(id==6)replacement.erase(std::remove(replacement.begin(),replacement.end(),' '),replacement.end());
            value.clear();for(char c:replacement){if(platform==0 || (platform==3 && (encoding==0 || encoding==1 || encoding==10)))value+='\0';value+=c;}
        }
        if(values.size()+value.size()>65535)throw std::runtime_error("Emoji font names are too large.");
        result.replace(row,8,source,row,8);put16(result,row+8,value.size());put16(result,row+10,values.size());values+=value;
    }
    return result+values;
}
std::string styledTable(hb_tag_t tag,std::string data,const std::string& style,bool bold)
{
    if(tag==HB_TAG('n','a','m','e'))return renamed(data,style);
    // Static instances need ordinary bold metadata so BFont.Face() retains
    // semantic formatting without synthesizing an expanded outline.
    if(tag==HB_TAG('h','e','a','d'))put16(data,44,(get16(data,44)&~1u)|(bold?1u:0u));
    if(tag==HB_TAG('O','S','/','2')){
        put16(data,4,bold?700:400);
        put16(data,62,(get16(data,62)&~0x60u)|(bold?0x20u:0x40u));
    }
    return data;
}
std::string fontBytes(hb_face_t* face)
{
    Blob blob(hb_face_reference_blob(face),hb_blob_destroy);unsigned size=0;auto* bytes=hb_blob_get_data(blob.get(),&size);
    if(!size || size>8*1024*1024)throw std::runtime_error("Emoji glyph data is unavailable.");
    return {bytes,size};
}
std::string textFont(hb_face_t* source,bool bold)
{
    Face builder(hb_face_builder_create(),hb_face_destroy);unsigned count=hb_face_get_table_tags(source,0,nullptr,nullptr);
    std::vector<hb_tag_t> tags(count);hb_face_get_table_tags(source,0,&count,tags.data());
    for(auto tag:tags)addTable(builder.get(),tag,styledTable(tag,table(source,tag),bold?"Text Bold":"Text Regular",bold));
    return fontBytes(builder.get());
}
std::string glyphFont(hb_face_t* source,uint32 glyph,bool blank,bool bold)
{
    Input input(hb_subset_input_create_or_fail(),hb_subset_input_destroy);
    if(!input)throw std::runtime_error("Could not prepare an emoji glyph.");
    hb_set_add(hb_subset_input_glyph_set(input.get()),glyph);
    hb_set_add_range(hb_subset_input_set(input.get(),HB_SUBSET_SETS_NAME_ID),0,65535);
    for(auto tag:{HB_TAG('G','S','U','B'),HB_TAG('G','P','O','S'),HB_TAG('G','D','E','F')})
        hb_set_add(hb_subset_input_set(input.get(),HB_SUBSET_SETS_DROP_TABLE_TAG),tag);
    Plan plan(hb_subset_plan_create_or_fail(source,input.get()),hb_subset_plan_destroy);
    if(!plan)throw std::runtime_error("Could not plan an emoji glyph.");
    auto mapped=hb_map_get(hb_subset_plan_old_to_new_glyph_mapping(plan.get()),glyph);
    Face subset(hb_subset_plan_execute_or_fail(plan.get()),hb_face_destroy);
    if(!subset || mapped==HB_MAP_VALUE_INVALID)throw std::runtime_error("Could not subset an emoji glyph.");
    Face builder(hb_face_builder_create(),hb_face_destroy);unsigned count=hb_face_get_table_tags(subset.get(),0,nullptr,nullptr);
    std::vector<hb_tag_t> tags(count);hb_face_get_table_tags(subset.get(),0,&count,tags.data());
    for(auto tag:tags){
        if(tag==HB_TAG('c','m','a','p'))continue;
        auto data=table(subset.get(),tag);
        data=styledTable(tag,std::move(data),"Glyph "+std::to_string(blank?0:glyph)+(bold?" Bold":" Regular"),bold);
        if(blank && tag==HB_TAG('h','m','t','x')){
            auto metrics=get16(table(subset.get(),HB_TAG('h','h','e','a')),34);
            if(!metrics)throw std::runtime_error("Invalid emoji glyph metrics.");
            put16(data,4*std::min(mapped,uint32(metrics-1)),0);
        }
        if(blank && tag==HB_TAG('h','h','e','a'))put16(data,10,0);
        addTable(builder.get(),tag,data);
    }
    // Format 13 maps every scalar to one glyph. Only the first scalar of the
    // cluster uses this face; all the others use the separate blank face.
    std::string cmap(40,'\0');put16(cmap,2,1);put16(cmap,4,3);put16(cmap,6,10);put32(cmap,8,12);
    put16(cmap,12,13);put32(cmap,16,28);put32(cmap,24,1);put32(cmap,32,0x10ffff);put32(cmap,36,mapped);
    addTable(builder.get(),HB_TAG('c','m','a','p'),cmap);
    return fontBytes(builder.get());
}
BFont load(const std::string& data)
{
    void* bytes=nullptr;auto rounded=(data.size()+B_PAGE_SIZE-1)&~std::size_t(B_PAGE_SIZE-1);
    auto area=create_area("Mail shaped emoji",&bytes,B_ANY_ADDRESS,rounded,B_NO_LOCK,B_READ_AREA|B_WRITE_AREA|B_CLONEABLE_AREA);
    if(area<0)throw std::runtime_error("Could not allocate an emoji glyph.");
    std::memcpy(bytes,data.data(),data.size());BFont font;auto status=font.LoadFont(area,data.size(),0);delete_area(area);
    if(status!=B_OK)throw std::runtime_error("Could not load an emoji glyph.");
    return font;
}
class Shaper {
public:
    Shaper():fOriginal(nullptr,hb_face_destroy),fFaces{Face(nullptr,hb_face_destroy),Face(nullptr,hb_face_destroy)},fFont(nullptr,hb_font_destroy)
    {
        auto* resources=BApplication::AppResources();std::size_t size=0;
        const auto* data=resources?resources->LoadResource('RAWT',"Mail:NotoEmoji",&size):nullptr;
        if(!data || !size || size>8*1024*1024)return;
        Blob blob(hb_blob_create(static_cast<const char*>(data),size,HB_MEMORY_MODE_DUPLICATE,nullptr,nullptr),hb_blob_destroy);
        fOriginal.reset(hb_face_create(blob.get(),0));
        auto* regular=WeightFace(false);fLimit=hb_face_get_glyph_count(regular);
        if(!fLimit || fLimit>8192){fOriginal.reset();return;}
        fFont.reset(hb_font_create(regular));hb_ot_font_set_funcs(fFont.get());hb_font_make_immutable(fFont.get());
    }
    std::optional<BFont> Shape(std::string_view cluster)
    {
        if(!fFont || cluster.size()>256 || cluster.empty())return {};
        int32 at=0;UChar32 first;U8_NEXT(cluster.data(),at,int32(cluster.size()),first);
        if(first<0 || at==int32(cluster.size()) || !u_hasBinaryProperty(first,UCHAR_EMOJI))return {};
        for(int32 pos=at;pos<int32(cluster.size());){UChar32 code;U8_NEXT(cluster.data(),pos,int32(cluster.size()),code);if(code<0 || code==0xfe0e)return {};}
        Owned<hb_buffer_t,hb_buffer_destroy> buffer(hb_buffer_create(),hb_buffer_destroy);
        hb_buffer_add_utf8(buffer.get(),cluster.data(),cluster.size(),0,cluster.size());
        hb_buffer_set_direction(buffer.get(),HB_DIRECTION_LTR);hb_buffer_set_script(buffer.get(),HB_SCRIPT_COMMON);
        hb_buffer_set_flags(buffer.get(),HB_BUFFER_FLAG_REMOVE_DEFAULT_IGNORABLES);
        hb_shape(fFont.get(),buffer.get(),nullptr,0);unsigned count=0;
        const auto* info=hb_buffer_get_glyph_infos(buffer.get(),&count);
        const auto* position=hb_buffer_get_glyph_positions(buffer.get(),nullptr);
        if(count!=1 || !info[0].codepoint || position[0].x_advance<=0 || position[0].x_offset || position[0].y_offset || position[0].y_advance)return {};
        std::lock_guard<std::mutex> lock(fMutex);Glyph(0,false);return Glyph(info[0].codepoint,false);
    }
    BFont Blank(){std::lock_guard<std::mutex> lock(fMutex);return Glyph(0,false);}
    BFont Style(const BFont& source,bool bold)
    {
        std::lock_guard<std::mutex> lock(fMutex);
        auto found=fIDs.find(source.FamilyAndStyle());
        auto variant=Glyph(found==fIDs.end()?fLimit:found->second,bold);
        BFont font(source);font.SetFamilyAndStyle(variant.FamilyAndStyle());return font;
    }
    bool Contains(const BFont& font){std::lock_guard<std::mutex> lock(fMutex);return fIDs.count(font.FamilyAndStyle());}
    EmojiFontCacheInfo Info(){std::lock_guard<std::mutex> lock(fMutex);return {fFonts.size(),fBytes,2*(fLimit+1)};}
private:
    hb_face_t* WeightFace(bool bold)
    {
        auto& face=fFaces[bold?1:0];if(face)return face.get();
        Input input(hb_subset_input_create_or_fail(),hb_subset_input_destroy);
        if(!input || !fOriginal)throw std::runtime_error("Emoji weight is unavailable.");
        hb_subset_input_keep_everything(input.get());
        if(!hb_subset_input_pin_all_axes_to_default(input.get(),fOriginal.get())
            || !hb_subset_input_pin_axis_location(input.get(),fOriginal.get(),HB_TAG('w','g','h','t'),bold?700:400))
            throw std::runtime_error("Could not select the emoji weight.");
        face.reset(hb_subset_or_fail(fOriginal.get(),input.get()));
        if(!face)throw std::runtime_error("Could not build the emoji weight.");
        return face.get();
    }
    BFont Glyph(uint32 glyph,bool bold)
    {
        uint32 key=2*glyph+bold;auto found=fFonts.find(key);if(found!=fFonts.end())return found->second;
        if(!fFont || glyph>fLimit)throw std::runtime_error("Invalid shaped emoji glyph.");
        hb_codepoint_t index=glyph;
        if(!glyph && !hb_font_get_nominal_glyph(fFont.get(),0x20,&index))throw std::runtime_error("Emoji continuation font is unavailable.");
        auto* face=WeightFace(bold);
        auto data=glyph==fLimit?textFont(face,bold):glyphFont(face,index,glyph==0,bold);
        auto font=load(data);fFonts.emplace(key,font);fIDs.emplace(font.FamilyAndStyle(),glyph);fBytes+=data.size();return font;
    }
    // One family with a style per glyph avoids Haiku's 128-user-family limit.
    // Keep native font IDs valid for BTextView's undo buffers and retained run
    // arrays until application exit. Each of the two designed weights has at
    // most one font per glyph (including blank) and one full scalar font.
    Face fOriginal;std::array<Face,2> fFaces;Font fFont;std::mutex fMutex;std::unordered_map<uint32,BFont> fFonts;
    std::unordered_map<uint32,uint32> fIDs;std::size_t fBytes=0,fLimit=0;
};
Shaper& shaper(){static Shaper value;return value;}
}
std::optional<BFont> EmojiSequenceFont(std::string_view cluster)
{ try{return shaper().Shape(cluster);}catch(const std::exception&){return {};}}
BFont EmojiContinuationFont(){return shaper().Blank();}
std::optional<BFont> StyledEmojiFont(const BFont& font,bool bold)
{try{return shaper().Style(font,bold);}catch(const std::exception&){return {};}}
bool IsShapedEmojiFont(const BFont& font){return shaper().Contains(font);}
EmojiFontCacheInfo EmojiShapingCacheInfo(){return shaper().Info();}
}
