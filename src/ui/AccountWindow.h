#pragma once
#include "mail/Store.h"
#include "Credentials.h"
#include <Messenger.h>
#include <Window.h>
#include <memory>
#include <optional>
#include <thread>

class BListView;
class BTextControl;
class BMenuField;
class BCheckBox;
class BCardView;
class BButton;
class BStringView;
class BTextView;
class BFilePanel;
namespace mail::ui {
constexpr uint32 kAccountsChanged='macC';
constexpr uint32 kAccountsClosed='macX';
class AccountWindow : public BWindow {
public:
    AccountWindow(BWindow* owner,std::shared_ptr<Store> store,GoogleOptions google={});
    ~AccountWindow() override;
    void MessageReceived(BMessage*) override;
    bool QuitRequested() override;
private:
    struct ServerFields {
        BTextControl *host,*port,*username,*password,*certificate;
        BMenuField* security;
        BButton *choose,*fetch;
        bool outgoing;
    };
    void Reload();
    void Load(const Account&);
    void Save();
    void UpdateProvider();
    void StartGoogle(bool revoke=false);
    void FinishGoogle();
    void CommitGoogle();
    void SetGoogleBusy(bool);
    Endpoint ReadEndpoint(const ServerFields&) const;
    BView* MakeServer(const char* name,ServerFields& fields,bool outgoing);
    void SetServer(ServerFields&,const Endpoint&);
    void SetDefaultPort(ServerFields&);
    void ChooseCertificate(bool outgoing);
    void CertificateChosen(BMessage*);
    void FetchCertificate(bool outgoing);
    void FinishCertificate(BMessage*);
    void UpdateCertificateTip(ServerFields&);
    std::string TrustedCertificate(ServerFields&,const std::string& host,int port);
    int ProviderType() const;
    std::shared_ptr<Store> fStore;
    BMessenger fOwner;
    std::vector<Account> fAccounts;
    Account fEditing;
    BListView* fList;
    BTextControl *fName,*fEmail;
    BMenuField *fProvider,*fSentMode;
    BTextControl* fSentFolder;
    BCheckBox *fUnified,*fSamePassword;
    ServerFields fIncoming{},fOutgoing{};
    GoogleOptions fGoogle;
    BCardView* fProviderCards;
    BButton *fGoogleSignIn,*fGoogleRevoke,*fSave,*fAdd,*fRemove;
    BStringView *fNote,*fHint;
    BTextView* fGoogleStatus;
    std::thread fGoogleWorker,fCertificateWorker;
    std::atomic<bool> fCancelCertificate{false};
    std::unique_ptr<BFilePanel> fCertificatePanel;
    bool fFetching=false;
    int fProviderType=0;
    std::atomic<bool> fCancelGoogle{false};
    bool fGoogleBusy=false,fRevoking=false,fRevoked=false,fCloseAfterGoogle=false;
    Account fGoogleAccount;
    std::optional<GoogleTokens> fAuthorized;
    std::optional<GmailProfile> fGoogleProfile;
    std::string fGoogleError;
};
}
