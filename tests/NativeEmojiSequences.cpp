#include "ComposerWindow.h"
#include "DraftEditor.h"
#include "Emoji.h"
#include "EmojiShaping.h"
#include "mail/Mime.h"
#include <Application.h>
#include <Autolock.h>
#include <Bitmap.h>
#include <BitmapStream.h>
#include <Clipboard.h>
#include <File.h>
#include <MessageRunner.h>
#include <Resources.h>
#include <Screen.h>
#include <TranslatorRoster.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <set>
#include <vector>
#include <unistd.h>

using namespace mail;
using namespace mail::ui;
namespace {
int checks=0;
void check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
struct Example {const char* text;unsigned glyph;};
// Glyph IDs refer to the exact, checksummed original Noto resource. FreeType
// draws that glyph directly, independently of Mail's font subsetting/cmap.
constexpr Example examples[]={{"👩🏽‍💻",1455},{"🇦🇺",1626},{"👨‍👩‍👧‍👦",1486},{"👩‍👩‍👧‍👧",1497},
    {"1️⃣",1550},{"🏳️‍🌈",1875},{"👩🏾‍❤️‍👩🏼",646},{"👍🏽",578}};
std::unique_ptr<BBitmap> render(const std::string& text,float width=128,float size=64,bool bold=false,bool legacy=false)
{
    auto bitmap=std::make_unique<BBitmap>(BRect(0,0,127,111),B_BITMAP_ACCEPTS_VIEWS,B_RGB32);
    check(bitmap->InitCheck()==B_OK && bitmap->Lock(),"allocate native glyph bitmap");
    auto* view=new BView(bitmap->Bounds(),"glyph pixels",B_FOLLOW_NONE,B_WILL_DRAW);bitmap->AddChild(view);
    view->SetHighColor(255,255,255);view->FillRect(view->Bounds());view->SetLowColor(255,255,255);view->SetHighColor(0,0,0);
    BFont font(bold?*be_bold_font:*be_plain_font);font.SetSize(size);font.SetFlags(B_FORCE_ANTIALIASING);
    if(legacy){
        auto glyph=*EmojiSequenceFont(text);glyph.SetSize(size);glyph.SetFalseBoldWidth(.6f);view->SetFont(&glyph);
        view->DrawString(text.data(),4,BPoint(8,85)); // Known regression's first scalar is U+1F469.
    }else DrawEmojiText(*view,text,font,{8,85},width);
    view->Sync();bitmap->RemoveChild(view);delete view;bitmap->Unlock();return bitmap;
}
std::vector<unsigned char> mask(const BBitmap& bitmap)
{
    std::vector<unsigned char> result(128*112);auto* bytes=static_cast<const uint8*>(bitmap.Bits());
    for(int y=0;y<112;++y)for(int x=0;x<128;++x){const auto* p=bytes+y*bitmap.BytesPerRow()+4*x;result[y*128+x]=255-(int(p[0])+p[1]+p[2])/3;}
    return result;
}
void pixels()
{
    FT_Library library=nullptr;check(FT_Init_FreeType(&library)==0,"initialize independent raster reference");
    struct Library {FT_Library p;~Library(){FT_Done_FreeType(p);}} ownedLibrary{library};
    std::size_t size=0;const auto* data=static_cast<const FT_Byte*>(BApplication::AppResources()->LoadResource('RAWT',"Mail:NotoEmoji",&size));
    FT_Face face=nullptr;check(data && FT_New_Memory_Face(library,data,size,0,&face)==0,"load original font in FreeType");
    struct Face {FT_Face p;~Face(){FT_Done_Face(p);}} ownedFace{face};
    check(FT_Set_Char_Size(face,0,64*64,72,72)==0,"set reference glyph size");
    for(const auto& example:examples){
        auto shaped=EmojiSequenceFont(example.text);check(bool(shaped),"complete emoji sequence obtains a shaped native face");
        shaped->SetSize(64);auto blank=EmojiContinuationFont();blank.SetSize(64);
        check(IsEmojiFont(*shaped) && IsEmojiFont(blank),"private sequence fonts retain semantic emoji styles");
        check(blank.StringWidth(example.text)==0,"continuation scalars have zero native advance");
        for(auto style:{B_BOLD_FACE,B_ITALIC_FACE,B_UNDERSCORE_FACE}){
            SetTextFace(blank,style);check(blank.StringWidth(example.text)==0,"styled continuation scalars remain zero width");
            SetTextFace(*shaped,style);
            auto expected=StyledEmojiFont(*EmojiSequenceFont(example.text),style&B_BOLD_FACE);
            check(expected && shaped->FamilyAndStyle()==expected->FamilyAndStyle(),"decorations preserve the glyph and select its designed weight");
            check(shaped->FalseBoldWidth()==0 && (TextFace(*shaped)&style)==style,"native weight preserves formatting without synthetic outline expansion");
        }
        SetTextFace(*shaped,B_REGULAR_FACE);
        check(FT_Load_Glyph(face,example.glyph,FT_LOAD_DEFAULT)==0 && FT_Render_Glyph(face->glyph,FT_RENDER_MODE_NORMAL)==0,"render original shaped glyph by index");
        auto glyph=face->glyph;int first=(static_cast<unsigned char>(example.text[0])<0x80)?1:4;
        if((static_cast<unsigned char>(example.text[0])&0xf0)==0xe0)first=3;
        check(std::abs(shaped->StringWidth(example.text,first)-glyph->advance.x/64.f)<2,"native width matches original ligature advance");
        std::vector<unsigned char> reference(128*112);const auto& bitmap=glyph->bitmap;
        for(unsigned y=0;y<bitmap.rows;++y)for(unsigned x=0;x<bitmap.width;++x){
            int dx=8+glyph->bitmap_left+x,dy=85-glyph->bitmap_top+y;
            if(dx>=0 && dx<128 && dy>=0 && dy<112)reference[dy*128+dx]=bitmap.buffer[y*bitmap.pitch+x];
        }
        auto actual=mask(*render(example.text));double intersection=0,united=0;
        for(std::size_t i=0;i<actual.size();++i){intersection+=std::min(actual[i],reference[i]);united+=std::max(actual[i],reference[i]);}
        auto overlap=united?intersection/united:0;
        std::printf("Sequence %s: original glyph %u, raster overlap %.3f\n",example.text,example.glyph,overlap);
        check(overlap>0.70,"native shaped pixels match original glyph, including blank continuation bytes");
    }
    // The original regression used a 13-point bold label. Compare both designed
    // weights and label sizes with FreeType's original variable font, not just
    // the large regular glyph path used above.
    std::vector<Example> weighted(std::begin(examples),std::end(examples));
    weighted.push_back({"🎉",FT_Get_Char_Index(face,0x1f389)});
    weighted.push_back({"😀",FT_Get_Char_Index(face,0x1f600)});
    for(bool bold:{false,true})for(float size:{13.f,24.f,64.f}){
        FT_Fixed weight=(bold?700:400)*65536;
        check(FT_Set_Var_Design_Coordinates(face,1,&weight)==0,"select independent original font weight");
        check(FT_Set_Char_Size(face,0,size*64,72,72)==0,"set reference label size");
        double minimum=1;
        for(const auto& example:weighted){
            const bool lcd=size<32;
            check(FT_Load_Glyph(face,example.glyph,lcd?FT_LOAD_TARGET_LCD:FT_LOAD_DEFAULT)==0
                && FT_Render_Glyph(face->glyph,lcd?FT_RENDER_MODE_LCD:FT_RENDER_MODE_NORMAL)==0,"render weighted reference glyph");
            auto glyph=face->glyph;std::vector<unsigned char> reference(128*112);const auto& bitmap=glyph->bitmap;
            for(unsigned y=0;y<bitmap.rows;++y)for(unsigned x=0;x<bitmap.width/(lcd?3:1);++x){
                int dx=8+glyph->bitmap_left+x,dy=85-glyph->bitmap_top+y;
                const auto* p=bitmap.buffer+y*bitmap.pitch+x*(lcd?3:1);
                if(dx>=0 && dx<128 && dy>=0 && dy<112)reference[dy*128+dx]=lcd?(int(p[0])+p[1]+p[2])/3:p[0];
            }
            auto actual=mask(*render(example.text,128,size,bold));double intersection=0,united=0;
            for(std::size_t i=0;i<actual.size();++i){intersection+=std::min(actual[i],reference[i]);united+=std::max(actual[i],reference[i]);}
            double overlap=united?intersection/united:0;
            double best=overlap,ink=0,referenceInk=0;int shiftX=0,shiftY=0;
            for(std::size_t i=0;i<actual.size();++i){ink+=actual[i];referenceInk+=reference[i];}
            for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){
                double a=0,b=0;
                for(int y=0;y<112;++y)for(int x=0;x<128;++x){int xx=x+dx,yy=y+dy;int r=xx>=0 && xx<128 && yy>=0 && yy<112?reference[yy*128+xx]:0;
                    a+=std::min(int(actual[y*128+x]),r);b+=std::max(int(actual[y*128+x]),r);}
                if(b && a/b>best){best=a/b;shiftX=dx;shiftY=dy;}
            }
            std::printf("Weighted sequence %s: weight %d, size %.0f, overlap %.3f, aligned %.3f (%d,%d), ink %.3f\n",example.text,bold?700:400,size,overlap,best,shiftX,shiftY,ink/referenceInk);
            // The native hinting/baseline rounds differently by up to one pixel.
            // Keep both shape overlap and total ink bounded; a filled-in glyph
            // cannot pass just by lining its bounding box up with the reference.
            minimum=std::min(minimum,best);
            check(best>(size<32?0.55:0.70) && ink/referenceInk>0.70 && ink/referenceInk<1.30,
                "label glyph retains the original weight's visible details");
            if(bold && size==13 && example.glyph==1455){
                auto legacy=mask(*render(example.text,128,size,false,true));double oldInk=0;
                for(auto pixel:legacy)oldInk+=pixel;
                std::printf("Original header defect: synthetic/reference ink ratio %.3f; designed/reference %.3f\n",oldInk/referenceInk,ink/referenceInk);
                check(oldInk/referenceInk>1.30 && ink<oldInk,"small-label reference rejects the original filled-in synthetic glyph");
            }
        }
        std::printf("Emoji label weight %d, size %.0f: minimum raster overlap %.3f\n",bold?700:400,size,minimum);
    }
    auto before=EmojiShapingCacheInfo();
    bool cached=true;for(int i=0;i<500;++i)for(const auto& example:examples)cached=bool(EmojiSequenceFont(example.text)) && cached;
    check(cached,"repeat shaping retains a cached face");
    auto after=EmojiShapingCacheInfo();check(after.fonts==before.fonts && after.bytes==before.bytes && after.fonts<=after.glyphLimit,"font cache is shared by glyph and bounded by bundled glyph count");
    check(!EmojiSequenceFont("plain") && !EmojiSequenceFont(std::string(2048,'x')),"nonemoji and oversized clusters do not create glyph fonts");
    BFont label(*be_plain_font);label.SetSize(64);
    auto clipped=mask(*render("👩🏽‍💻Z",label.StringWidth("…")+1)),ellipsis=mask(*render("…",128));
    check(clipped==ellipsis,"narrow labels omit an entire joined emoji before the ellipsis");
    auto regional=[](unsigned index){unsigned c=0x1f1e6+index;return std::string{char(0xf0|(c>>18)),char(0x80|((c>>12)&63)),char(0x80|((c>>6)&63)),char(0x80|(c&63))};};
    std::set<uint32> glyphs,families;
    for(unsigned a=0;a<26;++a)for(unsigned b=0;b<26;++b)if(auto flag=EmojiSequenceFont(regional(a)+regional(b))){glyphs.insert(flag->FamilyAndStyle());families.insert(flag->FamilyAndStyle()>>16);}
    check(glyphs.size()>=150 && families.size()==1,"more than 128 different flag glyphs load in one private family");
    after=EmojiShapingCacheInfo();
    std::printf("Emoji font cache: %zu faces, %zu font bytes, maximum %zu faces\n",after.fonts,after.bytes,after.glyphLimit);
}
void styleProbe()
{
    const char* path=std::getenv("MAIL_EMOJI_STYLE_GRID");if(!path)return;
    auto* bitmap=new BBitmap(BRect(0,0,479,289),B_BITMAP_ACCEPTS_VIEWS,B_RGB32);
    check(bitmap->InitCheck()==B_OK && bitmap->Lock(),"allocate emoji style probe");
    auto* view=new BView(bitmap->Bounds(),"style probe",B_FOLLOW_NONE,B_WILL_DRAW);bitmap->AddChild(view);
    view->SetHighColor(255,255,255);view->FillRect(view->Bounds());view->SetLowColor(255,255,255);view->SetHighColor(0,0,0);
    const char* labels[]={"Designed regular","Designed bold","Old synthetic bold"};
    for(int column=0;column<3;++column){view->SetFont(be_plain_font);view->DrawString(labels[column],BPoint(8+column*160,20));}
    int row=0;
    for(float size:{13.f,24.f,48.f,64.f}){
        for(int column=0;column<3;++column){
            BPoint point(20+column*160,50+row*65);
            if(column<2){BFont font(column?*be_bold_font:*be_plain_font);font.SetSize(size);DrawEmojiText(*view,"👩🏽‍💻",font,point,150);}
            else {auto font=*EmojiSequenceFont("👩🏽‍💻");font.SetSize(size);font.SetFalseBoldWidth(.6f);view->SetFont(&font);view->DrawString("👩",point);}
        }
        ++row;
    }
    view->Sync();bitmap->RemoveChild(view);delete view;bitmap->Unlock();
    BBitmapStream stream(bitmap);BFile output(path,B_WRITE_ONLY|B_CREATE_FILE|B_ERASE_FILE);
    check(BTranslatorRoster::Default()->Translate(&stream,nullptr,nullptr,&output,B_PNG_FORMAT)==B_OK,"save emoji style probe");
}
class Test:public BApplication {
public:
    explicit Test(std::string directory):BApplication("application/x-vnd.air-Mail-Emoji-Sequences-Test"),fDirectory(std::move(directory)){}
    int result=1;
    void ReadyToRun() override
    {
        try{
            styleProbe();pixels();fStore=std::make_shared<Store>(fDirectory+"/mail.db");Account account;account.provider=Provider::Gmail;account.email="author@example.test";account.name="Emoji fixture";account.id=fStore->saveAccount(account);fAccount=account;
            Draft d;d.account=account.id;d.to="recipient@example.test";d.subject="Joined emoji 👩🏽‍💻 and flags 🇦🇺";d.text="Ready.";
            fOwner=new BWindow(BRect(20,30,700,650),"Emoji fixture",B_TITLED_WINDOW,0);fOwner->Show();fComposer=new ComposerWindow(fOwner,fStore,d);fComposer->Show();
            BMessage tick('test');fTimer=std::make_unique<BMessageRunner>(BMessenger(this),&tick,200000);
        }catch(const std::exception& error){Fail(error.what());}
    }
    void MessageReceived(BMessage* message) override
    {
        if(message->what!='test'){BApplication::MessageReceived(message);return;}
        try{
            BAutolock lock(fComposer);auto* editor=dynamic_cast<DraftEditor*>(fComposer->FindView("compose body"));check(editor,"native composer exists");
            if(fStep++==0){
                Draft d;d.account=fAccount.id;d.to="recipient@example.test";d.text="";editor->Load(d);editor->MakeFocus();
                for(const char* part:{"👩","🏽","‍","💻"})editor->KeyDown(part,std::strlen(part));
                check(std::string(editor->Text())=="👩🏽‍💻","incremental typing retains exact sequence bytes");
                BFont font;editor->GetFontAndColor(0,&font);check(font.FamilyAndStyle()==EmojiSequenceFont("👩🏽‍💻")->FamilyAndStyle(),"incremental typing reshapes the preceding emoji");
                auto advance=editor->PointAt(editor->TextLength()).x-editor->PointAt(0).x;check(std::abs(advance-font.StringWidth("👩"))<2,"native caret uses one ligature width");
                editor->Select(0,editor->TextLength());editor->Toggle(B_BOLD_FACE);editor->Toggle(B_ITALIC_FACE);editor->Toggle(B_UNDERSCORE_FACE);editor->Save(d);
                check(d.runs.size()==1 && d.runs[0].bold && d.runs[0].italic && d.runs[0].underline,"sequence font boundaries coalesce into semantic formatting");
                d=fStore->draft(fStore->saveDraft(d));auto sent=composeMessage(d,fAccount,2000000000);auto parsed=parseMessage(sent.raw);
                check(parsed.text.find(d.text)!=std::string::npos && parsed.html.find(d.text)!=std::string::npos,"MIME keeps Unicode rather than presentation glyph IDs");
                d.text="Joined emoji\n👩🏽‍💻  👨‍👩‍👧‍👦  👩‍👩‍👧‍👧\nFlags\n🇦🇺  🇬🇧  🇺🇸  🏳️‍🌈\nModifiers and keycaps\n👍🏽  👩🏾‍❤️‍👩🏼  1️⃣  #️⃣\nText stays editable and copies as Unicode.";
                d.runs.clear();editor->Load(d);font=*be_plain_font;font.SetSize(23);editor->SetFontAndColor(0,editor->TextLength(),&font);ApplyEmojiFonts(*editor);editor->Select(0,0);
            }else if(fStep==4){
                if(const char* path=std::getenv("MAIL_SEQUENCES_SCREENSHOT")){
                    BBitmap* bitmap=nullptr;check(BScreen().GetBitmap(&bitmap)==B_OK,"capture native emoji composer");BBitmapStream stream(bitmap);BFile output(path,B_WRITE_ONLY|B_CREATE_FILE|B_ERASE_FILE);
                    check(BTranslatorRoster::Default()->Translate(&stream,nullptr,nullptr,&output,B_PNG_FORMAT)==B_OK,"save native emoji capture");
                }
                fTimer.reset();result=0;std::printf("Native emoji sequences: %d checks passed\n",checks);PostMessage(B_QUIT_REQUESTED);
            }
        }catch(const std::exception& error){Fail(error.what());}
    }
private:
    void Fail(const char* message){fTimer.reset();std::fprintf(stderr,"FAIL emoji sequences: %s\n",message);PostMessage(B_QUIT_REQUESTED);}
    std::string fDirectory;std::shared_ptr<Store> fStore;Account fAccount;BWindow* fOwner=nullptr;ComposerWindow* fComposer=nullptr;std::unique_ptr<BMessageRunner> fTimer;int fStep=0;
};
}
int main(){char path[]="/tmp/mail-emoji-ui-XXXXXX";if(!mkdtemp(path))return 2;int result;{Test app(path);app.Run();result=app.result;}std::filesystem::remove_all(path);return result;}
