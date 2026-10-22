#include "ComposerWindow.h"
#include "DraftEditor.h"
#include "Emoji.h"
#include "mail/Mime.h"
#include <Application.h>
#include <Autolock.h>
#include <Bitmap.h>
#include <BitmapStream.h>
#include <Clipboard.h>
#include <File.h>
#include <MessageRunner.h>
#include <Screen.h>
#include <TextControl.h>
#include <TranslatorRoster.h>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <unistd.h>

namespace {
int checks=0;
void check(bool value,const char* why) { ++checks; if (!value) throw std::runtime_error(why); }
constexpr uint16 kStyles=B_BOLD_FACE|B_ITALIC_FACE|B_UNDERSCORE_FACE;
class Typography : public BApplication {
public:
    explicit Typography(std::string directory) : BApplication("application/x-vnd.air-Mail-Typography-Test"),fDirectory(std::move(directory)) {}
    int result=1;
    void ReadyToRun() override
    {
        try {
            check(mail::ui::EmojiFontAvailable(),"embedded Noto Emoji loads without a font installation");
            BFont font=mail::ui::EmojiFont(*be_plain_font);
            for (const char* value:{"😀","😊","🎉","👍","❤","🙏","🌿","✅","📅","☕","🚀","👋","🫠","🫶"}) {
                bool glyph=false; font.GetHasGlyphs(value,1,&glyph,false); check(glyph,"embedded font covers picker and recent emoji");
            }
            fStore=std::make_shared<mail::Store>(fDirectory+"/mail.db");
            fAccount.name="Typography test"; fAccount.email="author@example.test"; fAccount.provider=mail::Provider::Gmail;
            fAccount.id=fStore->saveAccount(fAccount); // No sync worker or network is started in this test.
            mail::Draft draft; draft.account=fAccount.id; draft.subject="Emoji 🎉 and rich text"; draft.to="recipient@example.test";
            draft.text="Hello 🎉 café 😀 🫠 🫶\nPlain text remains in the system font.\n";
            draft.runs={{0,true,true,true,"https://example.test"}};
            draft.id=fStore->saveDraft(draft); fDraft=draft.id; fOriginal=draft.text;
            fOwner=new BWindow(BRect(20,30,950,760),"Typography fixture",B_TITLED_WINDOW,0); fOwner->Show();
            fComposer=new mail::ui::ComposerWindow(fOwner,fStore,draft); fComposer->Show();
            BMessage tick('test'); fTimer=std::make_unique<BMessageRunner>(BMessenger(this),&tick,200000);
        } catch (const std::exception& error) { fail(error.what()); }
    }
    void MessageReceived(BMessage* message) override
    {
        if (message->what!='test') { BApplication::MessageReceived(message); return; }
        try {
            check(++fTicks<100,"typography test completes");
            BAutolock lock(fComposer); check(lock.IsLocked(),"lock native composer");
            auto* editor=dynamic_cast<mail::ui::DraftEditor*>(fComposer->FindView("compose body")); check(editor,"native editor exists");
            if (fStep==0) {
                BFont font; editor->GetFontAndColor(6,&font);
                check(mail::ui::IsEmojiFont(font) && (mail::ui::TextFace(font)&kStyles)==kStyles,"emoji load preserves bold italic and underline");
                editor->GetFontAndColor(0,&font); check(!mail::ui::IsEmojiFont(font) && (mail::ui::TextFace(font)&kStyles)==kStyles,"ordinary text keeps its system font and styles");
                auto* subject=dynamic_cast<BTextControl*>(fComposer->FindView("compose subject")); check(subject,"subject input exists");
                subject->TextView()->GetFontAndColor(6,&font); check(mail::ui::IsEmojiFont(font),"subject displays emoji with embedded font");
                editor->Select(6,10); editor->Copy(be_clipboard);
                editor->Select(editor->TextLength(),editor->TextLength()); editor->Paste(be_clipboard);
                editor->GetFontAndColor(fOriginal.size(),&font);
                check(mail::ui::IsEmojiFont(font) && (mail::ui::TextFace(font)&kStyles)==kStyles,"clipboard preserves all emoji rich-text styles");
                check(be_clipboard->Lock(),"lock clipboard for font portability check");
                const void* data=nullptr; ssize_t size=0;
                bool portable=be_clipboard->Data()->FindData("application/x-vnd.Be-text_run_array",B_MIME_TYPE,&data,&size)==B_OK;
                if (portable) { const auto* runs=static_cast<const text_run_array*>(data); portable=runs->count==1 && !mail::ui::IsEmojiFont(runs->runs[0].font); }
                be_clipboard->Unlock(); check(portable,"clipboard exports ordinary fonts usable by other applications");
                editor->Undo(be_clipboard); check(std::string(editor->Text())==fOriginal,"undo formatted emoji paste");
                editor->Select(10,10); editor->Insert("!");
                editor->GetFontAndColor(10,&font); check(!mail::ui::IsEmojiFont(font) && (mail::ui::TextFace(font)&kStyles)==kStyles,"typing after an emoji restores the text font and retains formatting");
                editor->Select(6,10); fComposer->PostMessage('cbld'); fComposer->PostMessage('cita'); fComposer->PostMessage('cund'); fComposer->PostMessage('csav'); fStep=1;
            } else if (fStep==1) {
                auto draft=fStore->draft(fDraft); if (draft.text==fOriginal) return;
                BFont font; editor->GetFontAndColor(6,&font);
                check(mail::ui::IsEmojiFont(font) && !(mail::ui::TextFace(font)&kStyles),"format toggles work on emoji without changing its display font");
                editor->GetFontAndColor(10,&font); check((mail::ui::TextFace(font)&kStyles)==kStyles,"format toggles leave adjacent text intact");
                check(draft.runs.size()==3 && draft.runs[1].offset==6 && !draft.runs[1].bold && !draft.runs[1].italic && !draft.runs[1].underline
                    && draft.runs[2].offset==10 && draft.runs[2].bold && draft.runs[2].italic && draft.runs[2].underline,"draft stores semantic formatting rather than display-font boundaries");
                check(draft.runs[1].link=="https://example.test","emoji formatting retains link metadata");
                std::string before=editor->Text(); editor->Select(6,10); editor->Copy(be_clipboard);
                editor->Select(editor->TextLength(),editor->TextLength()); editor->Paste(be_clipboard);
                check(std::string(editor->Text())==before+"🎉","clipboard preserves emoji bytes");
                editor->GetFontAndColor(before.size(),&font); check(mail::ui::IsEmojiFont(font) && !(mail::ui::TextFace(font)&kStyles),"styled paste restores emoji display and formatting");
                editor->Undo(be_clipboard); check(std::string(editor->Text())==before,"undo restores text before emoji paste");
                editor->Redo(); check(std::string(editor->Text())==before+"🎉","redo restores emoji paste");
                editor->GetFontAndColor(before.size(),&font); check(mail::ui::IsEmojiFont(font),"redo keeps the emoji font");
                fComposer->PostMessage('csav'); fStep=2;
            } else if (fStep==2) {
                auto draft=fStore->draft(fDraft); if (draft.text.back()=='\n') return;
                auto composed=mail::composeMessage(draft,fAccount,2000000000);
                auto parsed=mail::parseMessage(composed.raw);
                check(parsed.text.find("🎉")!=std::string::npos && parsed.html.find("<strong>")!=std::string::npos
                    && parsed.html.find("<em>")!=std::string::npos && parsed.html.find("<u>")!=std::string::npos,"emoji presentation changes preserve formatted MIME output");
                // A dense emoji paragraph should not exhaust the semantic style
                // limit simply because each symbol uses a different font.
                std::string dense; for (int i=0;i<200;++i) dense+="a 🎉 ";
                editor->SetText(dense.c_str()); fComposer->PostMessage('csav'); fStep=3;
            } else if (fStep==3) {
                auto draft=fStore->draft(fDraft); if (draft.text.rfind("a 🎉 ",0)!=0) return;
                check(draft.runs.size()==1,"emoji font changes coalesce to one semantic style run");
                editor->SetText("Emoji from Mail\n\n😀 😊 🎉 👍 ❤️ 🙏 🌿 ✅ 📅 ☕ 🚀 👋\n\nRecent symbols: 🫠 🫶\n\nText stays editable and copies as Unicode.\n");
                editor->SetFontAndColor(0,editor->TextLength(),be_plain_font); mail::ui::ApplyEmojiFonts(*editor);
                editor->Select(0,15); fComposer->PostMessage('cbld'); fComposer->PostMessage('csav'); fStep=4;
            } else if (fStep==4) {
                editor->Select(editor->TextLength(),editor->TextLength()); fStep=5;
            } else if (fStep==5) {
                if (const char* path=std::getenv("MAIL_TYPOGRAPHY_SCREENSHOT")) {
                    BBitmap* bitmap=nullptr; check(BScreen().GetBitmap(&bitmap)==B_OK,"capture typography screenshot");
                    BBitmapStream stream(bitmap); BFile file(path,B_WRITE_ONLY|B_CREATE_FILE|B_ERASE_FILE);
                    check(file.InitCheck()==B_OK && BTranslatorRoster::Default()->Translate(&stream,nullptr,nullptr,&file,B_PNG_FORMAT)==B_OK,"write typography screenshot");
                }
                result=0; fTimer.reset(); std::printf("Native typography: %d checks passed (glyphs, rich styles, subject, typing, clipboard, undo/redo, MIME and run coalescing)\n",checks);
                std::fflush(stdout); PostMessage(B_QUIT_REQUESTED); fStep=6;
            }
        } catch (const std::exception& error) { fail(error.what()); }
    }
private:
    void fail(const char* error) { std::fprintf(stderr,"FAIL typography step %d: %s\n",fStep,error); fTimer.reset(); PostMessage(B_QUIT_REQUESTED); }
    std::string fDirectory,fOriginal;
    std::shared_ptr<mail::Store> fStore;
    mail::Account fAccount;
    mail::Id fDraft=0;
    BWindow* fOwner=nullptr;
    mail::ui::ComposerWindow* fComposer=nullptr;
    std::unique_ptr<BMessageRunner> fTimer;
    int fStep=0,fTicks=0;
};
}
int main()
{
    char directory[]="/tmp/mail-typography-XXXXXX"; if (!mkdtemp(directory)) return 2;
    int result; { Typography app(directory); app.Run(); result=app.result; }
    for (const char* file:{"mail.db","mail.db-wal","mail.db-shm"}) unlink((std::string(directory)+"/"+file).c_str());
    rmdir(directory); return result;
}
