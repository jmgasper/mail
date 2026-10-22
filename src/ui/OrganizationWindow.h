#pragma once
#include "mail/Store.h"
#include <Window.h>
#include <Messenger.h>
#include <memory>
class BListView;
class BTextControl;
class BTextView;
class BMenuField;
class BCheckBox;
class BButton;
namespace mail::ui {
enum class OrganizationKind { Rules,SmartFolders,Tags };
constexpr uint32 kOrganizationChanged='ogch',kOrganizationReload='ogrf',kOrganizationNew='ognw',kOrganizationActivate='ogac';
class OrganizationWindow : public BWindow {
public:
    OrganizationWindow(BWindow*,std::shared_ptr<Store>,OrganizationKind,const Query& seed={},bool create=false);
    ~OrganizationWindow() override;
    void MessageReceived(BMessage*) override;
    bool QuitRequested() override;
private:
    void Reload();
    void Load(int32);
    void New();
    void Save();
    void Remove();
    bool CanLeave();
    std::string Signature() const;
    void UpdateButtons();
    void UpdateStatus();
    void BuildScope(Id);
    void BuildFolders(const std::string&);
    Id Scope() const;
    std::string Folder() const;
    std::shared_ptr<Store> fStore;
    BMessenger fOwner;
    OrganizationKind fKind;
    Query fSeed;
    std::vector<Filter> fRules;
    std::vector<SmartFolder> fSmart;
    std::vector<TagInfo> fTags;
    BListView* fList;
    BTextControl *fName,*fSender=nullptr,*fSubject=nullptr,*fTag=nullptr,*fSearch=nullptr;
    BMenuField *fScope,*fFolder=nullptr;
    BCheckBox *fRead=nullptr,*fStar=nullptr,*fStop=nullptr,*fEnabled=nullptr,*fAttachments=nullptr,*fSnoozed=nullptr,*fIncludeSnoozed=nullptr;
    BTextView* fStatus;
    BButton *fSave,*fRemove,*fUp=nullptr,*fDown=nullptr;
    Id fId=0,fTagAccount=0;
    int32 fSelection=-1;
    std::string fTagKey,fBaseline;
};
}
