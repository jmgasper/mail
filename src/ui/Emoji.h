#pragma once
#include <Font.h>
#include <StringView.h>
#include <TextView.h>
#include <string>
#include <memory>

namespace mail::ui {
// The font is an application resource, never installed into the system.
// It is not used where the system draws emoji in color itself.
bool SystemDrawsColorEmoji();
bool EmojiFontAvailable();
bool IsEmojiFont(const BFont&);
BFont EmojiFont(const BFont& source);
uint16 TextFace(const BFont&);
void SetTextFace(BFont&,uint16 face);
void ApplyEmojiFonts(BTextView&,int32 begin=0,int32 end=-1);
void DrawEmojiText(BView&,const std::string&,const BFont&,BPoint,float width);
void CopyEmojiText(BTextView&,BClipboard*,bool cut=false);

class GraphemeEditing;
class EmojiTextView : public BTextView {
public:
    explicit EmojiTextView(const char* name);
    ~EmojiTextView() override;
    void Copy(BClipboard*) override;
    void Cut(BClipboard*) override;
    void Paste(BClipboard*) override;
    void KeyDown(const char*,int32) override;
    void MouseDown(BPoint) override;
    void MouseUp(BPoint) override;
    bool CanEndLine(int32) override;
    void SnapToGraphemes();
    int32 GraphemeStart(int32);
    int32 GraphemeEnd(int32);
protected:
    void InsertText(const char*,int32 length,int32 offset,const text_run_array*) override;
    void DeleteText(int32,int32) override;
private:
    std::unique_ptr<GraphemeEditing> fGraphemeEditing;
};
class EmojiStringView : public BStringView {
public:
    EmojiStringView(const char* name,const char* text) : BStringView(name,text) {}
    void Draw(BRect) override;
};
}
