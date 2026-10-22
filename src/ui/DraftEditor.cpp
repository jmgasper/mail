#include "DraftEditor.h"
#include "AttachmentViews.h"
#include "mail/Clipboard.h"
#include <Clipboard.h>
#include <Message.h>
#include <Messenger.h>
#include <ScrollBar.h>
#include <Window.h>
#include <algorithm>
#include <cmath>
#include <exception>
#include <set>
#include <stdexcept>

namespace mail::ui {
namespace {
std::string contentId() { auto id=newMessageId(); return id.substr(1,id.size()-2); }
bool contains(const InlineImage& image,std::size_t offset) { return offset>image.offset && offset<image.offset+image.length; }
BFont styleFont(const TextRun& run)
{
    BFont font(*be_plain_font);
    SetTextFace(font,(run.bold?B_BOLD_FACE:0)|(run.italic?B_ITALIC_FACE:0)|(run.underline?B_UNDERSCORE_FACE:0));
    return font;
}
// Mail's fragment preserves exact binary files and placement metadata. Portable
// HTML and native text/font runs remain available to other applications.
BMessage encodeFragment(const Draft& d)
{
    BMessage out; out.AddString("text",d.text.c_str());
    for(const auto& run:d.runs) {
        BMessage item; item.AddInt64("offset",run.offset); item.AddBool("bold",run.bold); item.AddBool("italic",run.italic);
        item.AddBool("underline",run.underline); item.AddString("link",run.link.c_str()); out.AddMessage("run",&item);
    }
    for(const auto& image:d.images) {
        BMessage item; item.AddInt64("offset",image.offset); item.AddInt64("length",image.length);
        item.AddString("cid",image.contentId.c_str()); out.AddMessage("image",&item);
    }
    for(const auto& file:d.attachments) {
        BMessage item; item.AddString("name",file.name.c_str()); item.AddString("type",file.mediaType.c_str()); item.AddString("cid",file.contentId.c_str());
        if(!file.data.empty()) item.AddData("data",B_RAW_TYPE,file.data.data(),file.data.size());
        out.AddMessage("file",&item);
    }
    return out;
}
Draft decodeFragment(const BMessage& clip,const char* plain,ssize_t length)
{
    BMessage data; if(clip.FindMessage("mail:fragment",&data)!=B_OK || data.FlattenedSize()>24*1024*1024) throw std::invalid_argument("No Mail fragment");
    const char* text=nullptr;
    if(data.FindString("text",&text)!=B_OK || length<0 || std::string(plain,length)!=text) throw std::invalid_argument("Stale Mail fragment");
    Draft d; d.account=1; d.text=text;
    for(int32 i=0;;++i) {
        BMessage item; if(data.FindMessage("run",i,&item)!=B_OK) break;
        if(i>=10000) throw std::invalid_argument("Too many clipboard styles");
        int64 offset=-1; const char* link=""; item.FindInt64("offset",&offset); item.FindString("link",&link);
        d.runs.push_back({static_cast<std::size_t>(offset),item.GetBool("bold",false),item.GetBool("italic",false),item.GetBool("underline",false),link});
    }
    for(int32 i=0;;++i) {
        BMessage item; if(data.FindMessage("image",i,&item)!=B_OK) break;
        if(i>=500) throw std::invalid_argument("Too many clipboard images");
        int64 offset=-1,size=-1; const char* cid=""; item.FindInt64("offset",&offset); item.FindInt64("length",&size); item.FindString("cid",&cid);
        d.images.push_back({static_cast<std::size_t>(offset),static_cast<std::size_t>(size),cid});
    }
    std::size_t total=d.text.size();
    for(int32 i=0;;++i) {
        BMessage item; if(data.FindMessage("file",i,&item)!=B_OK) break;
        if(i>=100) throw std::invalid_argument("Too many clipboard files");
        const char *name="",*type="",*cid=""; const void* bytes=nullptr; ssize_t size=0;
        item.FindString("name",&name); item.FindString("type",&type); item.FindString("cid",&cid);
        item.FindData("data",B_RAW_TYPE,&bytes,&size);
        if(size<0 || static_cast<std::size_t>(size)>22*1024*1024-total) throw std::invalid_argument("Clipboard images are too large");
        total+=size; d.attachments.push_back({name,type,cid,size?std::string(static_cast<const char*>(bytes),size):std::string(),true});
    }
    validateDraft(d); return d;
}
}
class DraftEditor::Action {
public:
    explicit Action(DraftEditor& editor):fEditor(editor),fExceptions(std::uncaught_exceptions()) { fEditor.BeginAction(); }
    ~Action() { fEditor.EndAction(std::uncaught_exceptions()==fExceptions); }
private:
    DraftEditor& fEditor; int fExceptions;
};
DraftEditor::DraftEditor():EmojiTextView("compose body")
{
    SetDoesUndo(false); SetWordWrap(true); SetInsets(12,12,12,12); SetMaxBytes(2*1024*1024);
    SetToolTip("Click an image to select it; cut, copy or delete it with the message text. Images that cannot be displayed keep a named placeholder.");
}
DraftEditor::~DraftEditor() { RemoveImageLayout(); }
void DraftEditor::AttachedToWindow()
{
    EmojiTextView::AttachedToWindow();fImageCache.Attach(BMessenger(this));ApplyImageLayout();
}
BFont DraftEditor::ContentFont(int32 at) const
{
    for(const auto& layout:fImageLayout)if(at>=static_cast<int32>(layout.image.offset) && at<static_cast<int32>(layout.image.offset+layout.image.length))return layout.textFont;
    BFont font;GetFontAndColor(at,&font);return font;
}
void DraftEditor::RemoveImageLayout()
{
    if(!fImageScrollAnchor && Window() && (!fImages.empty() || !fImageLayout.empty())) {
        auto offset=OffsetAt(BPoint(Bounds().left,Bounds().top));
        for(const auto& image:fImages)if(contains(image,offset)){offset=image.offset;break;}
        fImageScrollAnchor=ScrollAnchor{offset,Bounds().top-PointAt(offset).y};
    }
    for(const auto& layout:fImageLayout)SetFontAndColor(layout.image.offset,layout.image.offset+layout.image.length,&layout.textFont);
    fImageLayout.clear();
}
void DraftEditor::ApplyImageLayout()
{
    if(fApplyingImageLayout || fActionDepth || fRestoring)return;
    fApplyingImageLayout=true;
    RemoveImageLayout();
    std::vector<std::shared_ptr<const Attachment>> files;
    for(const auto& file:fFiles)if(std::any_of(fImages.begin(),fImages.end(),[&](const auto& image){return image.contentId==file->contentId;}))files.push_back(file);
    fImageCache.Sync(files,fFiles);
    std::set<uint16> used;
    for(const auto& image:fImages) {
        auto file=std::find_if(files.begin(),files.end(),[&](const auto& file){return file->contentId==image.contentId;});
        if(file==files.end())continue;
        auto* bitmap=fImageCache.Bitmap(file->get());if(!bitmap)continue;
        auto bounds=bitmap->Bounds();float width=bounds.Width()+1,height=bounds.Height()+1;
        float scale=std::min(1.f,std::max(1.f,TextRect().Width()-12)/std::max(1.f,width));
        width=std::max(8.f,width*scale)+8;height=std::max(8.f,height*scale)+8;
        if(width>=TextRect().Width() || TextRect().Width()<24)continue;
        auto marker=std::string(Text()+image.offset,image.length);
        int brackets=std::count(marker.begin(),marker.end(),'[');if(!brackets)continue;
        auto advance=static_cast<uint16>(std::clamp(std::lround(width*1024/(height*brackets)),1L,65535L));
        try {
            auto& object=fObjectFonts[advance];if(!object)object=std::make_shared<InlineObjectFont>(advance);
            auto font=object->Font();font.SetSize(height);
            auto semantic=ContentFont(image.offset);width=font.StringWidth(marker.c_str());
            fImageLayout.push_back({image,semantic,object,file->get(),width,height});used.insert(advance);
            SetFontAndColor(image.offset,image.offset+image.length,&font);
        }catch(const std::exception&) { /* A named placeholder remains usable if private fonts are unavailable. */ }
    }
    for(auto it=fObjectFonts.begin();it!=fObjectFonts.end();) {if(!used.count(it->first))it=fObjectFonts.erase(it);else ++it;}
    if(fImageScrollAnchor) {
        auto anchor=*fImageScrollAnchor;fImageScrollAnchor.reset();
        if(auto* scroll=ScrollBar(B_VERTICAL))scroll->SetValue(PointAt(std::clamp(anchor.offset,0,TextLength())).y+anchor.delta);
    }
    fApplyingImageLayout=false;Invalidate();
}
void DraftEditor::FrameResized(float width,float height)
{ EmojiTextView::FrameResized(width,height);ApplyImageLayout(); }
bool DraftEditor::CanEndLine(int32 at)
{
    auto found=std::lower_bound(fImageLayout.begin(),fImageLayout.end(),at,[](const auto& layout,int32 offset){return static_cast<int32>(layout.image.offset+layout.image.length)-1<offset;});
    if(found!=fImageLayout.end()) {
        const auto& image=found->image;
        if(at==static_cast<int32>(image.offset)-1 || at==static_cast<int32>(image.offset+image.length)-1)return true;
        if(at>=static_cast<int32>(image.offset) && at<static_cast<int32>(image.offset+image.length))return false;
    }
    return EmojiTextView::CanEndLine(at);
}
BRect DraftEditor::InlineImageFrame(int32 offset) const
{
    auto found=std::find_if(fImageLayout.begin(),fImageLayout.end(),[&](const auto& layout){return layout.image.offset==static_cast<std::size_t>(offset);});
    if(found==fImageLayout.end())return BRect();
    auto point=PointAt(offset);int32 line=LineAt(offset),begin=OffsetAt(line),end=line+1<CountLines()?OffsetAt(line+1):TextLength();
    float ascent=0;
    if(auto* runs=RunArray(begin,end)) {
        for(int32 i=0;i<runs->count;++i){font_height height;runs->runs[i].font.GetHeight(&height);ascent=std::max(ascent,std::ceil(height.ascent));}FreeRunArray(runs);
    }
    return BRect(point.x,point.y+ascent-found->height+1,point.x+found->width-1,point.y+ascent);
}
void DraftEditor::Draw(BRect update)
{
    EmojiTextView::Draw(update);int32 begin,end;GetSelection(&begin,&end);PushState();
    for(const auto& layout:fImageLayout) {
        auto frame=InlineImageFrame(layout.image.offset);if(!frame.Intersects(update))continue;
        auto* bitmap=fImageCache.Bitmap(layout.file);if(!bitmap)continue;
        auto target=frame.InsetByCopy(4,4);auto source=bitmap->Bounds();
        float scale=std::min(target.Width()/(source.Width()+1),target.Height()/(source.Height()+1));
        BRect painted(0,0,std::max(1.f,(source.Width()+1)*scale)-1,std::max(1.f,(source.Height()+1)*scale)-1);
        painted.OffsetTo(target.LeftTop());SetDrawingMode(B_OP_ALPHA);SetBlendingMode(B_PIXEL_ALPHA,B_ALPHA_OVERLAY);
        DrawBitmap(bitmap,source,painted,B_FILTER_BITMAP_BILINEAR);SetDrawingMode(B_OP_COPY);
        if(begin<static_cast<int32>(layout.image.offset+layout.image.length) && end>static_cast<int32>(layout.image.offset)) {
            SetHighUIColor(B_KEYBOARD_NAVIGATION_COLOR);SetPenSize(2);StrokeRect(frame.InsetByCopy(1,1));
        }
    }
    PopState();
}
void DraftEditor::Load(const Draft& source)
{
    validateDraft(source); auto d=source;
    // Old drafts had only trailing inline files. Give those a visible, editable
    // position when opened, retaining their order and binary contents.
    for(const auto& file:d.attachments) if(file.inlineImage && std::none_of(d.images.begin(),d.images.end(),[&](const InlineImage& i){return i.contentId==file.contentId;})) {
        if(!d.text.empty() && d.text.back()!='\n') d.text+='\n';
        auto marker=imagePlaceholder(file); d.images.push_back({d.text.size(),marker.size(),file.contentId}); d.text+=marker; d.text+='\n';
    }
    validateDraft(d);
    State state; state.body=d; state.body.attachments.clear();
    for(auto& file:d.attachments) state.files.push_back(std::make_shared<const Attachment>(std::move(file)));
    Restore(state); fUndo.clear(); fRedo.clear(); fTypingKind=0;if(IsFocus())ScrollToSelection();
}
void DraftEditor::LoadBody(const Draft& d)
{
    fLinks.clear(); fImages.clear(); SetText(d.text.c_str());
    SetFontAndColor(0,TextLength(),be_plain_font);
    for(std::size_t i=0;i<d.runs.size();++i) {
        const auto& run=d.runs[i]; int32 end=i+1<d.runs.size()?d.runs[i+1].offset:d.text.size();
        auto font=styleFont(run); SetFontAndColor(run.offset,end,&font);
        if(!run.link.empty()) fLinks.push_back({static_cast<int32>(run.offset),end,run.link});
    }
    fImages=d.images; ApplyEmojiFonts(*this);
}
void DraftEditor::SaveBody(Draft& d)
{
    d.text.assign(Text(),TextLength()); d.images=fImages; d.runs.clear(); std::set<int32> offsets{0};
    if(!TextLength()) return;
    auto* array=RunArray(0,TextLength());
    if(array) { for(int32 i=0;i<array->count;++i) offsets.insert(array->runs[i].offset); FreeRunArray(array); }
    for(const auto& link:fLinks) { offsets.insert(link.begin); offsets.insert(link.end); }
    for(const auto& image:fImages) { offsets.insert(image.offset); offsets.insert(image.offset+image.length); }
    for(auto offset:offsets) {
        if((offset>=TextLength() && offset) || std::any_of(fImages.begin(),fImages.end(),[&](const InlineImage& image){return contains(image,offset);})) continue;
        auto font=ContentFont(offset); auto face=TextFace(font);
        TextRun run{static_cast<std::size_t>(offset),bool(face&B_BOLD_FACE),bool(face&B_ITALIC_FACE),bool(face&B_UNDERSCORE_FACE),{}};
        for(const auto& link:fLinks) if(offset>=link.begin && offset<link.end) run.link=link.url;
        if(d.runs.empty() || d.runs.back().bold!=run.bold || d.runs.back().italic!=run.italic
            || d.runs.back().underline!=run.underline || d.runs.back().link!=run.link) d.runs.push_back(std::move(run));
    }
}
void DraftEditor::Save(Draft& d)
{
    SaveBody(d); d.attachments.clear();
    for(std::size_t i=0;i<fFiles.size();++i) { d.attachments.push_back(*fFiles[i]); d.attachments.back().inlineImage=IsInline(i); }
}
DraftEditor::State DraftEditor::Capture()
{
    State state; SaveBody(state.body); state.files=fFiles; GetSelection(&state.begin,&state.end);
    state.insertionFont=InsertionFont(); return state;
}
BFont DraftEditor::InsertionFont() const
{
    int32 begin,end; GetSelection(&begin,&end);
    // BTextView's public font getters read text runs, not its pending typing
    // style. In particular, an empty view may retain its last deleted run.
    if(fInsertionFont && begin==end && begin==fInsertionOffset) return *fInsertionFont;
    // A retained zero-length style run may sit exactly at TextLength() after
    // a paste or SetText. The end caret inherits the last real character.
    if(begin==end && begin==TextLength() && begin>0) {
        --begin; while(begin>0 && (static_cast<unsigned char>(ByteAt(begin))&0xc0)==0x80) --begin;
    }
    return ContentFont(begin);
}
void DraftEditor::SetInsertionFont(const BFont& font)
{
    int32 begin,end; GetSelection(&begin,&end);
    fInsertionFont.reset();
    if(begin==end) { SetFontAndColor(begin,end,&font); fInsertionFont=font; fInsertionOffset=begin; }
}
void DraftEditor::Select(int32 begin,int32 end)
{
    int32 oldBegin,oldEnd; GetSelection(&oldBegin,&oldEnd);
    EmojiTextView::Select(begin,end); GetSelection(&begin,&end);
    if(begin!=oldBegin || end!=oldEnd) { fInsertionFont.reset(); fTypingKind=0; }
}
void DraftEditor::Restore(const State& state)
{
    // Pending transfers follow their surrounding text across undo/redo too.
    std::string before(Text(),TextLength()); const auto& after=state.body.text;
    std::size_t prefix=0,suffix=0;
    while(prefix<std::min(before.size(),after.size()) && before[prefix]==after[prefix]) ++prefix;
    while(prefix && prefix<before.size() && (static_cast<unsigned char>(before[prefix])&0xc0)==0x80) --prefix;
    while(suffix<std::min(before.size(),after.size())-prefix && before[before.size()-1-suffix]==after[after.size()-1-suffix]) ++suffix;
    RemoveImageLayout();MovePositions(prefix,before.size()-suffix,after.size()-prefix-suffix);
    fRestoring=true;
    try { fInsertionFont.reset(); LoadBody(state.body); fFiles=state.files; Select(state.begin,state.end); SetInsertionFont(state.insertionFont); }
    catch(...) { fRestoring=false; throw; }
    fRestoring=false;ApplyImageLayout();
}
void DraftEditor::BeginAction()
{
    if(fActionDepth++==0) { if(!fKeyAction) fTypingKind=0; fBefore=Capture();RemoveImageLayout(); fChanged=false; }
}
void DraftEditor::EndAction(bool commit)
{
    if(--fActionDepth) return;
    if(!commit) { Restore(*fBefore); fBefore.reset(); NotifyChanged(); return; }
    if(fChanged && !SameContent(*fBefore,Capture())) {
        if(!fKeepUndo || fUndo.empty()) fUndo.push_back(std::move(*fBefore));
        fRedo.clear(); TrimHistory(); NotifyChanged();
    }
    fBefore.reset();ApplyImageLayout();if(IsFocus() && fChanged)ScrollToSelection();
}
bool DraftEditor::SameContent(const State& a,const State& b) const
{
    if(a.body.text!=b.body.text || a.files!=b.files) return false;
    if(!std::equal(a.body.runs.begin(),a.body.runs.end(),b.body.runs.begin(),b.body.runs.end(),[](const TextRun& x,const TextRun& y) {
        return x.offset==y.offset && x.bold==y.bold && x.italic==y.italic && x.underline==y.underline && x.link==y.link;
    })) return false;
    if(!std::equal(a.body.images.begin(),a.body.images.end(),b.body.images.begin(),b.body.images.end(),[](const InlineImage& x,const InlineImage& y) {
        return x.offset==y.offset && x.length==y.length && x.contentId==y.contentId;
    })) return false;
    // A style toggle at the caret changes future typing even without body text.
    return a.begin!=a.end || b.begin!=b.end || a.begin!=b.begin || a.insertionFont==b.insertionFont;
}
std::size_t DraftEditor::HistoryBytes() const
{
    std::size_t bytes=0; std::set<const Attachment*> files;
    // Current attachments are already required by the draft. Count only the
    // additional retained files, once across both history directions.
    for(const auto& file:fFiles) files.insert(file.get());
    for(const auto* history:{&fUndo,&fRedo}) for(const auto& state:*history) {
        bytes+=sizeof(State)+state.body.text.capacity()+state.body.runs.capacity()*sizeof(TextRun)
            +state.body.images.capacity()*sizeof(InlineImage)+state.files.capacity()*sizeof(state.files[0]);
        for(const auto& run:state.body.runs) bytes+=run.link.capacity();
        for(const auto& image:state.body.images) bytes+=image.contentId.capacity();
        for(const auto& file:state.files) if(files.insert(file.get()).second)
            bytes+=sizeof(Attachment)+file->data.capacity()+file->name.capacity()+file->mediaType.capacity()+file->contentId.capacity();
    }
    return bytes;
}
void DraftEditor::TrimHistory()
{
    constexpr std::size_t limit=100,bytes=64*1024*1024;
    while(fUndo.size()+fRedo.size()>limit || HistoryBytes()>bytes) {
        if(!fUndo.empty()) fUndo.pop_front();
        else if(!fRedo.empty()) fRedo.pop_front();
        else break;
    }
}
void DraftEditor::NotifyChanged() { ++fChangeCount; if(Window()) Window()->PostMessage(kEditorChanged); }
void DraftEditor::Validate() { Draft d; d.account=1; Save(d); validateDraft(d); }
void DraftEditor::StepHistory(std::deque<State>& source,std::deque<State>& destination)
{
    fTypingKind=0;
    if(source.empty()) return;
    auto current=Capture(); Restore(source.back()); destination.push_back(std::move(current)); source.pop_back(); TrimHistory(); NotifyChanged();if(IsFocus())ScrollToSelection();
}
void DraftEditor::Undo(BClipboard*) { StepHistory(fUndo,fRedo); }
void DraftEditor::Redo() { StepHistory(fRedo,fUndo); }
void DraftEditor::MouseDown(BPoint point)
{
    fTypingKind=0;
    for(const auto& layout:fImageLayout)if(InlineImageFrame(layout.image.offset).Contains(point)) {
        MakeFocus();Select(layout.image.offset,layout.image.offset+layout.image.length);Invalidate();return;
    }
    EmojiTextView::MouseDown(point);SnapImageSelection();
}
void DraftEditor::MouseUp(BPoint point) {EmojiTextView::MouseUp(point);SnapImageSelection();}
void DraftEditor::SnapImageSelection(int direction)
{
    int32 begin,end;GetSelection(&begin,&end);bool caret=begin==end;
    for(const auto& image:fImages) {
        if(caret && contains(image,begin)) {begin=end=direction<0?image.offset:image.offset+image.length;break;}
        if(!caret && image.offset<static_cast<std::size_t>(end) && image.offset+image.length>static_cast<std::size_t>(begin)) {
            begin=std::min(begin,static_cast<int32>(image.offset));end=std::max(end,static_cast<int32>(image.offset+image.length));
        }
    }
    Select(begin,end);Invalidate();
}
void DraftEditor::MakeFocus(bool focus) { fTypingKind=0; EmojiTextView::MakeFocus(focus); }
void DraftEditor::ExpandSelection(int32& begin,int32& end)
{
    if(begin==end) return;
    begin=GraphemeStart(begin);end=GraphemeEnd(end);
    for(const auto& image:fImages) if(image.offset<static_cast<std::size_t>(end) && image.offset+image.length>static_cast<std::size_t>(begin)) {
        begin=std::min(begin,static_cast<int32>(image.offset)); end=std::max(end,static_cast<int32>(image.offset+image.length));
    }
    Select(begin,end);
}
void DraftEditor::Toggle(uint16 face)
{
    Action action(*this); int32 begin,end; GetSelection(&begin,&end); ExpandSelection(begin,end);
    BFont initial=InsertionFont(); bool on=!(TextFace(initial)&face);
    auto toggle=[&](BFont& font) { auto updated=TextFace(font)&~B_REGULAR_FACE; SetTextFace(font,on?updated|face:updated&~face); };
    if(begin==end) { toggle(initial); SetInsertionFont(initial); }
    else if(auto* array=RunArray(begin,end)) {
        for(int32 i=0;i<array->count;++i) toggle(array->runs[i].font);
        SetRunArray(begin,end,array); FreeRunArray(array);
    }
    fChanged=true; MakeFocus();
}
void DraftEditor::ClearLinks(int32 begin,int32 end)
{
    std::vector<LinkSpan> links;
    for(const auto& link:fLinks) {
        if(link.end<=begin || link.begin>=end) links.push_back(link);
        else { if(link.begin<begin) links.push_back({link.begin,begin,link.url}); if(link.end>end) links.push_back({end,link.end,link.url}); }
    }
    fLinks=std::move(links);
}
void DraftEditor::Link(const std::string& url)
{
    Draft check; check.account=1; check.runs.push_back({0,false,false,false,url}); validateDraft(check);
    Action action(*this); int32 begin,end; GetSelection(&begin,&end); ExpandSelection(begin,end);
    if(begin==end) { Insert(url.c_str()); end=begin+url.size(); }
    ClearLinks(begin,end); if(!url.empty()) fLinks.push_back({begin,end,url});
    if(auto* array=RunArray(begin,end)) {
        for(int32 i=0;i<array->count;++i) SetTextFace(array->runs[i].font,TextFace(array->runs[i].font)|B_UNDERSCORE_FACE);
        SetRunArray(begin,end,array); FreeRunArray(array);
    }
    fChanged=true; MakeFocus();
}
void DraftEditor::ReplaceSelection(const std::string& text)
{ Action action(*this);SnapToGraphemes(); Delete(); Insert(text.c_str()); fChanged=true; MakeFocus(); }
const Attachment* DraftEditor::AttachmentAt(int32 index) const
{ return index>=0 && static_cast<std::size_t>(index)<fFiles.size()?fFiles[index].get():nullptr; }
bool DraftEditor::IsInline(int32 index) const
{
    const auto* file=AttachmentAt(index);
    return file && std::any_of(fImages.begin(),fImages.end(),[&](const InlineImage& image){return image.contentId==file->contentId;});
}
void DraftEditor::PlaceImage(int32 index,int32 at)
{
    auto file=fFiles.at(index);
    if(file->contentId.empty()) { auto updated=std::make_shared<Attachment>(*file); updated->contentId=contentId(); fFiles[index]=file=updated; }
    at=GraphemeEnd(std::clamp(at,0,TextLength()));
    for(const auto& image:fImages) if(contains(image,at)) at=image.offset+image.length;
    auto marker=imagePlaceholder(*file); int32 before=TextLength(); Insert(at,marker.c_str(),marker.size());
    if(TextLength()-before!=static_cast<int32>(marker.size())) throw std::runtime_error("The message body exceeds 2 MiB.");
    fImages.push_back({static_cast<std::size_t>(at),marker.size(),file->contentId});
    std::sort(fImages.begin(),fImages.end(),[](const InlineImage& a,const InlineImage& b){return a.offset<b.offset;});
    Select(at+marker.size(),at+marker.size()); fChanged=true;
}
void DraftEditor::AddAttachments(std::vector<Attachment> files,int32 at)
{
    Action action(*this); if(at<0) { int32 end; GetSelection(&at,&end); }
    for(auto& file:files) {
        bool inlineImage=file.inlineImage; fFiles.push_back(std::make_shared<const Attachment>(std::move(file)));
        if(inlineImage) { PlaceImage(fFiles.size()-1,at); int32 end; GetSelection(&at,&end); }
    }
    fChanged=true; Validate();
}
void DraftEditor::RemovePlacements(const std::string& cid)
{
    auto images=fImages;
    for(auto it=images.rbegin();it!=images.rend();++it) if(it->contentId==cid) Delete(it->offset,it->offset+it->length);
}
void DraftEditor::RemoveAttachment(int32 index)
{
    const auto* file=AttachmentAt(index); if(!file) return;
    Action action(*this); auto cid=file->contentId; RemovePlacements(cid); fFiles.erase(fFiles.begin()+index); fChanged=true;
}
void DraftEditor::ToggleImage(int32 index)
{
    const auto* file=AttachmentAt(index); if(!file || file->mediaType.rfind("image/",0)!=0) return;
    Action action(*this);
    if(IsInline(index)) { auto cid=file->contentId; RemovePlacements(cid); }
    else { int32 begin,end; GetSelection(&begin,&end); PlaceImage(index,begin); }
    Validate(); fChanged=true; MakeFocus();
}
void DraftEditor::MovePositions(int32 begin,int32 end,int32 inserted)
{
    if(fImageScrollAnchor) {
        auto& p=fImageScrollAnchor->offset;p=(p>=end?p-(end-begin):std::min(p,begin));if(p>=begin)p+=inserted;
    }
    for(auto& item:fPositions) {
        auto& p=item.second.offset;
        p=(p>=end?p-(end-begin):std::min(p,begin)); if(p>=begin) p+=inserted;
    }
}
int64 DraftEditor::RememberPosition(int32 at)
{
    for(auto it=fPositions.begin();it!=fPositions.end();) {
        if(system_time()-it->second.created>300000000) it=fPositions.erase(it); else ++it;
    }
    if(fPositions.size()>=16) throw std::runtime_error("Wait for pending attachment copies to finish.");
    at=std::clamp(at,0,TextLength()); fPositions[++fNextPosition]={at,system_time()}; return fNextPosition;
}
int32 DraftEditor::TakePosition(int64 id)
{
    auto found=fPositions.find(id); if(found==fPositions.end()) return -1;
    auto at=found->second.offset; fPositions.erase(found); return at;
}
void DraftEditor::ForgetPosition(int64 id) { fPositions.erase(id); }
void DraftEditor::InsertText(const char* text,int32 length,int32 at,const text_run_array* runs)
{
    if(length<=0) return;
    if(fRestoring) { EmojiTextView::InsertText(text,length,at,runs); return; }
    Action action(*this);
    auto font=InsertionFont();
    if(!runs) SetFontAndColor(at,at,&font);
    for(auto& link:fLinks) { if(at<=link.begin) { link.begin+=length; link.end+=length; } else if(at<link.end) link.end+=length; }
    fImages.erase(std::remove_if(fImages.begin(),fImages.end(),[&](const InlineImage& image){return contains(image,at);}),fImages.end());
    for(auto& image:fImages) if(image.offset>=static_cast<std::size_t>(at)) image.offset+=length;
    MovePositions(at,at,length); EmojiTextView::InsertText(text,length,at,runs);
    if(runs) GetFontAndColor(at+length-1,&font);
    SetInsertionFont(font); fChanged=true;
}
void DraftEditor::DeleteText(int32 begin,int32 end)
{
    if(fRestoring) { EmojiTextView::DeleteText(begin,end); return; }
    if(begin==end) return;
    Action action(*this); auto move=[&](int32 p){return p>=end?p-(end-begin):std::min(p,begin);};
    BFont font; GetFontAndColor(begin,&font);
    for(auto& link:fLinks) { link.begin=move(link.begin); link.end=move(link.end); }
    fLinks.erase(std::remove_if(fLinks.begin(),fLinks.end(),[](const LinkSpan& link){return link.begin==link.end;}),fLinks.end());
    fImages.erase(std::remove_if(fImages.begin(),fImages.end(),[&](const InlineImage& image){return image.offset<static_cast<std::size_t>(end) && image.offset+image.length>static_cast<std::size_t>(begin);}),fImages.end());
    for(auto& image:fImages) if(image.offset>=static_cast<std::size_t>(end)) image.offset-=end-begin;
    MovePositions(begin,end,0); EmojiTextView::DeleteText(begin,end); SetInsertionFont(font); fChanged=true;
}
void DraftEditor::Copy(BClipboard* clipboard)
{
    fTypingKind=0;
    EmojiTextView::Copy(clipboard); int32 begin,end; GetSelection(&begin,&end); if(begin==end) return;
    Draft body; SaveBody(body); Draft fragment; fragment.account=1; fragment.text=body.text.substr(begin,end-begin);
    TextRun initial;
    for(const auto& run:body.runs) { if(run.offset<=static_cast<std::size_t>(begin)) initial=run; }
    initial.offset=0; fragment.runs.push_back(initial);
    for(auto run:body.runs) if(run.offset>static_cast<std::size_t>(begin) && run.offset<static_cast<std::size_t>(end)) { run.offset-=begin; fragment.runs.push_back(run); }
    for(auto image:body.images) if(image.offset>=static_cast<std::size_t>(begin) && image.offset+image.length<=static_cast<std::size_t>(end)) { image.offset-=begin; fragment.images.push_back(image); }
    for(const auto& file:fFiles) if(std::any_of(fragment.images.begin(),fragment.images.end(),[&](const InlineImage& image){return image.contentId==file->contentId;})) {
        fragment.attachments.push_back(*file); fragment.attachments.back().inlineImage=true;
    }
    validateDraft(fragment); auto encoded=encodeFragment(fragment);
    std::optional<std::string> html;
    try { html=exportClipboardHtml(fragment); } catch(const std::invalid_argument&) { /* Large images retain the native/Mail representations. */ }
    if(!clipboard->Lock()) return;
    const void* plain=nullptr; ssize_t size=0; auto* data=clipboard->Data();
    if(data && data->FindData("text/plain",B_MIME_TYPE,&plain,&size)==B_OK && size==end-begin && std::string(static_cast<const char*>(plain),size)==fragment.text) {
        // Private layout fonts must not escape the application. Export native
        // text runs using the same semantic fonts as the saved draft.
        int32 runBytes=0;
        if(auto* runs=RunArray(begin,end,&runBytes)) {
            for(int32 i=0;i<runs->count;++i) {
                auto font=ContentFont(begin+runs->runs[i].offset);auto face=TextFace(font);
                font=*be_plain_font;SetTextFace(font,face);runs->runs[i].font=font;
            }
            data->RemoveName("application/x-vnd.Be-text_run_array");
            data->AddData("application/x-vnd.Be-text_run_array",B_MIME_TYPE,runs,runBytes);FreeRunArray(runs);
        }
        data->RemoveName("mail:fragment"); data->AddMessage("mail:fragment",&encoded);
        data->RemoveName("text/html"); if(html) data->AddData("text/html",B_MIME_TYPE,html->data(),html->size());
        clipboard->Commit();
    }
    clipboard->Unlock();
}
void DraftEditor::Cut(BClipboard* clipboard) { Action action(*this); Copy(clipboard); Delete(); }
void DraftEditor::Paste(BClipboard* clipboard)
{
    std::optional<Draft> fragment; std::string html,htmlError; bool plainAvailable=false;
    if(clipboard->Lock()) {
        struct Unlock { BClipboard* clipboard; ~Unlock(){clipboard->Unlock();} } unlock{clipboard};
        const void* text=nullptr; ssize_t length=0; auto* data=clipboard->Data();
        plainAvailable=data && data->FindData("text/plain",B_MIME_TYPE,&text,&length)==B_OK && length>=0;
        try { if(plainAvailable && length<=2*1024*1024) fragment=decodeFragment(*data,static_cast<const char*>(text),length); }
        catch(const std::exception&) { /* Try portable HTML, then native text. */ }
        // If another application changed only Mail's plain representation,
        // both our private fragment and our accompanying HTML may be stale.
        if(!fragment && data && !data->HasMessage("mail:fragment") && data->FindData("text/html",B_MIME_TYPE,&text,&length)==B_OK) {
            if(length>0 && static_cast<std::size_t>(length)<=kClipboardHtmlLimit) html.assign(static_cast<const char*>(text),length);
            else if(length>0) htmlError="Clipboard HTML exceeds 8 MiB.";
        }
    }
    // Parsing and image decoding happen after releasing the system clipboard.
    if(!fragment && !html.empty()) {
        try {auto parsed=importClipboardHtml(html);if(!parsed.text.empty())fragment=std::move(parsed);}
        catch(const std::exception& error) {htmlError=error.what();}
    }
    if(!fragment && !plainAvailable && !htmlError.empty()) throw std::invalid_argument(htmlError);
    Action action(*this);
    if(!fragment) { EmojiTextView::Paste(clipboard); return; }
    SnapToGraphemes();
    int32 begin,end; GetSelection(&begin,&end); Delete(); int32 before=TextLength(); Insert(fragment->text.c_str());
    if(TextLength()-before!=static_cast<int32>(fragment->text.size())) throw std::runtime_error("The message body exceeds 2 MiB.");
    end=begin+fragment->text.size(); ClearLinks(begin,end);
    for(auto& file:fragment->attachments) {
        auto found=std::find_if(fFiles.begin(),fFiles.end(),[&](const auto& existing){return existing->contentId==file.contentId && existing->name==file.name && existing->mediaType==file.mediaType && existing->data==file.data;});
        if(found!=fFiles.end()) continue;
        auto old=file.contentId; file.contentId=contentId();
        for(auto& image:fragment->images) if(image.contentId==old) image.contentId=file.contentId;
        fFiles.push_back(std::make_shared<const Attachment>(std::move(file)));
    }
    for(auto image:fragment->images) { image.offset+=begin; fImages.push_back(std::move(image)); }
    std::sort(fImages.begin(),fImages.end(),[](const InlineImage& a,const InlineImage& b){return a.offset<b.offset;});
    for(std::size_t i=0;i<fragment->runs.size();++i) {
        const auto& run=fragment->runs[i]; int32 to=i+1<fragment->runs.size()?begin+fragment->runs[i+1].offset:end;
        auto font=styleFont(run); SetFontAndColor(begin+run.offset,to,&font);
        if(!run.link.empty()) fLinks.push_back({static_cast<int32>(begin+run.offset),to,run.link});
    }
    ApplyEmojiFonts(*this); Select(end,end); Validate(); fChanged=true;
}
void DraftEditor::Report(const std::exception& error)
{ BMessage message(kEditorError); message.AddString("text",error.what()); if(Window()) Window()->PostMessage(&message); }
void DraftEditor::KeyDown(const char* text,int32 length)
{
    if(length<=0) return;
    auto first=static_cast<unsigned char>(text[0]);
    int kind=first==B_BACKSPACE?2:(first==B_DELETE?3:(first>=B_SPACE || first==B_ENTER || first==B_TAB?1:0));
    if(!kind) {
        fTypingKind=0;EmojiTextView::KeyDown(text,length);SnapImageSelection(first==B_LEFT_ARROW?-1:1);return;
    }
    int32 begin,end; GetSelection(&begin,&end); auto now=system_time();
    fKeepUndo=kind && kind==fTypingKind && begin==end && begin==fTypingCaret && now-fTypingTime<1500000 && !fUndo.empty();
    fKeyAction=true;
    try {
        { Action action(*this);
          if(begin==end && (first==B_BACKSPACE || first==B_DELETE)) {
              for(const auto& image:fImages)if((first==B_BACKSPACE && begin==static_cast<int32>(image.offset+image.length))
                  || (first==B_DELETE && begin==static_cast<int32>(image.offset))) {Select(image.offset,image.offset+image.length);break;}
          }
          EmojiTextView::KeyDown(text,length);SnapImageSelection(); }
        GetSelection(&fTypingCaret,&end); fTypingKind=kind; fTypingTime=now;
    } catch(const std::exception& error) { fTypingKind=0; Report(error); }
    fKeyAction=false; fKeepUndo=false;
}
void DraftEditor::MessageReceived(BMessage* message)
{
    try {
        if(message->what==kInlineImagesReady) {ApplyImageLayout();return;}
        if(message->what==kEditorRedo) { Redo(); return; }
        if(((message->what==B_SIMPLE_DATA || message->what==B_REFS_RECEIVED) && message->HasRef("refs")) || IsMailAttachmentOffer(*message)) {
            int32 at,end; GetSelection(&at,&end);
            if(message->WasDropped()) {
                auto point=ConvertFromScreen(message->DropPoint());at=OffsetAt(point);
                for(const auto& layout:fImageLayout) {
                    auto frame=InlineImageFrame(layout.image.offset);
                    if(frame.Contains(point)){at=point.x<(frame.left+frame.right)/2?layout.image.offset:layout.image.offset+layout.image.length;break;}
                }
            }
            auto position=RememberPosition(at);
            BMessage refs(*message); refs.RemoveName("inline"); refs.AddBool("inline",true);
            refs.RemoveName("mail:position"); refs.AddInt64("mail:position",position);
            if(BMessenger(Window()).SendMessage(&refs,message->ReturnAddress())!=B_OK) ForgetPosition(position);
            return;
        }
        if(message->what==B_SIMPLE_DATA) { Action action(*this); BTextView::MessageReceived(message); }
        else BTextView::MessageReceived(message);
    } catch(const std::exception& error) { Report(error); }
}
}
