#pragma once
#include "mail/Model.h"
#include <Bitmap.h>
#include <Font.h>
#include <Messenger.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace mail::ui {
constexpr uint32 kInlineImagesReady='imgr';
// An application-private blank font gives BTextView a real object advance and
// line height. It is never saved in drafts, exported, or installed system-wide.
class InlineObjectFont {
public:
    explicit InlineObjectFont(uint16 advance);
    ~InlineObjectFont();
    const BFont& Font() const { return fFont; }
private:
    BFont fFont;
    bool fLoaded=false;
};
class InlineImageCache {
public:
    InlineImageCache()=default;
    ~InlineImageCache();
    void Attach(BMessenger);
    void Sync(const std::vector<std::shared_ptr<const Attachment>>& visible,const std::vector<std::shared_ptr<const Attachment>>& retained);
    const BBitmap* Bitmap(const Attachment*) const;
private:
    struct Entry {
        std::shared_ptr<const Attachment> file;
        std::unique_ptr<BBitmap> bitmap;
        std::atomic<bool> canceled{false},ready{false};
    };
    void Work();
    std::map<const Attachment*,std::shared_ptr<Entry>> fEntries;
    std::deque<std::shared_ptr<Entry>> fPending;
    BMessenger fTarget;
    std::mutex fMutex;
    std::condition_variable fWake;
    bool fStop=false,fAttached=false;
    std::thread fWorker;
};
}
