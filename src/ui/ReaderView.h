#pragma once
#include "mail/Preview.h"
#include <View.h>
#include <set>
#include <string>
#include <vector>

class BCardLayout;
class BTextView;
class BWebKitView;
namespace mail::ui {
// Sent to the window when the user clicks an http(s) or mailto link. Field: "url".
constexpr uint32 kReaderLink='rdlk';
// Sent to the window when formatted viewing becomes unavailable.
constexpr uint32 kReaderChanged='rdch';
// The embedded web engine is shared by all readers and set up on the
// application thread. Without it, readers fall back to plain text.
bool InitializeReaderEngine();
// Releases the engine. Keep the application running while ReaderEngineBusy().
void ShutdownReaderEngine();
bool ReaderEngineBusy();

// Shows a conversation (or a draft) formatted in the main window. The plain
// text is always kept in the "message reader" text view, which is shown when
// the plain text view is chosen or the web engine is unavailable.
class ReaderView : public BView {
public:
    ReaderView();
    void AttachedToWindow() override;
    void MessageReceived(BMessage*) override;
    void SetEmpty(const char* text);
    void SetConversation(std::vector<ReceivedPreview> messages,const std::string& plainText,std::set<Id> expanded);
    void SetDraft(const Draft&,const std::string& plainText);
    void SetPlain(bool);
    bool Plain() const { return fPlain; }
    bool CanFormat() const { return fWeb!=nullptr && !fWebFailed; }
    BTextView* TextView() const { return fText; }
private:
    enum class Mode { Empty, Conversation, Draft };
    void Render();
    void Serve(BMessage&);
    void ShowCard();
    BCardLayout* fCards=nullptr;
    BTextView* fText=nullptr;
    BView* fTextScroll=nullptr;
    BWebKitView* fWeb=nullptr;
    Mode fMode=Mode::Empty;
    std::vector<ReceivedPreview> fMessages;
    std::set<Id> fExpanded;
    Draft fDraft;
    std::string fRoot,fHtml;
    bool fPlain=false;
    int fRendererExits=0;
    bool fWebFailed=false;
};
}
