#pragma once
#include "mail/TextClusters.h"
#include <TextView.h>
#include <memory>
#include <optional>
#include <string>

namespace mail::ui {
class GraphemeEditing {
public:
    explicit GraphemeEditing(BTextView& view):fView(view){}
    void Invalidate(){fDirty=true;fAnchor.reset();}
    void ObserveText();
    const TextClusters& Clusters();
    void Normalize(int direction=0);
    bool KeyDown(const char*,int32);
private:
    BTextView& fView;
    std::string fText;
    std::unique_ptr<TextClusters> fClusters;
    bool fDirty=true;
    std::optional<int32> fAnchor;
    int32 fLastBegin=-1,fLastEnd=-1;
};
void InstallGraphemeEditing(BTextView&);
}
