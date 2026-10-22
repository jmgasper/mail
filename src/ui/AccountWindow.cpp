#include "AccountWindow.h"
#include "Credentials.h"
#include "mail/Sync.h"
#include "mail/Certificates.h"
#include <Alert.h>
#include <Button.h>
#include <CheckBox.h>
#include <CardLayout.h>
#include <CardView.h>
#include <Directory.h>
#include <Entry.h>
#include <File.h>
#include <FilePanel.h>
#include <FindDirectory.h>
#include <GroupView.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <Path.h>
#include <PopUpMenu.h>
#include <ScrollView.h>
#include <StringView.h>
#include <TabView.h>
#include <TextControl.h>
#include <TextView.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>

namespace mail::ui {
namespace {
enum : uint32 { kSelect='acSl',kNew='acNw',kSave='acSv',kRemove='acRm',kProvider='acPr',kGoogle='acGo',kGoogleDone='acGd',kRevoke='acRv',
    kSecurity='acSc',kChooseCertificate='acCc',kCertificateChosen='acCh',kFetchCertificate='acFc',kCertificateFetched='acFd' };
constexpr char kCertificateHelp[]="Only needed for servers with a self-signed certificate, such as Proton Mail Bridge. "
    "Choose a PEM or DER certificate file, or get it from the server. Leave blank to use the system's trusted authorities.";
// Keep a stable area for asynchronous status changes. BTextView's read-only
// height-for-width layout can retain the previous one-line height in BCardView.
class GoogleStatus : public BTextView {
public:
    GoogleStatus() : BTextView("google status") {}
    bool HasHeightForWidth() override { return false; }
};
void clear(BListView* list) { while (auto* item=list->RemoveItem(int32(0))) delete item; }
BMessage* tagged(uint32 what,bool outgoing) { auto* message=new BMessage(what); message->AddBool("outgoing",outgoing); return message; }
BMenuField* securityMenu(bool outgoing)
{
    auto* menu=new BPopUpMenu("Connection"); menu->AddItem(new BMenuItem("TLS",tagged(kSecurity,outgoing)));
    menu->AddItem(new BMenuItem("STARTTLS",tagged(kSecurity,outgoing))); menu->ItemAt(0)->SetMarked(true);
    auto* field=new BMenuField("security","Security:",menu);
    field->SetToolTip("TLS encrypts from the start (usually ports 993, 995 and 465).\nSTARTTLS upgrades a plain connection (usually ports 143, 110 and 587).");
    return field;
}
// Account types in the provider menu: IMAP, POP3, Proton Mail Bridge, Google.
int defaultPort(int type,bool outgoing,bool tls)
{
    if (outgoing) return type==2?1025:(tls?465:587);
    if (type==2) return 1143;
    return type==1?(tls?995:110):(tls?993:143);
}
bool presetPort(const char* text)
{
    if (!*text) return true;
    for (const char* port : {"993","143","995","110","1143","465","587","25","1025"}) if (std::string(text)==port) return true;
    return false;
}
std::string readCertificate(const std::string& path)
{
    BFile file(path.c_str(),B_READ_ONLY); off_t size=0;
    if (file.InitCheck()!=B_OK || file.GetSize(&size)!=B_OK || size<=0 || size>1024*1024)
        throw std::invalid_argument("Could not read the certificate file “"+path+"”.");
    std::string data(static_cast<std::size_t>(size),'\0');
    if (file.Read(data.data(),size)!=size) throw std::invalid_argument("Could not read the certificate file “"+path+"”.");
    return data;
}
// Trusted certificates live with Mail's settings, readable only by the user.
std::string saveCertificate(const std::string& pem,const std::string& host,int port)
{
    BPath path;
    if (find_directory(B_USER_SETTINGS_DIRECTORY,&path)!=B_OK || path.Append("Mail/certificates")!=B_OK
        || create_directory(path.Path(),0700)!=B_OK) throw std::runtime_error("Could not create the Mail certificates folder.");
    std::string name;
    for (unsigned char c : host) name+=std::isalnum(c) || c=='.' || c=='-'?static_cast<char>(c):'_';
    if (name.empty()) name="server";
    name+="-"+std::to_string(port)+".pem";
    BDirectory directory(path.Path()); BFile file(&directory,(name+".new").c_str(),B_WRITE_ONLY|B_CREATE_FILE|B_ERASE_FILE);
    if (file.InitCheck()!=B_OK || file.SetPermissions(0600)!=B_OK
        || file.Write(pem.data(),pem.size())!=static_cast<ssize_t>(pem.size()) || file.Sync()!=B_OK)
        throw std::runtime_error("Could not save the trusted certificate.");
    BEntry entry(&directory,(name+".new").c_str());
    if (entry.Rename(name.c_str(),true)!=B_OK) throw std::runtime_error("Could not save the trusted certificate.");
    path.Append(name.c_str()); return path.Path();
}
std::string describe(const CertificateDetails& d)
{
    return "Issued to: "+d.subject+"\nIssued by: "+(d.selfSigned?"itself (self-signed)":d.issuer)
        +"\nValid from: "+d.validFrom+"\nValid until: "+d.validUntil+"\n\nSHA-256 fingerprint:\n"+d.sha256;
}
}
AccountWindow::AccountWindow(BWindow* owner,std::shared_ptr<Store> store,GoogleOptions google)
    : BWindow(BRect(0,0,780,580),"Mail accounts",B_TITLED_WINDOW_LOOK,B_MODAL_SUBSET_WINDOW_FEEL,
        B_AUTO_UPDATE_SIZE_LIMITS|B_CLOSE_ON_ESCAPE),fStore(std::move(store)),fOwner(owner),fGoogle(std::move(google))
{
    AddToSubset(owner);
    fList=new BListView("accounts"); fList->SetSelectionMessage(new BMessage(kSelect));
    auto* scroll=new BScrollView("accounts scroll",fList,0,false,true);
    scroll->SetExplicitMinSize(BSize(170,250)); scroll->SetExplicitMaxSize(BSize(220,B_SIZE_UNLIMITED));
    fName=new BTextControl("account name","Name:","",nullptr);
    fEmail=new BTextControl("email","Email:","",nullptr);
    fName->SetToolTip("The name shown in the mailbox list, for example “Work”.");
    fName->TextView()->SetExplicitMinSize(BSize(320,B_SIZE_UNSET));
    auto* providers=new BPopUpMenu("Account type");
    for (const char* label : {"IMAP","POP3","Proton Mail Bridge","Google (Gmail)"}) providers->AddItem(new BMenuItem(label,new BMessage(kProvider)));
    providers->ItemAt(0)->SetMarked(true); fProvider=new BMenuField("provider","Account type:",providers);
    fUnified=new BCheckBox("Include in All inboxes",nullptr);
    fSamePassword=new BCheckBox("Use incoming password for sending",nullptr); fSamePassword->SetValue(B_CONTROL_ON);
    auto* tabs=new BTabView("servers");
    tabs->AddTab(MakeServer("Incoming",fIncoming,false)); tabs->TabAt(0)->SetLabel("Incoming mail");
    tabs->AddTab(MakeServer("Outgoing",fOutgoing,true)); tabs->TabAt(1)->SetLabel("Outgoing mail (SMTP)");
    auto* sentModes=new BPopUpMenu("Sent copies");
    for (const char* label:{"Automatic","Save to IMAP server","Server saves Sent","Keep on this computer"}) sentModes->AddItem(new BMenuItem(label,nullptr));
    sentModes->ItemAt(0)->SetMarked(true); fSentMode=new BMenuField("sent copies","Sent copies:",sentModes);
    fSentMode->SetToolTip("Automatic saves to IMAP, lets Proton Bridge save its own copy, and keeps POP3 Sent mail on this computer.");
    fSentFolder=new BTextControl("sent folder","Sent folder:","",nullptr);
    fSentFolder->SetToolTip("Optional server mailbox name. Leave blank to use the server's advertised Sent folder.");
    auto* servers=new BGroupView("server settings",B_VERTICAL);
    BLayoutBuilder::Group<>(servers).Add(tabs).Add(fSamePassword)
        .AddGrid(B_USE_DEFAULT_SPACING,B_USE_SMALL_SPACING)
            .AddMenuField(fSentMode,0,0).AddTextControl(fSentFolder,0,1).SetColumnWeight(1,1).End();
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
    fAdd=new BButton("Add",new BMessage(kNew)); fRemove=new BButton("Remove…",new BMessage(kRemove));
    BLayoutBuilder::Group<>(this,B_VERTICAL).SetInsets(B_USE_WINDOW_INSETS)
        .AddGroup(B_HORIZONTAL)
            .AddGroup(B_VERTICAL,B_USE_SMALL_SPACING,0).Add(scroll,1).AddGroup(B_HORIZONTAL,B_USE_SMALL_SPACING)
                .Add(fAdd).Add(fRemove).AddGlue().End().End()
            .AddGroup(B_VERTICAL,B_USE_DEFAULT_SPACING,3)
                .AddGrid(B_USE_DEFAULT_SPACING,B_USE_SMALL_SPACING)
                    .AddMenuField(fProvider,0,0).AddTextControl(fName,0,1).AddTextControl(fEmail,0,2).Add(fUnified,1,3)
                    .SetColumnWeight(1,1).End()
                .Add(fProviderCards).End()
        .End()
        .Add(fNote).Add(fHint)
        .AddGroup(B_HORIZONTAL).AddGlue().Add(new BButton("Done",new BMessage(B_QUIT_REQUESTED))).Add(fSave).End();
    SetDefaultButton(fSave); CenterIn(owner->Frame()); Reload();
    if (fAccounts.empty()) Load({}); else fList->Select(0);
}
AccountWindow::~AccountWindow()
{
    fCancelGoogle=true; fCancelCertificate=true;
    if (fGoogleWorker.joinable()) fGoogleWorker.join();
    if (fCertificateWorker.joinable()) fCertificateWorker.join();
    clear(fList); fOwner.SendMessage(kAccountsClosed);
}
bool AccountWindow::QuitRequested()
{
    fCancelCertificate=true;
    if (!fGoogleBusy) return true;
    fCloseAfterGoogle=true; fCancelGoogle=true; fGoogleStatus->SetText("Finishing Google sign-in cancellation…"); return false;
}
BView* AccountWindow::MakeServer(const char* name,ServerFields& f,bool outgoing)
{
    auto* view=new BGroupView(name,B_VERTICAL); f.outgoing=outgoing;
    f.host=new BTextControl("host","Server:","",nullptr);
    f.host->SetToolTip(outgoing?"The SMTP server name, for example smtp.example.com.":"The mail server name, for example imap.example.com.");
    f.port=new BTextControl("port","Port:","",nullptr);
    f.port->TextView()->SetMaxBytes(5);
    for (uint32 c=0;c<256;++c) if (c<'0' || c>'9') f.port->TextView()->DisallowChar(c);
    f.username=new BTextControl("username","Username:","",nullptr);
    f.username->SetToolTip("Usually your full email address. Leave blank to use the email address above.");
    f.password=new BTextControl("password","Password:","",nullptr); f.password->TextView()->HideTyping(true);
    f.certificate=new BTextControl("certificate","Trusted certificate:","",nullptr);
    f.certificate->SetToolTip(kCertificateHelp);
    f.security=securityMenu(outgoing);
    f.choose=new BButton("choose certificate","Choose…",tagged(kChooseCertificate,outgoing));
    f.choose->SetToolTip("Choose a certificate file (PEM or DER).");
    f.fetch=new BButton("fetch certificate","Get from server…",tagged(kFetchCertificate,outgoing));
    f.fetch->SetToolTip("Connect to the server above, show its certificate and fingerprint, and trust it if you confirm.");
    auto* hint=new BStringView("certificate hint","Blank uses system trust. Set for self-signed servers (Proton Bridge).");
    hint->SetHighUIColor(B_PANEL_TEXT_COLOR,B_LIGHTEN_1_TINT); BFont small(be_plain_font); small.SetSize(small.Size()*0.9f); hint->SetFont(&small); hint->SetExplicitMinSize(BSize(small.StringWidth(hint->Text())+4,B_SIZE_UNSET));
    auto* port=f.port->CreateTextViewLayoutItem(); port->SetExplicitMaxSize(BSize(be_plain_font->StringWidth("000000")+12,B_SIZE_UNSET));
    port->SetExplicitMinSize(BSize(be_plain_font->StringWidth("000000")+12,B_SIZE_UNSET));
    BLayoutBuilder::Group<>(view).SetInsets(B_USE_DEFAULT_SPACING)
        .AddGrid(B_USE_DEFAULT_SPACING,B_USE_SMALL_SPACING)
            .AddTextControl(f.host,0,0).Add(f.port->CreateLabelLayoutItem(),2,0).Add(port,3,0)
            .AddMenuField(f.security,0,1,B_ALIGN_HORIZONTAL_UNSET,1,3)
            .AddTextControl(f.username,0,2,B_ALIGN_HORIZONTAL_UNSET,1,3)
            .AddTextControl(f.password,0,3,B_ALIGN_HORIZONTAL_UNSET,1,3)
            .AddTextControl(f.certificate,0,4,B_ALIGN_HORIZONTAL_UNSET,1,3)
            .AddGroup(B_HORIZONTAL,B_USE_SMALL_SPACING,1,5,3).Add(f.choose).Add(f.fetch).AddGlue().End()
            .Add(hint,1,6,3)
            .SetColumnWeight(1,1).End()
        .AddGlue();
    return view;
}
void AccountWindow::SetServer(ServerFields& fields,const Endpoint& endpoint)
{
    fields.host->SetText(endpoint.host.c_str()); fields.port->SetText(endpoint.port?std::to_string(endpoint.port).c_str():"");
    fields.username->SetText(endpoint.username.c_str()); fields.password->SetText("");
    fields.certificate->SetText(endpoint.certificate.c_str()); fields.security->Menu()->ItemAt(endpoint.security==Security::TLS?0:1)->SetMarked(true);
    UpdateCertificateTip(fields);
}
int AccountWindow::ProviderType() const { return fProvider->Menu()->IndexOf(fProvider->Menu()->FindMarked()); }
// Changing the account type or security fills in the usual port, but never
// replaces a port the user typed.
void AccountWindow::SetDefaultPort(ServerFields& f)
{
    if (!presetPort(f.port->Text())) return;
    bool tls=f.security->Menu()->IndexOf(f.security->Menu()->FindMarked())==0;
    f.port->SetText(std::to_string(defaultPort(ProviderType(),f.outgoing,tls)).c_str());
}
void AccountWindow::UpdateCertificateTip(ServerFields& f)
{
    std::string value=f.certificate->Text(),tip=kCertificateHelp;
    if (!value.empty()) {
        try { tip=describe(describeCertificate(isPemText(value)?value:readCertificate(value))); }
        catch (const std::exception& error) { tip=std::string("This certificate cannot be used: ")+error.what(); }
    }
    f.certificate->SetToolTip(tip.c_str());
}
// Pasted certificate text is saved as a file so the setting stays a path.
std::string AccountWindow::TrustedCertificate(ServerFields& f,const std::string& host,int port)
{
    std::string value=f.certificate->Text();
    if (value.empty()) return value;
    if (isPemText(value)) {
        auto pem=normalizePem(value); describeCertificate(pem);
        value=saveCertificate(pem,host,port); f.certificate->SetText(value.c_str()); UpdateCertificateTip(f); return value;
    }
    if (!BEntry(value.c_str()).Exists()) throw std::invalid_argument("The trusted certificate “"+value+"” does not exist. Choose the file again, or clear the field.");
    describeCertificate(readCertificate(value));
    return value;
}
void AccountWindow::ChooseCertificate(bool outgoing)
{
    auto& f=outgoing?fOutgoing:fIncoming;
    if (!fCertificatePanel) {
        BMessenger target(this);
        fCertificatePanel=std::make_unique<BFilePanel>(B_OPEN_PANEL,&target,nullptr,B_FILE_NODE,false);
        fCertificatePanel->SetButtonLabel(B_DEFAULT_BUTTON,"Trust");
        if (fCertificatePanel->Window()->Lock()) {
            fCertificatePanel->Window()->SetTitle("Mail: Choose a trusted certificate"); fCertificatePanel->Window()->Unlock();
        }
    }
    BMessage chosen(kCertificateChosen); chosen.AddBool("outgoing",outgoing); fCertificatePanel->SetMessage(&chosen);
    std::string current=f.certificate->Text(); BPath parent;
    if (!current.empty() && !isPemText(current) && BPath(current.c_str()).GetParent(&parent)==B_OK && BEntry(parent.Path()).Exists())
        fCertificatePanel->SetPanelDirectory(parent.Path());
    fCertificatePanel->Show();
}
void AccountWindow::CertificateChosen(BMessage* message)
{
    entry_ref ref; if (message->FindRef("refs",&ref)!=B_OK) return;
    auto& f=message->GetBool("outgoing",false)?fOutgoing:fIncoming;
    BPath path(&ref); auto bytes=readCertificate(path.Path()); auto details=describeCertificate(bytes);
    std::string value=path.Path();
    // libcurl reads PEM files; keep a converted copy of a DER certificate.
    if (!isPemText(bytes)) value=saveCertificate(details.pem,*f.host->Text()?f.host->Text():ref.name,std::atoi(f.port->Text()));
    f.certificate->SetText(value.c_str()); UpdateCertificateTip(f);
    fHint->SetText(("Trusting “"+details.subject+"”. Save the account to keep it.").c_str());
}
void AccountWindow::FetchCertificate(bool outgoing)
{
    if (fFetching) return;
    if (fCertificateWorker.joinable()) fCertificateWorker.join();
    auto& f=outgoing?fOutgoing:fIncoming;
    Endpoint endpoint=ReadEndpoint(f);
    if (endpoint.host.empty()) { f.host->MakeFocus(); throw std::invalid_argument("Enter the server name first."); }
    std::string scheme=outgoing?"smtp":(ProviderType()==1?"pop3":"imap");
    fFetching=true; fIncoming.fetch->SetEnabled(false); fOutgoing.fetch->SetEnabled(false);
    fHint->SetText(("Getting the certificate from "+endpoint.host+":"+std::to_string(endpoint.port)+"…").c_str());
    fCancelCertificate=false; BMessenger target(this);
    fCertificateWorker=std::thread([this,target,endpoint,scheme,outgoing] {
        BMessage done(kCertificateFetched); done.AddBool("outgoing",outgoing);
        done.AddString("host",endpoint.host.c_str()); done.AddInt32("port",endpoint.port);
        try {
            auto d=fetchServerCertificate(endpoint,scheme,fCancelCertificate);
            done.AddString("pem",d.pem.c_str()); done.AddString("details",describe(d).c_str());
        } catch (const std::exception& error) { done.AddString("error",error.what()); }
        target.SendMessage(&done);
    });
}
void AccountWindow::FinishCertificate(BMessage* message)
{
    if (fCertificateWorker.joinable()) fCertificateWorker.join();
    fFetching=false; fIncoming.fetch->SetEnabled(true); fOutgoing.fetch->SetEnabled(true); fHint->SetText("");
    if (fCancelCertificate) return;
    bool outgoing=message->GetBool("outgoing",false); auto& f=outgoing?fOutgoing:fIncoming; auto& other=outgoing?fIncoming:fOutgoing;
    std::string host=message->GetString("host",""); int port=message->GetInt32("port",0);
    std::string server=host+":"+std::to_string(port);
    if (const char* error=message->GetString("error",nullptr)) {
        (new BAlert("Mail accounts",("Could not get the certificate from "+server+".\n\n"+error).c_str(),"OK",nullptr,nullptr,B_WIDTH_AS_USUAL,B_WARNING_ALERT))->Go();
        return;
    }
    std::string prompt="The server "+server+" presented this certificate:\n\n"+message->GetString("details","")
        +"\n\nTrust it only if you expect this server to use it. For Proton Mail Bridge, compare the fingerprint with the certificate Bridge exports.";
    auto* alert=new BAlert("Trust certificate",prompt.c_str(),"Cancel","Trust certificate",nullptr,B_WIDTH_FROM_LABEL,B_WARNING_ALERT);
    alert->SetShortcut(0,B_ESCAPE);
    if (alert->Go()!=1) return;
    auto path=saveCertificate(message->GetString("pem",""),host,port);
    f.certificate->SetText(path.c_str()); UpdateCertificateTip(f);
    std::string status="Certificate saved. Save the account to use it.";
    // Proton Mail Bridge and most servers use one certificate for both directions.
    if (host==other.host->Text() && !*other.certificate->Text()) {
        other.certificate->SetText(path.c_str()); UpdateCertificateTip(other);
        status="Certificate saved for incoming and outgoing mail. Save the account to use it.";
    }
    fHint->SetText(status.c_str());
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
    fProvider->Menu()->ItemAt(type)->SetMarked(true); fProviderType=type; fUnified->SetValue(fEditing.unified);
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
    a.incoming.certificate=TrustedCertificate(fIncoming,a.incoming.host,a.incoming.port);
    a.outgoing.certificate=TrustedCertificate(fOutgoing,a.outgoing.host,a.outgoing.port);
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
                int type=ProviderType();
                if (type==2) {
                    fIncoming.host->SetText("127.0.0.1"); fOutgoing.host->SetText("127.0.0.1");
                    fIncoming.security->Menu()->ItemAt(1)->SetMarked(true); fOutgoing.security->Menu()->ItemAt(1)->SetMarked(true);
                } else if (fProviderType==2) {
                    for (auto* f : {&fIncoming,&fOutgoing}) if (std::string(f->host->Text())=="127.0.0.1") f->host->SetText("");
                    fIncoming.security->Menu()->ItemAt(0)->SetMarked(true); fOutgoing.security->Menu()->ItemAt(1)->SetMarked(true);
                }
                if (type!=3) { SetDefaultPort(fIncoming); SetDefaultPort(fOutgoing); }
                fProviderType=type; UpdateProvider();
                break;
            }
            case kSecurity: SetDefaultPort(message->GetBool("outgoing",false)?fOutgoing:fIncoming); break;
            case kChooseCertificate: ChooseCertificate(message->GetBool("outgoing",false)); break;
            case kCertificateChosen: CertificateChosen(message); break;
            case kFetchCertificate: FetchCertificate(message->GetBool("outgoing",false)); break;
            case kCertificateFetched: FinishCertificate(message); break;
            default: BWindow::MessageReceived(message);
        }
    } catch (const std::exception& error) { (new BAlert("Mail accounts",error.what(),"OK",nullptr,nullptr,B_WIDTH_AS_USUAL,B_WARNING_ALERT))->Go(); }
}
}
