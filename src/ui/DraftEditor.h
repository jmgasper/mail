#pragma once
#include "Emoji.h"
#include "InlineImages.h"
#include "mail/Compose.h"
#include <map>
#include <deque>
#include <memory>
#include <optional>

namespace mail::ui {
constexpr uint32 kEditorChanged='cedt',kEditorError='cerr';
constexpr uint32 kEditorUndo='cund'+1,kEditorRedo='crdo';
class DraftEditor : public EmojiTextView {
public:
    DraftEditor();
    ~DraftEditor() override;
    void AttachedToWindow() override;
    void Draw(BRect) override;
    void FrameResized(float,float) override;
    bool CanEndLine(int32) override;
    BRect InlineImageFrame(int32 offset) const;
    void Load(const Draft&);
    void Save(Draft&);
    uint64 ChangeCount() const { return fChangeCount; }
    void Toggle(uint16 face);
    void Link(const std::string&);
    void ReplaceSelection(const std::string&);
    const Attachment* AttachmentAt(int32) const;
    bool IsInline(int32) const;
    void AddAttachments(std::vector<Attachment>,int32 at=-1);
    void RemoveAttachment(int32);
    void ToggleImage(int32);
    int64 RememberPosition(int32);
    int32 TakePosition(int64);
    void ForgetPosition(int64);
    void Copy(BClipboard*) override;
    void Cut(BClipboard*) override;
    void Paste(BClipboard*) override;
    void Undo(BClipboard*) override;
    void Redo();
    bool CanUndo() const { return !fUndo.empty(); }
    bool CanRedo() const { return !fRedo.empty(); }
    void Select(int32 begin,int32 end) override;
    void MouseDown(BPoint) override;
    void MouseUp(BPoint) override;
    void MakeFocus(bool focus=true) override;
    void KeyDown(const char*,int32) override;
    void MessageReceived(BMessage*) override;
protected:
    void InsertText(const char*,int32,int32,const text_run_array*) override;
    void DeleteText(int32,int32) override;
private:
    struct LinkSpan { int32 begin,end; std::string url; };
    struct State {
        Draft body;
        std::vector<std::shared_ptr<const Attachment>> files;
        int32 begin=0,end=0;
        BFont insertionFont;
    };
    class Action;
    State Capture();
    BFont InsertionFont() const;
    BFont ContentFont(int32) const;
    void ApplyImageLayout();
    void RemoveImageLayout();
    void SnapImageSelection(int direction=0);
    void SetInsertionFont(const BFont&);
    void Restore(const State&);
    void LoadBody(const Draft&);
    void SaveBody(Draft&);
    void BeginAction();
    void EndAction(bool);
    bool SameContent(const State&,const State&) const;
    void TrimHistory();
    std::size_t HistoryBytes() const;
    void StepHistory(std::deque<State>&,std::deque<State>&);
    void NotifyChanged();
    void Validate();
    void PlaceImage(int32,int32);
    void RemovePlacements(const std::string&);
    void ExpandSelection(int32&,int32&);
    void ClearLinks(int32,int32);
    void MovePositions(int32,int32,int32);
    void Report(const std::exception&);
    std::vector<LinkSpan> fLinks;
    std::vector<InlineImage> fImages;
    std::vector<std::shared_ptr<const Attachment>> fFiles;
    struct ImageLayout {
        InlineImage image;
        BFont textFont;
        std::shared_ptr<InlineObjectFont> objectFont;
        const Attachment* file=nullptr;
        float width=0,height=0;
    };
    std::vector<ImageLayout> fImageLayout;
    std::map<uint16,std::shared_ptr<InlineObjectFont>> fObjectFonts;
    struct ScrollAnchor { int32 offset; float delta; };
    std::optional<ScrollAnchor> fImageScrollAnchor;
    InlineImageCache fImageCache;
    bool fApplyingImageLayout=false;
    struct Position { int32 offset; bigtime_t created; };
    std::map<int64,Position> fPositions;
    int64 fNextPosition=0;
    std::optional<State> fBefore;
    std::optional<BFont> fInsertionFont;
    int32 fInsertionOffset=0;
    std::deque<State> fUndo,fRedo;
    int fActionDepth=0;
    uint64 fChangeCount=0;
    bool fChanged=false,fRestoring=false;
    bool fKeyAction=false,fKeepUndo=false;
    int fTypingKind=0;
    int32 fTypingCaret=0;
    bigtime_t fTypingTime=0;
};
}
