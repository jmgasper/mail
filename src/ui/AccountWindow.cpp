#include "AccountWindow.h"
#include "Credentials.h"
#include "mail/Sync.h"
#include <Alert.h>
#include <Button.h>
#include <CheckBox.h>
#include <CardLayout.h>
#include <CardView.h>
#include <GroupView.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <ScrollView.h>
#include <StringView.h>
#include <TabView.h>
#include <TextControl.h>
#include <TextView.h>
#include <algorithm>
#include <ctime>

namespace mail::ui {
namespace {
enum : uint32 { kSelect='acSl',kNew='acNw',kSave='acSv',kRemove='acRm',kProvider='acPr',kGoogle='acGo',kGoogleDone='acGd',kRevoke='acRv' };
// Keep a stable area for asynchronous status changes. BTextView's read-only
// height-for-width layout can retain the previous one-line height in BCardView.
class GoogleStatus : public BTextView {
public:
    GoogleStatus() : BTextView("google status") {}
    bool HasHeightForWidth() override { return false; }
};
void clear(BListView* list) { while (auto* item=list->RemoveItem(int32(0))) delete item; }
BMenuField* securityMenu()
{
    auto* menu=new BPopUpMenu("Connection"); menu->AddItem(new BMenuItem("TLS",nullptr));
    menu->AddItem(new BMenuItem("STARTTLS",nullptr)); menu->ItemAt(0)->SetMarked(true);
    return new BMenuField("security","Connection:",menu);
}
}
AccountWindow::AccountWindow(BWindow* owner,std::shared_ptr<Store> store,GoogleOptions google)
    : BWindow(BRect(0,0,720,560),"Mail accounts",B_TITLED_WINDOW_LOOK,B_MODAL_SUBSET_WINDOW_FEEL,
        B_AUTO_UPDATE_SIZE_LIMITS|B_CLOSE_ON_ESCAPE),fStore(std::move(store)),fOwner(owner),fGoogle(std::move(google))
{
    AddToSubset(owner);
    fList=new BListView("accounts"); fList->SetSelectionMessage(new BMessage(kSelect));
    auto* scroll=new BScrollView("accounts scroll",fList,0,false,true);
    scroll->SetExplicitMinSize(BSize(150,250));
    fName=new BTextControl("account name","Name:","",nullptr);
    fEmail=new BTextControl("email","Email:","",nullptr);
    for (auto* field : {fName,fEmail}) {
        field->SetExplicitMinSize(BSize(380,B_SIZE_UNSET));
        field->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED,B_SIZE_UNSET));
    }
    auto* providers=new BPopUpMenu("Account type");
    for (const char* label : {"IMAP","POP3","Proton Mail Bridge","Google (Gmail)"}) providers->AddItem(new BMenuItem(label,new BMessage(kProvider)));
    providers->ItemAt(0)->SetMarked(true); fProvider=new BMenuField("provider","Account type:",providers);
    fUnified=new BCheckBox("Include in All inboxes",nullptr);
    fSamePassword=new BCheckBox("Use incoming password for sending",nullptr); fSamePassword->SetValue(B_CONTROL_ON);
    auto* tabs=new BTabView("servers");
    tabs->AddTab(MakeServer("Incoming",fIncoming)); tabs->TabAt(0)->SetLabel("Incoming");
    tabs->AddTab(MakeServer("Outgoing",fOutgoing)); tabs->TabAt(1)->SetLabel("Outgoing");
    auto* sentModes=new BPopUpMenu("Sent copies");
    for (const char* label:{"Automatic","Save to IMAP server","Server saves Sent","Keep on this computer"}) sentModes->AddItem(new BMenuItem(label,nullptr));
    sentModes->ItemAt(0)->SetMarked(true); fSentMode=new BMenuField("sent copies","Sent copies:",sentModes);
    fSentMode->SetToolTip("Automatic saves to IMAP, lets Proton Bridge save its own copy, and keeps POP3 Sent mail on this computer.");
    fSentFolder=new BTextControl("sent folder","Sent folder:","",nullptr);
    fSentFolder->SetToolTip("Optional server mailbox name. Leave blank to use the server's advertised Sent folder.");
    auto* servers=new BGroupView("server settings",B_VERTICAL);
    BLayoutBuilder::Group<>(servers).Add(tabs).Add(fSamePassword).Add(fSentMode).Add(fSentFolder);
    auto* googlePanel=new BGroupView("google settings",B_VERTICAL);
    fGoogleStatus=new GoogleStatus(); fGoogleStatus->MakeEditable(false); fGoogleStatus->SetWordWrap(true);
    fGoogleStatus->SetViewUIColor(B_PANEL_BACKGROUND_COLOR); fGoogleStatus->SetLowUIColor(B_PANEL_BACKGROUND_COLOR);
    auto text=ui_color(B_PANEL_TEXT_COLOR); fGoogleStatus->SetFontAndColor(be_plain_font,B_FONT_ALL,&text);
    fGoogleStatus->SetExplicitMinSize(BSize(300,100)); fGoogleStatus->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED,150));
    fGoogleSignIn=new BButton("google sign in","Sign in with Google",new BMessage(kGoogle));
    fGoogleRevoke=new BButton("google revoke","Revoke Google access…",new BMessage(kRevoke));
    BLayoutBuilder::Group<>(googlePanel).SetInsets(B_USE_DEFAULT_SPACING).Add(fGoogleStatus)
        .AddGroup(B_HORIZONTAL).Add(fGoogleSignIn).Add(fGoogleRevoke).AddGlue().End().AddGlue();
    fProviderCards=new BCardView("provider settings"); fProviderCards->AddChild(servers); fProviderCards->AddChild(googlePanel);
    fNote=new BStringView("password note","");
    fNote->SetExplicitMinSize(BSize(100,B_SIZE_UNSET)); fNote->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED,B_SIZE_UNSET));
    fHint=new BStringView("password hint","");
    fHint->SetExplicitMinSize(BSize(100,B_SIZE_UNSET)); fHint->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED,B_SIZE_UNSET));
    fSave=new BButton("Save account",new BMessage(kSave));
    fAdd=new BButton("Add",new BMessage(kNew)); fRemove=new BButton("Remove",new BMessage(kRemove));
    BLayoutBuilder::Group<>(this,B_VERTICAL).SetInsets(B_USE_WINDOW_INSETS)
        .AddGroup(B_HORIZONTAL)
            .AddGroup(B_VERTICAL).Add(scroll).AddGroup(B_HORIZONTAL)
                .Add(fAdd).Add(fRemove).End().End()
            .AddGroup(B_VERTICAL, B_USE_DEFAULT_SPACING,3)
                .Add(fName).Add(fEmail).Add(fProvider).Add(fUnified).Add(fProviderCards).End()
        .End()
        .Add(fNote).Add(fHint)
        .AddGroup(B_HORIZONTAL).AddGlue().Add(new BButton("Done",new BMessage(B_QUIT_REQUESTED))).Add(fSave).End();
    SetDefaultButton(fSave); CenterIn(owner->Frame()); Reload();
    if (fAccounts.empty()) Load({}); else fList->Select(0);
}
AccountWindow::~AccountWindow()
{
    fCancelGoogle=true; if (fGoogleWorker.joinable()) fGoogleWorker.join();
    clear(fList); fOwner.SendMessage(kAccountsClosed);
}
bool AccountWindow::QuitRequested()
{
    if (!fGoogleBusy) return true;
    fCloseAfterGoogle=true; fCancelGoogle=true; fGoogleStatus->SetText("Finishing Google sign-in cancellation…"); return false;
}
BView* AccountWindow::MakeServer(const char* name,ServerFields& f)
{
    auto* view=new BGroupView(name,B_VERTICAL);
    f.host=new BTextControl("host","Server:","",nullptr);
    f.port=new BTextControl("port","Port:","",nullptr);
    f.username=new BTextControl("username","Username:","",nullptr);
    f.password=new BTextControl("password","Password:","",nullptr); f.password->TextView()->HideTyping(true);
    f.certificate=new BTextControl("certificate","CA certificate:","",nullptr);
    for (auto* field : {f.host,f.port,f.username,f.password,f.certificate})
        field->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED,B_SIZE_UNSET));
    f.certificate->SetToolTip("For a local Bridge or private mail server, enter the path to its trusted PEM certificate. Leave blank for system trust.");
    f.security=securityMenu();
    BLayoutBuilder::Group<>(view).SetInsets(B_USE_DEFAULT_SPACING).Add(f.host).Add(f.port).Add(f.security)
        .Add(f.username).Add(f.password).Add(f.certificate).AddGlue();
    return view;
}
void AccountWindow::SetServer(ServerFields& fields,const Endpoint& endpoint)
{
    fields.host->SetText(endpoint.host.c_str()); fields.port->SetText(std::to_string(endpoint.port).c_str());
    fields.username->SetText(endpoint.username.c_str()); fields.password->SetText("");
    fields.certificate->SetText(endpoint.certificate.c_str()); fields.security->Menu()->ItemAt(endpoint.security==Security::TLS?0:1)->SetMarked(true);
}
void AccountWindow::Reload()
{
    clear(fList); fAccounts.clear();
    for (const auto& a : fStore->accounts()) if (a.provider!=Provider::Local) {
        fAccounts.push_back(a); fList->AddItem(new BStringItem(a.name.c_str()));
    }
}
void AccountWindow::Load(const Account& value)
{
    fEditing=value;
    if (!fEditing.id) {
        fEditing.provider=Provider::Imap; fEditing.incoming.port=993;
        fEditing.outgoing.port=587; fEditing.outgoing.security=Security::StartTLS;
    }
    fName->SetText(fEditing.name.c_str()); fEmail->SetText(fEditing.email.c_str());
    int type=fEditing.provider==Provider::Gmail?3:(fEditing.provider==Provider::Pop3?1:(fEditing.provider==Provider::ProtonBridge?2:0));
    fProvider->Menu()->ItemAt(type)->SetMarked(true); fUnified->SetValue(fEditing.unified);
    fSentMode->Menu()->ItemAt(static_cast<int>(fEditing.sentMode))->SetMarked(true); fSentFolder->SetText(fEditing.sentFolder.c_str());
    SetServer(fIncoming,fEditing.incoming); SetServer(fOutgoing,fEditing.outgoing);
    fSamePassword->SetValue(fEditing.id?B_CONTROL_OFF:B_CONTROL_ON);
    UpdateProvider();
}
void AccountWindow::UpdateProvider()
{
    auto type=fProvider->Menu()->IndexOf(fProvider->Menu()->FindMarked());
    bool google=type==3;
    fSentMode->SetEnabled(type==0 || type==2); fSentFolder->SetEnabled(type==0 || type==2);
    if (google && !fEditing.id) fEmail->SetText("");
    fProviderCards->CardLayout()->SetVisibleItem(google?1:0);
    fEmail->SetEnabled(!google && !fGoogleBusy); fProvider->SetEnabled(!fEditing.id && !fGoogleBusy);
    fSave->SetEnabled(!fGoogleBusy && (!google || fEditing.id));
    fGoogleRevoke->SetEnabled(google && fEditing.id && !fGoogleBusy);
    fGoogleSignIn->SetEnabled(!fGoogleBusy); fGoogleSignIn->SetLabel(fEditing.id?"Sign in again":"Sign in with Google");
    fNote->SetText(google?"Google authorization is saved in the system key store.":"Passwords are saved in the system key store.");
    fHint->SetText(google?"Your email address is supplied by Google.":"Leave a password blank to keep the saved one.");
    std::string status=fEditing.id && google?"Google account: "+fEditing.email+"\nSign in again if access has expired.":"Choose an account and allow Mail to access Gmail in your browser.";
    fGoogleStatus->SetText(status.c_str()); SetDefaultButton(google && !fEditing.id?fGoogleSignIn:fSave);
}
void AccountWindow::SetGoogleBusy(bool busy)
{
    fGoogleBusy=busy; fName->SetEnabled(!busy); fUnified->SetEnabled(!busy); fAdd->SetEnabled(!busy); fRemove->SetEnabled(!busy);
    fProvider->SetEnabled(!busy && !fEditing.id); fSave->SetEnabled(!busy && fEditing.id);
    fGoogleRevoke->SetEnabled(!busy && fEditing.id); fGoogleSignIn->SetEnabled(true);
    fGoogleSignIn->SetLabel(busy?"Cancel":(fEditing.id?"Sign in again":"Sign in with Google"));
}
void AccountWindow::StartGoogle(bool revoke)
{
    if (fGoogleBusy) { fCancelGoogle=true; fGoogleStatus->SetText("Cancelling Google request…"); return; }
    if (fProvider->Menu()->IndexOf(fProvider->Menu()->FindMarked())!=3) return;
    if (revoke && !fEditing.id) return;
    if (revoke && (new BAlert("Revoke Google access","Revoke this Mail project's Google access for this account? This also invalidates its access on other devices. Cached mail stays on this computer.","Cancel","Revoke",nullptr,B_WIDTH_AS_USUAL,B_WARNING_ALERT))->Go()!=1) return;
    GoogleClient client;
    if (!revoke) client=fGoogle.client.id.empty()?ReadGoogleClient():fGoogle.client;
    if (fGoogleWorker.joinable()) fGoogleWorker.join();
    fGoogleAccount=fEditing; fGoogleAccount.name=fName->Text(); fGoogleAccount.unified=fUnified->Value();
    fGoogleAccount.provider=Provider::Gmail; fGoogleAccount.incoming={}; fGoogleAccount.outgoing={};
    fCancelGoogle=false; fRevoking=revoke; fRevoked=false; fGoogleError.clear(); fAuthorized.reset(); fGoogleProfile.reset();
    SetGoogleBusy(true); fGoogleStatus->SetText(revoke?"Revoking Google access…":"Complete sign-in in your browser. You can cancel here.");
    auto options=fGoogle; auto account=fGoogleAccount; BMessenger target(this);
    fGoogleWorker=std::thread([this,target,options,client,account,revoke] {
        try {
            if (revoke) { RevokeGoogleAccess(account,fCancelGoogle,options.endpoints); fRevoked=true; }
            else {
                auto launch=options.launchBrowser?options.launchBrowser:OpenGoogleBrowser;
                auto tokens=authorizeGoogle(client,launch,fCancelGoogle,account.email,options.endpoints);
                GmailApi api([&](bool force) {
                    if (force || tokens.expires<=std::time(nullptr)+60) tokens=refreshGoogle(tokens,fCancelGoogle,options.endpoints);
                    return tokens.access;
                },fCancelGoogle,options.endpoints);
                auto profile=api.profile(); if (account.id) verifyGmailIdentity(account,profile);
                fAuthorized=std::move(tokens); fGoogleProfile=std::move(profile);
            }
        } catch (const std::exception& error) { fGoogleError=error.what(); }
        target.SendMessage(kGoogleDone);
    });
}
void AccountWindow::CommitGoogle()
{
    if (!fAuthorized || !fGoogleProfile) throw std::runtime_error("Google sign-in did not return an account.");
    Account account=fGoogleAccount,previous=fEditing;
    if (!account.id) {
        // Selecting an already connected Google identity reauthorizes that
        // account rather than creating duplicate mailbox and token stores.
        for (const auto& existing : fStore->accounts()) if (existing.provider==Provider::Gmail) {
            try { verifyGmailIdentity(existing,*fGoogleProfile); account=existing; previous=existing; break; }
            catch (const NetworkError&) {}
        }
    }
    account.email=fGoogleProfile->email; if (account.name.empty()) account.name=account.email;
    bool created=!account.id; account.id=fStore->saveAccount(account);
    for (const auto& saved : fStore->accounts()) if (saved.id==account.id) account=saved;
    try { SaveGoogleTokens(account,*fAuthorized); }
    catch (...) { if (created) fStore->removeAccount(account.id); else fStore->saveAccount(previous); throw; }
    fEditing=account; Reload();
    for (std::size_t i=0;i<fAccounts.size();++i) if (fAccounts[i].id==account.id) { fList->Select(i); break; }
    Load(account); fOwner.SendMessage(kAccountsChanged);
    fGoogleStatus->SetText(("Signed in as "+account.email+". Close Accounts to check mail.").c_str());
}
void AccountWindow::FinishGoogle()
{
    if (fGoogleWorker.joinable()) fGoogleWorker.join();
    SetGoogleBusy(false);
    try {
        if (fRevoked) fGoogleStatus->SetText("Google access revoked. Cached mail is still available. Sign in again to reconnect.");
        else if (fCancelGoogle) fGoogleStatus->SetText("Google sign-in cancelled. Your existing account was kept.");
        else if (!fGoogleError.empty()) fGoogleStatus->SetText(fGoogleError.c_str());
        else if (!fRevoking) CommitGoogle();
    } catch (const std::exception& error) { fGoogleStatus->SetText(error.what()); }
    fAuthorized.reset(); fGoogleProfile.reset();
    if (fCloseAfterGoogle) PostMessage(B_QUIT_REQUESTED);
}
Endpoint AccountWindow::ReadEndpoint(const ServerFields& fields) const
{
    Endpoint endpoint;
    endpoint.host=fields.host->Text(); endpoint.username=fields.username->Text();
    if (endpoint.username.empty()) endpoint.username=fEmail->Text();
    endpoint.certificate=fields.certificate->Text();
    std::string port=fields.port->Text();
    if (port.empty() || port.size()>5 || port.find_first_not_of("0123456789")!=std::string::npos)
        throw std::invalid_argument("Enter a port between 1 and 65535.");
    endpoint.port=std::stoi(port);
    endpoint.security=fields.security->Menu()->IndexOf(fields.security->Menu()->FindMarked())==0?Security::TLS:Security::StartTLS;
    return endpoint;
}
void AccountWindow::Save()
{
    Account a=fEditing; a.name=fName->Text(); a.email=fEmail->Text(); a.unified=fUnified->Value();
    int type=fProvider->Menu()->IndexOf(fProvider->Menu()->FindMarked());
    if (type==3) {
        if (!a.id) { StartGoogle(); return; }
        a.email=fEditing.email; a.provider=Provider::Gmail; fStore->saveAccount(a); fEditing=a; Reload();
        for (std::size_t i=0;i<fAccounts.size();++i) if (fAccounts[i].id==a.id) { fList->Select(i); break; }
        Load(a); fOwner.SendMessage(kAccountsChanged); return;
    }
    a.provider=type==1?Provider::Pop3:(type==2?Provider::ProtonBridge:Provider::Imap);
    a.sentMode=static_cast<SentCopyMode>(fSentMode->Menu()->IndexOf(fSentMode->Menu()->FindMarked())); a.sentFolder=fSentFolder->Text();
    a.incoming=ReadEndpoint(fIncoming); a.outgoing=ReadEndpoint(fOutgoing); validateAccount(a);
    Credentials credentials{fIncoming.password->Text(),fOutgoing.password->Text()};
    if (a.id && (credentials.incoming.empty() || (!fSamePassword->Value() && credentials.outgoing.empty()))) {
        auto old=LoadCredentials(fEditing);
        if (credentials.incoming.empty()) credentials.incoming=old.incoming;
        if (credentials.outgoing.empty()) credentials.outgoing=old.outgoing;
    }
    if (fSamePassword->Value()) credentials.outgoing=credentials.incoming;
    if (credentials.incoming.empty() || credentials.outgoing.empty()) throw std::invalid_argument("Enter both server passwords, or choose to use the incoming password for sending.");
    bool created=!a.id;
    // Validate/store metadata before touching an existing account's key.
    a.id=fStore->saveAccount(a);
    for (const auto& saved : fStore->accounts()) if (saved.id==a.id) a=saved;
    try { SaveCredentials(a,credentials); }
    catch (...) { if (created) fStore->removeAccount(a.id); else fStore->saveAccount(fEditing); throw; }
    fEditing=a; Reload();
    for (std::size_t i=0;i<fAccounts.size();++i) if (fAccounts[i].id==a.id) { fList->Select(i); break; }
    Load(a); fOwner.SendMessage(kAccountsChanged);
}
void AccountWindow::MessageReceived(BMessage* message)
{
    try {
        if (fGoogleBusy && message->what!=kGoogle && message->what!=kGoogleDone && message->what!=B_QUIT_REQUESTED) {
            if (message->what==kSelect) {
                int32 index=-1;
                for (std::size_t i=0;i<fAccounts.size();++i) if (fAccounts[i].id==fEditing.id) index=static_cast<int32>(i);
                if (fList->CurrentSelection()!=index) { if (index<0) fList->DeselectAll(); else fList->Select(index); }
            }
            if (message->what==kSelect || message->what==kNew || message->what==kSave || message->what==kRemove || message->what==kProvider || message->what==kRevoke) return;
        }
        switch (message->what) {
            case kGoogle: StartGoogle(); break;
            case kGoogleDone: FinishGoogle(); break;
            case kRevoke: StartGoogle(true); break;
            case kSelect: {
                int32 index=fList->CurrentSelection(); if (index>=0 && static_cast<std::size_t>(index)<fAccounts.size()) Load(fAccounts[index]); break;
            }
            case kNew: fList->DeselectAll(); Load({}); fName->MakeFocus(); break;
            case kSave: Save(); break;
            case kRemove: {
                if (!fEditing.id) break;
                std::string prompt="Remove “"+fEditing.name+"” from Mail? Its downloaded mail, drafts, queued messages, and settings will be removed from this computer. Mail on the server is kept.";
                if ((new BAlert("Remove account",prompt.c_str(),"Cancel","Remove",nullptr,B_WIDTH_AS_USUAL,B_WARNING_ALERT))->Go()!=1) break;
                auto removed=fEditing; fStore->removeAccount(removed.id); Reload(); Load({});
                fOwner.SendMessage(kAccountsChanged); RemoveCredentials(removed); break;
            }
            case kProvider: {
                int type=fProvider->Menu()->IndexOf(fProvider->Menu()->FindMarked());
                if (type==2) {
                    fIncoming.host->SetText("127.0.0.1"); fOutgoing.host->SetText("127.0.0.1");
                    fIncoming.port->SetText("1143"); fOutgoing.port->SetText("1025");
                    fIncoming.security->Menu()->ItemAt(1)->SetMarked(true); fOutgoing.security->Menu()->ItemAt(1)->SetMarked(true);
                } else if (type!=3) {
                    fIncoming.port->SetText(type==1?"995":"993"); fIncoming.security->Menu()->ItemAt(0)->SetMarked(true);
                }
                UpdateProvider();
                break;
            }
            default: BWindow::MessageReceived(message);
        }
    } catch (const std::exception& error) { (new BAlert("Mail accounts",error.what(),"OK",nullptr,nullptr,B_WIDTH_AS_USUAL,B_WARNING_ALERT))->Go(); }
}
}
