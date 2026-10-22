#include "GraphemeEditing.h"
#include "Emoji.h"
#include <Clipboard.h>
#include <Message.h>
#include <MessageFilter.h>
#include <Messenger.h>
#include <Window.h>
#include <algorithm>
#include <cstring>

namespace mail::ui {
namespace {
constexpr uint32 kSnapGraphemes='gsnp';
uint32 keyModifiers(BTextView& view)
{
    auto* message=view.Window()?view.Window()->CurrentMessage():nullptr;
    return message && message->what==B_KEY_DOWN?message->GetInt32("modifiers",modifiers()):modifiers();
}
class GraphemeFilter:public BMessageFilter {
public:
    explicit GraphemeFilter(BTextView& view):BMessageFilter(B_ANY_DELIVERY,B_ANY_SOURCE),fView(view),fEditing(view){}
    filter_result Filter(BMessage* message,BHandler**) override
    {
        try {
            if(message->what==B_KEY_DOWN) {
                fEditing.ObserveText();const char* bytes=message->GetString("bytes","");
                if(fEditing.KeyDown(bytes,std::strlen(bytes)))return B_SKIP_MESSAGE;
                BMessage snap(kSnapGraphemes);
                snap.AddInt32("direction",bytes[0]==B_LEFT_ARROW || bytes[0]==B_UP_ARROW?-1:1);
                if(fView.Looper())fView.Looper()->PostMessage(&snap,&fView);
            }else if(message->what==B_MOUSE_UP || message->what==B_MOUSE_DOWN)BMessenger(&fView).SendMessage(kSnapGraphemes);
            else if(message->what==B_CUT || message->what==B_COPY || message->what==B_PASTE){
                fEditing.ObserveText();fEditing.Normalize();
                if(message->what==B_COPY || (message->what==B_CUT && fView.IsEditable())) {CopyEmojiText(fView,be_clipboard,message->what==B_CUT);return B_SKIP_MESSAGE;}
            }
            else if(message->what==kSnapGraphemes){fEditing.ObserveText();fEditing.Normalize(message->GetInt32("direction",0));return B_SKIP_MESSAGE;}
        }catch(const std::exception&) {return B_SKIP_MESSAGE;}
        return B_DISPATCH_MESSAGE;
    }
private:
    BTextView& fView;
    GraphemeEditing fEditing;
};
}
void GraphemeEditing::ObserveText()
{
    if(fText.size()!=static_cast<std::size_t>(fView.TextLength()) || std::memcmp(fText.data(),fView.Text(),fText.size())!=0)Invalidate();
}
const TextClusters& GraphemeEditing::Clusters()
{
    if(fDirty || !fClusters){fText.assign(fView.Text(),fView.TextLength());fClusters=std::make_unique<TextClusters>(fText);fDirty=false;}
    return *fClusters;
}
void GraphemeEditing::Normalize(int direction)
{
    int32 begin,end;fView.GetSelection(&begin,&end);const auto& clusters=Clusters();
    if(begin!=end){begin=clusters.Floor(begin);end=clusters.Ceil(end);}
    else {
        int32 floor=clusters.Floor(begin),ceil=clusters.Ceil(begin);
        begin=end=direction<0?floor:direction>0?ceil:(begin-floor<ceil-begin?floor:ceil);
    }
    // A range Select() does not move BTextView's internal caret. Native Paste
    // and Cut use that caret after deleting the selection, so keep it at the
    // active edge before restoring the visible range.
    int32 caret=direction<0?begin:direction>0?end:(fAnchor && *fAnchor==end?begin:end);
    fView.Select(caret,caret);fView.Select(begin,end);
}
bool GraphemeEditing::KeyDown(const char* bytes,int32 length)
{
    if(length<=0)return false;
    auto key=static_cast<uint8>(bytes[0]);auto flags=keyModifiers(fView);bool word=flags&(B_COMMAND_KEY|B_CONTROL_KEY|B_OPTION_KEY);
    int32 begin,end;fView.GetSelection(&begin,&end);const auto& clusters=Clusters();
    if((key==B_LEFT_ARROW || key==B_RIGHT_ARROW) && !word) {
        bool left=key==B_LEFT_ARROW;
        int32 caret;
        if(flags&B_SHIFT_KEY) {
            if(!fAnchor || begin!=fLastBegin || end!=fLastEnd)fAnchor=left?end:begin;
            caret=begin==*fAnchor?end:begin;
            caret=left?clusters.Before(caret):clusters.After(caret);
            begin=std::min(caret,*fAnchor);end=std::max(caret,*fAnchor);
        }else {
            fAnchor.reset();begin=end=begin!=end?(left?clusters.Floor(begin):clusters.Ceil(end)):(left?clusters.Before(begin):clusters.After(end));
            caret=begin;
        }
        fView.Select(caret,caret);fView.Select(begin,end);fLastBegin=begin;fLastEnd=end;fView.ScrollToSelection();return true;
    }
    fAnchor.reset();
    if(fView.IsEditable() && (key==B_BACKSPACE || key==B_DELETE) && !word) {
        if(begin==end) {
            if(!clusters.IsBoundary(begin)){begin=clusters.Floor(begin);end=clusters.Ceil(end);}
            else if(key==B_BACKSPACE)begin=clusters.Before(begin);else end=clusters.After(end);
        }else{begin=clusters.Floor(begin);end=clusters.Ceil(end);}
        fView.Select(begin,begin);fView.Select(begin,end);
    }else if(key>=B_SPACE || key==B_ENTER || key==B_TAB)Normalize();
    return false;
}
void InstallGraphemeEditing(BTextView& view)
{
    auto* filters=view.FilterList();
    if(filters)for(int32 i=0;i<filters->CountItems();++i)if(dynamic_cast<GraphemeFilter*>(static_cast<BMessageFilter*>(filters->ItemAt(i))))return;
    view.AddFilter(new GraphemeFilter(view));
}
}
