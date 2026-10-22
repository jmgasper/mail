#include "Emoji.h"
#include "GraphemeEditing.h"
#include "EmojiShaping.h"
#include <Application.h>
#include <Clipboard.h>
#include <OS.h>
#include <Resources.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace mail::ui {
namespace {
struct Typeface {
    BFont font;
    bool available=false;
    std::mutex mutex;
    std::unordered_map<uint32,bool> glyphs;
    Typeface()
    {
        auto* resources=BApplication::AppResources(); if (!resources) return;
        std::size_t size=0; const void* data=resources->LoadResource('RAWT',"Mail:NotoEmoji",&size);
        if (!data || !size || size>8*1024*1024) return;
        void* bytes=nullptr;
        auto rounded=(size+B_PAGE_SIZE-1)&~(std::size_t(B_PAGE_SIZE)-1);
        auto area=create_area("Mail emoji font",&bytes,B_ANY_ADDRESS,rounded,B_NO_LOCK,B_READ_AREA|B_WRITE_AREA|B_CLONEABLE_AREA);
        if (area<0) return;
        std::memcpy(bytes,data,size); available=font.LoadFont(area,size,0)==B_OK;
        // app_server copies the font data. Its private font lives for the
        // application's lifetime, and is reclaimed when the application exits.
        delete_area(area);
    }
};
Typeface& typeface() { static Typeface value; return value; }
struct Character { uint32 code; int32 size; };
Character character(const char* text,int32 available)
{
    auto first=static_cast<uint8>(text[0]);
    if (first<0x80) return {first,1};
    int32 size=first>=0xc2 && first<=0xdf?2:(first>=0xe0 && first<=0xef?3:(first>=0xf0 && first<=0xf4?4:1));
    if (size==1 || size>available) return {0xfffd,1};
    uint32 code=first&((1u<<(7-size))-1);
    for (int32 i=1;i<size;++i) {
        auto byte=static_cast<uint8>(text[i]); if ((byte&0xc0)!=0x80) return {0xfffd,1};
        code=(code<<6)|(byte&0x3f);
    }
    if ((size==2 && code<0x80) || (size==3 && code<0x800) || (size==4 && code<0x10000)
        || (code>=0xd800 && code<=0xdfff) || code>0x10ffff) return {0xfffd,1};
    return {code,size};
}
bool emoji(const char* bytes,Character c)
{
    // Limit font queries and the cache to the symbol ranges used by this font.
    // ASCII digits retain the text font. Native sequence shaping is separate.
    bool candidate=c.code==0xa9 || c.code==0xae || (c.code>=0x2000 && c.code<=0x2bff)
        || c.code==0x3030 || c.code==0x303d || c.code==0x3297 || c.code==0x3299
        || c.code==0xfe0e || c.code==0xfe0f || (c.code>=0x1f000 && c.code<=0x1faff)
        || (c.code>=0xe0020 && c.code<=0xe007f);
    if (!candidate) return false;
    auto& face=typeface(); if (!face.available) return false;
    std::lock_guard<std::mutex> lock(face.mutex);
    auto found=face.glyphs.find(c.code); if (found!=face.glyphs.end()) return found->second;
    char text[5]={}; std::memcpy(text,bytes,c.size); bool has=false;
    face.font.GetHasGlyphs(text,1,&has,false); face.glyphs.emplace(c.code,has); return has;
}
BFont textFont(const BFont& source)
{
    if (!IsEmojiFont(source)) return source;
    BFont font(*be_plain_font); font.SetSize(source.Size()); SetTextFace(font,TextFace(source)); return font;
}
struct Piece { int32 start,end; bool emoji; std::optional<BFont> shaped; };
std::vector<Piece> pieces(const char* text,int32 size)
{
    std::vector<Piece> result;
    TextClusters clusters(std::string_view(text,size));const auto& bounds=clusters.Boundaries();
    for(std::size_t i=1;i<bounds.size();++i) {
        int32 begin=bounds[i-1],end=bounds[i];auto shaped=EmojiSequenceFont(std::string_view(text+begin,end-begin));
        if(shaped){
            auto first=character(text+begin,end-begin);
            result.push_back({begin,begin+first.size,true,shaped});
            result.push_back({begin+first.size,end,true,EmojiContinuationFont()});continue;
        }
        for(int32 pos=begin;pos<end;){
            auto c=character(text+pos,end-pos);bool use=emoji(text+pos,c);
            if(!result.empty() && !result.back().shaped && result.back().emoji==use)result.back().end=pos+c.size;
            else result.push_back({pos,pos+c.size,use,{}});
            pos+=c.size;
        }
    }
    return result;
}
BFont pieceFont(const Piece& part,const BFont& source)
{
    if(!part.shaped)return part.emoji?EmojiFont(source):textFont(source);
    auto font=*part.shaped;font.SetSize(source.Size());SetTextFace(font,TextFace(source));return font;
}
}
bool EmojiFontAvailable() { return typeface().available; }
bool IsEmojiFont(const BFont& font) { return (typeface().available && font.FamilyAndStyle()==typeface().font.FamilyAndStyle()) || IsShapedEmojiFont(font); }
uint16 TextFace(const BFont& font)
{
    uint16 face=font.Face();
    if (IsEmojiFont(font)) {
        if (font.FalseBoldWidth()>0) face|=B_BOLD_FACE;
        if (font.Shear()!=90) face|=B_ITALIC_FACE;
    }
    return face;
}
void SetTextFace(BFont& font,uint16 face)
{
    if (IsEmojiFont(font)) {
        // Use the font's designed weight. A fixed synthetic stroke overwhelmed
        // the details of joined emoji at normal header/menu sizes.
        auto styled=StyledEmojiFont(font,face&B_BOLD_FACE);
        if(styled)font=*styled;
        else font.SetFamilyAndStyle(typeface().font.FamilyAndStyle());
        auto identity=font.FamilyAndStyle();
        font.SetFace((styled && (face&B_BOLD_FACE)?B_BOLD_FACE:B_REGULAR_FACE)|(face&B_UNDERSCORE_FACE));
        // Sequence glyphs share a family. SetFace can choose another regular
        // style, so restore this glyph's style while retaining decorations.
        if(font.FamilyAndStyle()!=identity)font.SetFamilyAndStyle(identity);
        font.SetFalseBoldWidth(!styled && (face&B_BOLD_FACE)?font.Size()/100.f:0);
        // Noto Emoji has no italic face; native shear preserves that semantic.
        font.SetShear(face&B_ITALIC_FACE?70:90);
    } else {
        face&=~B_REGULAR_FACE;
        if (!(face&(B_BOLD_FACE|B_ITALIC_FACE))) face|=B_REGULAR_FACE;
        font.SetFace(face);
    }
}
BFont EmojiFont(const BFont& source)
{
    if (!EmojiFontAvailable()) return source;
    BFont font(typeface().font); font.SetSize(source.Size()); SetTextFace(font,TextFace(source)); return font;
}
void ApplyEmojiFonts(BTextView& view,int32 begin,int32 end)
{
    if(!dynamic_cast<EmojiTextView*>(&view))InstallGraphemeEditing(view);
    if (!EmojiFontAvailable()) return;
    begin=std::clamp(begin,int32(0),view.TextLength());
    end=end<0?view.TextLength():std::clamp(end,begin,view.TextLength());
    // An inserted scalar can join an existing sequence, and a regional
    // indicator can change subsequent flag pairing. Revisit its paragraph.
    while(begin>0 && view.ByteAt(begin-1)!='\n')--begin;
    while(end<view.TextLength() && view.ByteAt(end)!='\n')++end;
    if (begin==end) return;
    std::unique_ptr<text_run_array,decltype(&BTextView::FreeRunArray)> old(view.RunArray(begin,end),BTextView::FreeRunArray);
    if (!old) return;
    std::vector<text_run> result; bool changed=false;
    int32 i=0;
    for(const auto& part:pieces(view.Text()+begin,end-begin)) {
        for(int32 at=part.start;at<part.end;){
            while(i+1<old->count && old->runs[i+1].offset<=at)++i;
            const auto& original=old->runs[i];
            text_run run=original;run.offset=at;run.font=pieceFont(part,original.font);
            changed=changed || run.font!=original.font;
            if (result.empty() || result.back().font!=run.font || result.back().color!=run.color) result.push_back(run);
            at=i+1<old->count?std::min(part.end,old->runs[i+1].offset):part.end;
        }
    }
    if (!changed || result.empty()) return;
    std::unique_ptr<text_run_array,decltype(&BTextView::FreeRunArray)> runs(BTextView::AllocRunArray(result.size()),BTextView::FreeRunArray);
    if (!runs) return;
    for (std::size_t i=0;i<result.size();++i) runs->runs[i]=result[i];
    view.SetStylable(true); view.SetRunArray(begin,end,runs.get());
}
EmojiTextView::EmojiTextView(const char* name) : BTextView(name),fGraphemeEditing(std::make_unique<GraphemeEditing>(*this)) { SetStylable(true); }
EmojiTextView::~EmojiTextView()=default;
void EmojiTextView::SnapToGraphemes(){fGraphemeEditing->Normalize();}
int32 EmojiTextView::GraphemeStart(int32 at){return fGraphemeEditing->Clusters().Floor(at);}
int32 EmojiTextView::GraphemeEnd(int32 at){return fGraphemeEditing->Clusters().Ceil(at);}
bool EmojiTextView::CanEndLine(int32 at)
{ return BTextView::CanEndLine(at) && (at==TextLength() || fGraphemeEditing->Clusters().IsBoundary(at+1)); }
void EmojiTextView::KeyDown(const char* text,int32 length)
{
    if(fGraphemeEditing->KeyDown(text,length))return;
    BTextView::KeyDown(text,length);
    int direction=length && (text[0]==B_LEFT_ARROW || text[0]==B_UP_ARROW)?-1:1;
    fGraphemeEditing->Normalize(direction);
}
void EmojiTextView::MouseDown(BPoint point){BTextView::MouseDown(point);SnapToGraphemes();}
void EmojiTextView::MouseUp(BPoint point){BTextView::MouseUp(point);SnapToGraphemes();}
void EmojiTextView::Cut(BClipboard* clipboard){SnapToGraphemes();BTextView::Cut(clipboard);}
void EmojiTextView::Paste(BClipboard* clipboard){SnapToGraphemes();BTextView::Paste(clipboard);}
void EmojiTextView::InsertText(const char* text,int32 length,int32 at,const text_run_array* runs)
{
    fGraphemeEditing->Invalidate();
    BTextView::InsertText(text,length,at,runs); ApplyEmojiFonts(*this,at,at+length);
}
void EmojiTextView::DeleteText(int32 begin,int32 end)
{ fGraphemeEditing->Invalidate();BTextView::DeleteText(begin,end);ApplyEmojiFonts(*this,begin,begin); }
void CopyEmojiText(BTextView& view,BClipboard* clipboard,bool cut)
{
    int32 begin,end;view.GetSelection(&begin,&end);if(begin==end)return;
    std::string copied(view.Text()+begin,end-begin);int32 size=0;
    std::unique_ptr<text_run_array,decltype(&BTextView::FreeRunArray)> runs(view.RunArray(begin,end,&size),BTextView::FreeRunArray);
    if(cut)view.BTextView::Cut(clipboard);else view.BTextView::Copy(clipboard);
    if(!clipboard->Lock())return;
    auto* data=clipboard->Data(); const void* plain=nullptr; ssize_t length=0;
    if (data && data->FindData("text/plain",B_MIME_TYPE,&plain,&length)==B_OK
        && length==end-begin && !std::memcmp(plain,copied.data(),length)) {
        if (runs) {
            // Other applications cannot use our private font IDs. Export
            // standard native text fonts with the same semantic formatting.
            for (int32 i=0;i<runs->count;++i) runs->runs[i].font=textFont(runs->runs[i].font);
            data->RemoveName("application/x-vnd.Be-text_run_array");
            data->AddData("application/x-vnd.Be-text_run_array",B_MIME_TYPE,runs.get(),size); clipboard->Commit();
        }
    }
    clipboard->Unlock();
}
void EmojiTextView::Copy(BClipboard* clipboard){SnapToGraphemes();CopyEmojiText(*this,clipboard);}
void DrawEmojiText(BView& view,const std::string& text,const BFont& font,BPoint point,float width)
{
    BFont normal=textFont(font);auto parts=pieces(text.data(),text.size());std::vector<BFont> fonts;
    for(const auto& part:parts)fonts.push_back(pieceFont(part,font));
    auto measure=[&](int32 end){
        float result=0;
        for(std::size_t i=0;i<parts.size() && parts[i].start<end;++i)
            result+=fonts[i].StringWidth(text.data()+parts[i].start,std::min(end,parts[i].end)-parts[i].start);
        return result;
    };
    float total=measure(text.size()),ellipsis=normal.StringWidth("…");int32 kept=text.size();
    if(total>width){
        TextClusters clusters(text);const auto& boundaries=clusters.Boundaries();std::size_t low=0,high=boundaries.size();
        while(low+1<high){auto middle=(low+high)/2;if(measure(boundaries[middle])<=width-ellipsis)low=middle;else high=middle;}
        kept=boundaries[low];
    }
    for(std::size_t i=0;i<parts.size() && parts[i].start<kept;++i){
        auto length=std::min(kept,parts[i].end)-parts[i].start;view.SetFont(&fonts[i]);
        view.DrawString(text.data()+parts[i].start,length,point);point.x+=fonts[i].StringWidth(text.data()+parts[i].start,length);
    }
    if (total>width && width>=ellipsis) { view.SetFont(&normal); view.DrawString("…",point); }
    view.SetFont(&font);
}
void EmojiStringView::Draw(BRect)
{
    if (!Text()) return;
    if (LowUIColor()==B_NO_COLOR) SetLowColor(ViewColor());
    BFont font; GetFont(&font); font_height height; font.GetHeight(&height);
    std::string text=Text(); for (char& c:text) if (c=='\n' || c=='\r') c=' ';
    DrawEmojiText(*this,text,font,BPoint(Bounds().left,Bounds().bottom-std::ceil(height.descent)),Bounds().Width());
}
}
