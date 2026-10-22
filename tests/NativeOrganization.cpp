// Drives native organization forms and observes a separate HTTPS Gmail fixture.
#include "MailWindow.h"
#include "OrganizationWindow.h"
#include "Credentials.h"
#include "mail/Mime.h"
#include <Alert.h>
#include <Application.h>
#include <Autolock.h>
#include <Bitmap.h>
#include <BitmapStream.h>
#include <Button.h>
#include <CheckBox.h>
#include <File.h>
#include <ListView.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <MessageRunner.h>
#include <Screen.h>
#include <TextControl.h>
#include <TextView.h>
#include <TranslatorRoster.h>
#include <nlohmann/json.hpp>
#include <cstdio>
#include <ctime>
#include <unistd.h>
namespace {
using Json=nlohmann::json;
void check(bool value,const char* text) { if(!value) throw std::runtime_error(text); }
template<class T> T* view(BWindow* window,const char* name) { auto* result=dynamic_cast<T*>(window->FindView(name)); check(result,name); return result; }
void capture(const char* path)
{
    BBitmap* bitmap=nullptr; check(BScreen().GetBitmap(&bitmap)==B_OK,"capture organization window");
    BBitmapStream stream(bitmap); BFile file(path,B_WRITE_ONLY|B_CREATE_FILE|B_ERASE_FILE);
    check(BTranslatorRoster::Default()->Translate(&stream,nullptr,nullptr,&file,B_PNG_FORMAT)==B_OK,"write organization capture");
}
class Test : public BApplication {
public:
    Test(std::string directory,char** args):BApplication("application/x-vnd.air-Mail-Organization-Test"),dir(std::move(directory))
    {
        base="https://"+std::string(args[1])+":"+args[3]; options.endpoints.gmail=base+"/gmail"; options.endpoints.certificate=args[2];
    }
    int result=1;
    void ReadyToRun() override
    {
        try {
            store=std::make_shared<mail::Store>(dir+"/mail.db");
            local.name="On this computer"; local.email="local@example.test"; local.id=store->saveAccount(local);
            other=local; other.id=0; other.name="Other local"; other.id=store->saveAccount(other);
            google.name="Google fixture"; google.email="user@example.test"; google.provider=mail::Provider::Gmail;
            auto id=store->saveAccount(google); for(const auto& a:store->accounts()) if(a.id==id) google=a;
            for(const auto& account:{local,other}) {
                auto m=mail::parseMessage("From: sender@example.test\r\nSubject: Local message\r\nMessage-ID: <local@example.test>\r\n\r\nLocal body\r\n");
                m.account=account.id; m.remoteKey="local"; m.received=std::time(nullptr); m.tags={"Local project"};
                auto message=store->ingest(m); if(account.id==local.id) localMessage=message; else otherMessage=message;
            }
            mail::SmartFolder smart; smart.name="Tracked view"; smart.query.account=local.id; smart.query.folder=""; smart.query.tag="Local project"; smartId=store->saveSmartFolder(smart);
            mail::GoogleTokens tokens{{"organization.apps.googleusercontent.com","fixture-secret"},"fixture-access","fixture-refresh",std::time(nullptr)+3600,mail::kGoogleMailScope};
            std::puts("Approve this disposable organization test app's key-store prompts if shown."); std::fflush(stdout);
            mail::ui::SaveGoogleTokens(google,tokens);
            main=new mail::ui::MailWindow(store,options); main->Show();
            BMessage tick('test'); timer=std::make_unique<BMessageRunner>(BMessenger(this),&tick,200000);
        } catch(const std::exception& e) { fail(e.what()); }
    }
    void MessageReceived(BMessage* message) override
    {
        if(message->what!='test') { BApplication::MessageReceived(message); return; }
        try { check(++ticks<800,"native organization timeout"); step(); } catch(const std::exception& e) { fail(e.what()); }
    }
private:
    template<class T> T* window(const char* title=nullptr)
    { for(int32 i=0;i<CountWindows();++i) if(auto* w=dynamic_cast<T*>(WindowAt(i))) if(!title || std::string(w->Title())==title) return w; return nullptr; }
    void fail(const char* text) { std::fprintf(stderr,"FAIL organization stage %d: %s\n",stage,text); timer.reset(); PostMessage(B_QUIT_REQUESTED); }
    void next() { ++stage; std::printf("Organization stage %d\n",stage); std::fflush(stdout); }
    Json fixture(const std::string& path,const Json& data=Json())
    {
        mail::HttpRequest r; r.url=base+"/fixture/"+path; r.certificate=options.endpoints.certificate;
        if(!data.is_null()) { r.method="POST"; r.contentType="application/json"; r.body=data.dump(); }
        auto response=mail::requestHttps(r,cancel); mail::requireHttpSuccess(response); return Json::parse(response.body);
    }
    bool idle() { BAutolock lock(main); return std::string(view<BButton>(main,"check mail")->Label())=="Check mail"; }
    void press(BWindow* w,const char* control) { check(w,"editor exists"); BAutolock lock(w); auto* b=view<BButton>(w,control); check(b->IsEnabled(),control); b->Invoke(); }
    void text(BWindow* w,const char* field,const char* value) { BAutolock lock(w); view<BTextControl>(w,field)->SetText(value); }
    void select(BWindow* w,const std::string& name)
    {
        BAutolock lock(w); auto* list=view<BListView>(w,"organization list");
        for(int32 i=0;i<list->CountItems();++i) if(std::string(static_cast<BStringItem*>(list->ItemAt(i))->Text()).find(name)!=std::string::npos) { list->Select(i); return; }
        throw std::runtime_error("organization row missing: "+name);
    }
    void scope(BWindow* w,mail::Id account)
    {
        BAutolock lock(w); auto* menu=view<BMenuField>(w,"organization account")->Menu();
        for(int32 i=0;i<menu->CountItems();++i) { int64 id=-1; auto* item=menu->ItemAt(i); item->Message()->FindInt64("account",&id); if(id==account) { item->SetMarked(true); check(static_cast<BInvoker*>(item)->Invoke()==B_OK,"native menu invocation"); return; } }
        throw std::runtime_error("account scope missing");
    }
    void folder(BWindow* w,const std::string& target)
    {
        BAutolock lock(w); auto* menu=view<BMenuField>(w,"organization folder")->Menu();
        for(int32 i=0;i<menu->CountItems();++i) { auto* item=menu->ItemAt(i); if(target==item->Message()->FindString("folder")) { item->SetMarked(true); check(static_cast<BInvoker*>(item)->Invoke()==B_OK,"native menu invocation"); return; } }
        throw std::runtime_error("folder choice missing");
    }
    bool confirm(const char* title,bool accepted)
    {
        auto* alert=window<BAlert>(); if(!alert) return false; BAutolock lock(alert);
        check(std::string(alert->Title())==title,"correct confirmation"); check(alert->DefaultButton()==alert->ButtonAt(0),"Cancel is default");
        alert->ButtonAt(accepted?1:0)->Invoke(); return true;
    }
    void sidebar(const std::string& name)
    {
        BAutolock lock(main); auto* list=view<BListView>(main,"mailboxes");
        for(int32 i=0;i<list->CountItems();++i) if(name==static_cast<BStringItem*>(list->ItemAt(i))->Text()) { list->Select(i); return; }
        throw std::runtime_error("sidebar view missing");
    }
    void step()
    {
        auto* rules=window<mail::ui::OrganizationWindow>("Mail rules");
        auto* smart=window<mail::ui::OrganizationWindow>("Smart folders");
        auto* tags=window<mail::ui::OrganizationWindow>("Tags");
        switch(stage) {
            case 0: if(!idle() || !store->findRemote(google.id,"gmail:a")) return; main->PostMessage('mflt'); next(); break;
            case 1:
                if(!rules) return;
                scope(rules,google.id); text(rules,"organization name","News 🎉"); text(rules,"rule sender","news@"); text(rules,"organization tag","Priority"); folder(rules,"archive");
                { BAutolock lock(rules); for(const char* name:{"organization read","organization star","rule stop"}) view<BCheckBox>(rules,name)->SetValue(B_CONTROL_ON); }
                next(); break;
            case 2: press(rules,"organization save"); next(); break;
            case 3:
                if(store->filters().size()!=1) return;
                check(store->filters()[0].account==google.id && store->filters()[0].star,"native rule fields persist");
                capture("/boot/home/mail-check/organization-rules.png"); press(rules,"organization new"); next(); break;
            case 4:
                scope(rules,google.id); text(rules,"organization name","Fallback"); text(rules,"rule sender","news@");
                { BAutolock lock(rules); view<BCheckBox>(rules,"organization star")->SetValue(B_CONTROL_ON); } next(); break;
            case 5: press(rules,"organization save"); next(); break;
            case 6: if(store->filters().size()!=2) return; select(rules,"Fallback"); next(); break;
            case 7: press(rules,"rule up"); next(); break;
            case 8:
                if(store->filters()[0].name!="Fallback") return;
                text(rules,"organization name","Fallback disabled"); { BAutolock lock(rules); view<BCheckBox>(rules,"rule enabled")->SetValue(B_CONTROL_OFF); } next(); break;
            case 9: press(rules,"organization save"); next(); break;
            case 10:
                if(ruleRemovePhase==0) { if(store->filters()[0].enabled) return; press(rules,"organization remove"); ruleRemovePhase=1; return; }
                if(ruleRemovePhase==1) { if(confirm("Remove",false)) ruleRemovePhase=2; return; }
                if(ruleRemovePhase==2) { if(window<BAlert>()) return; check(store->filters().size()==2,"Cancel retains rule"); press(rules,"organization remove"); ruleRemovePhase=3; return; }
                if(ruleRemovePhase==3) { if(confirm("Remove",true)) ruleRemovePhase=4; return; }
                if(store->filters().size()!=1) return;
                check(store->filters()[0].name=="News 🎉","native removal keeps other rules");
                fixture("config",{{"inject",true}}); main->PostMessage('mcsc'); next(); break;
            case 11: {
                auto id=store->findRemote(google.id,"gmail:news"); if(!idle() || !id || !store->pendingChanges(google.id).empty()) return;
                auto m=store->message(id); check(!m.unread && m.starred && m.folder=="archive" && m.tags==std::vector<std::string>{"Priority"},"native-created rule changes received Gmail message");
                auto memberships=fixture("state")["memberships"]["news"];
                check(memberships==Json::array({"Label_new1","STARRED"}),"server observes native rule's read/star/tag/archive actions");
                rules->PostMessage(B_QUIT_REQUESTED); sidebar("◆ Tracked view"); next(); break;
            }
            case 12: if(rules) return; main->PostMessage('mmsf'); next(); break;
            case 13:
                if(!smart) return;
                text(smart,"organization name","Tracked renamed"); text(smart,"smart search","unmatched"); next(); break;
            case 14: press(smart,"organization save"); next(); break;
            case 15: {
                if(store->smartFolders()[0].name!="Tracked renamed") return;
                BAutolock lock(main); auto* list=view<BListView>(main,"mailboxes"); auto* item=dynamic_cast<BStringItem*>(list->ItemAt(list->CurrentSelection()));
                check(item && std::string(item->Text())=="◆ Tracked renamed","renamed selected smart folder retains sidebar identity");
                check(view<BListView>(main,"conversations")->CountItems()==0,"edited criteria immediately update selected saved view");
                capture("/boot/home/mail-check/organization-smart.png");
                text(smart,"organization name","Unsaved name"); smart->PostMessage(B_QUIT_REQUESTED); next(); break;
            }
            case 16: if(confirm("Unsaved changes",false)) next(); break;
            case 17:
                if(window<BAlert>()) return;
                check(smart,"cancel keeps editor open"); press(smart,"organization refresh"); next(); break;
            case 18: if(confirm("Unsaved changes",true)) next(); break;
            case 19: {
                if(window<BAlert>()) return;
                BAutolock lock(smart); check(std::string(view<BTextControl>(smart,"organization name")->Text())=="Tracked renamed","discard refresh restores saved fields");
                press(smart,"organization remove"); next(); break;
            }
            case 20: if(confirm("Remove",false)) next(); break;
            case 21: if(window<BAlert>()) return; check(store->smartFolders().size()==1,"Cancel retains smart folder"); press(smart,"organization remove"); next(); break;
            case 22: if(confirm("Remove",true)) next(); break;
            case 23:
                if(smartCreatePhase==0) {
                    if(!store->smartFolders().empty()) return;
                    check(!store->message(localMessage).raw.empty(),"removing smart folder preserves messages"); press(smart,"organization new"); smartCreatePhase=1; return;
                }
                if(smartCreatePhase==1) { text(smart,"organization name","Local tag view"); text(smart,"smart search",""); smartCreatePhase=2; return; }
                if(smartCreatePhase==2) { press(smart,"organization save"); smartCreatePhase=3; return; }
                if(store->smartFolders().empty()) return;
                check(store->smartFolders()[0].id!=smartId && store->smartFolders()[0].query.account==local.id,"native new smart folder saves seed scope and uses a fresh ID");
                smart->PostMessage(B_QUIT_REQUESTED); next(); break;
            case 24: if(smart) return; main->PostMessage('mmtg'); next(); break;
            case 25: if(!tags) return; scope(tags,local.id); next(); break;
            case 26: select(tags,"Local project"); next(); break;
            case 27: text(tags,"organization name","Local renamed 🎉"); next(); break;
            case 28: press(tags,"organization save"); next(); break;
            case 29:
                if(store->message(localMessage).tags!=std::vector<std::string>{"Local renamed 🎉"}) return;
                check(store->message(otherMessage).tags==std::vector<std::string>{"Local project"},"local tag rename stays within selected account");
                check(store->smartFolders()[0].query.tag=="Local renamed 🎉","native tag rename updates scoped smart criteria"); scope(tags,google.id); next(); break;
            case 30: select(tags,"Project"); next(); break;
            case 31: text(tags,"organization name","Project renamed"); next(); break;
            case 32: press(tags,"organization save"); next(); break;
            case 33:
                if(store->labelChanges(google.id).empty()) return;
                check(fixture("state")["labels"]["Label_1"]["name"]=="Project","rename is durable before network check"); press(tags,"organization check"); next(); break;
            case 34:
                if(!idle() || !store->labelChanges(google.id).empty()) return;
                check(fixture("state")["labels"]["Label_1"]["name"]=="Project renamed","native rename reaches Gmail API"); select(tags,"Project renamed"); next(); break;
            case 35: press(tags,"organization remove"); next(); break;
            case 36: if(confirm("Remove",false)) next(); break;
            case 37: if(window<BAlert>()) return; check(fixture("state")["labels"].contains("Label_1"),"cancel keeps server label"); press(tags,"organization remove"); next(); break;
            case 38: if(confirm("Remove",true)) next(); break;
            case 39: if(store->labelChanges(google.id).empty()) return; press(tags,"organization check"); next(); break;
            case 40:
                if(!idle() || !store->labelChanges(google.id).empty()) return;
                check(!fixture("state")["labels"].contains("Label_1"),"native label removal reaches Gmail"); select(tags,"Empty"); next(); break;
            case 41: press(tags,"organization remove"); next(); break;
            case 42: if(confirm("Remove",true)) next(); break;
            case 43: if(store->labelChanges(google.id).empty()) return; press(tags,"organization check"); next(); break;
            case 44:
                if(!idle() || !store->labelChanges(google.id).empty()) return;
                check(!fixture("state")["labels"].contains("Label_2") && !store->message(store->findRemote(google.id,"gmail:a")).raw.empty(),"removing unused label keeps original mail");
                capture("/boot/home/mail-check/organization-tags.png"); tags->PostMessage(B_QUIT_REQUESTED); next(); break;
            case 45:
                if(tags) return;
                sidebar("◆ Local tag view"); next(); break;
            case 46: {
                BAutolock lock(main); auto* list=view<BListView>(main,"conversations");
                check(list->CountItems()==1,"renamed local smart view retains its message"); list->Select(0);
                main->PostMessage('mrtg'); next(); break;
            }
            case 47: {
                auto* edit=window<BWindow>("Remove tag"); if(!edit) return;
                text(edit,"name","Local renamed 🎉"); edit->PostMessage('medt'); next(); break;
            }
            case 48: {
                if(!store->message(localMessage).tags.empty()) return;
                check(store->message(otherMessage).tags==std::vector<std::string>{"Local project"} && store->labelChanges(google.id).empty(),"message tag removal preserves other accounts and label definitions");
                mail::Draft draft; draft.account=local.id; draft.subject="Plain draft"; draft.id=store->saveDraft(draft);
                draft.id=0; draft.subject="Draft with file"; mail::Attachment attachment; attachment.name="note.txt"; attachment.mediaType="text/plain"; attachment.data={'x'}; draft.attachments={attachment}; store->saveDraft(draft);
                mail::SmartFolder filter; filter.name="Draft criteria"; filter.query.account=local.id; filter.query.folder="drafts"; filter.query.unread=true;
                draftSmart=store->saveSmartFolder(filter); main->PostMessage(mail::ui::kOrganizationChanged); next(); break;
            }
            case 49: sidebar("◆ Draft criteria"); next(); break;
            case 50: {
                BAutolock lock(main); check(view<BListView>(main,"conversations")->CountItems()==0,"unread smart condition excludes local drafts");
                mail::SmartFolder filter; filter.id=draftSmart; filter.name="Draft criteria"; filter.query.account=local.id; filter.query.folder="drafts"; filter.query.attachments=true;
                store->saveSmartFolder(filter); main->PostMessage(mail::ui::kOrganizationChanged); next(); break;
            }
            case 51: {
                BAutolock lock(main); check(view<BListView>(main,"conversations")->CountItems()==1,"attachment smart condition selects only the attached local draft");
                mail::ui::RemoveCredentials(google); timer.reset(); result=0;
                std::puts("Native organization passed: rule creation/order/edit/disable, real Gmail filtering, smart-view identity and draft criteria, unsaved Cancel/Discard, removal Cancel/Confirm, scoped local rename, message tag removal, Gmail label rename/delete and retained messages"); std::fflush(stdout); PostMessage(B_QUIT_REQUESTED); next(); break;
            }
        }
    }
    std::string dir,base; std::shared_ptr<mail::Store> store; mail::Account local,other,google;
    mail::ui::GoogleOptions options; mail::ui::MailWindow* main=nullptr; std::unique_ptr<BMessageRunner> timer;
    std::atomic<bool> cancel{false}; int stage=0,ticks=0,ruleRemovePhase=0,smartCreatePhase=0; mail::Id localMessage=0,otherMessage=0,smartId=0,draftSmart=0;
};
}
int main(int argc,char** argv)
{
    if(argc!=4) return 2;
    char dir[]="/tmp/mail-native-organization-XXXXXX"; if(!mkdtemp(dir)) return 2;
    int result; { Test app(dir,argv); app.Run(); result=app.result; }
    for(const char* name:{"mail.db-wal","mail.db-shm","mail.db"}) unlink((std::string(dir)+"/"+name).c_str());
    rmdir(dir); return result;
}
