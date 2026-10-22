#include "mail/Store.h"
#include "mail/Mime.h"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <unistd.h>
using namespace mail;
namespace {
int checks=0;
void expect(bool value,const char* text) { ++checks; if(!value) throw std::runtime_error(text); }
template<class F> void rejects(F fn,const char* text) { bool failed=false; try { fn(); } catch(const std::exception&) { failed=true; } expect(failed,text); }
Account addAccount(Store& store,const std::string& name,Provider provider=Provider::Local)
{ Account a; a.name=name; a.email=name+"@example.test"; a.provider=provider; a.id=store.saveAccount(a); return a; }
Message message(Id account,const std::string& id)
{ auto m=parseMessage("From: news@example.test\r\nTo: user@example.test\r\nSubject: Release news\r\nMessage-ID: <"+id+"@example.test>\r\n\r\nBody\r\n"); m.account=account; m.remoteKey=id; m.received=1792542600; return m; }
GmailMessage gmail(const std::string& id)
{ return {id,"thread-"+id,"100",{"INBOX","UNREAD","Label_1"},1792542600,message(1,id).raw}; }
bool tagged(Store& store,Id id,const std::string& tag) { auto m=store.message(id); return std::find(m.tags.begin(),m.tags.end(),tag)!=m.tags.end(); }
void run(const std::string& path)
{
    Store store(path),other(path); auto a=addAccount(store,"Local"),b=addAccount(store,"Other"),g=addAccount(store,"Google",Provider::Gmail);
    Filter first; first.name="First"; first.account=a.id; first.senderContains="NEWS@"; first.addTag="First"; first.stop=true; first.id=store.saveFilter(first);
    Filter second=first; second.id=0; second.name="Second"; second.addTag="Second"; second.stop=false; second.id=store.saveFilter(second);
    auto one=store.ingest(message(a.id,"one")); expect(tagged(store,one,"First") && !tagged(store,one,"Second"),"stop-after-match respects initial rule order");
    store.moveFilter(second.id,-1); expect(store.filters()[0].id==second.id,"reorder preserves rule identities");
    auto two=store.ingest(message(a.id,"two")); expect(tagged(store,two,"First") && tagged(store,two,"Second"),"reordered rules change subsequent delivery behavior");
    second.enabled=false; second.name="Disabled"; store.saveFilter(second);
    expect(store.filters()[0].id==second.id && !store.filters()[0].enabled,"editing keeps order and enabled state");
    auto three=store.ingest(message(a.id,"three")); expect(!tagged(store,three,"Second"),"disabled rule does not run");
    auto unrelated=store.ingest(message(b.id,"other")); expect(store.message(unrelated).tags.empty(),"account-scoped rules leave other accounts unchanged");
    auto remote=message(a.id,"remote-sent"); remote.folder="trash";
    auto trash=store.ingestRemote(remote,{"Trash","1","1"}); expect(store.message(trash).tags.empty() && store.message(trash).folder=="trash","receive does not filter historic Trash mail");
    store.removeFilter(first.id); auto old=first; first.id=0; first.id=store.saveFilter(first);
    expect(first.id!=old.id,"deleted rule identifiers are never reused"); rejects([&]{other.saveFilter(old);},"stale editor cannot overwrite replacement rule");
    rejects([&]{store.moveFilter(old.id,1);},"reorder rejects removed rule");
    rejects([&]{auto invalid=first; invalid.account=999; store.saveFilter(invalid);},"rule cannot reference removed account");
    rejects([&]{auto invalid=first; invalid.name=" \n"; store.saveFilter(invalid);},"rule name rejects hidden controls");

    SmartFolder smart; smart.name="Project view"; smart.query.account=a.id; smart.query.folder=""; smart.query.tag="First"; smart.query.includeSnoozed=true;
    smart.id=store.saveSmartFolder(smart); auto global=smart; global.id=0; global.name="All Project"; global.query.account=0; global.id=store.saveSmartFolder(global);
    store.renameTag(a.id,"First","Renamed 🎉"); expect(tagged(store,one,"Renamed 🎉") && !tagged(store,one,"First"),"local rename updates all assigned messages");
    expect(store.smartFolders()[1].query.tag=="Renamed 🎉" && store.smartFolders()[0].query.tag=="First","rename updates scoped views and preserves all-account criteria");
    auto rules=store.filters(); expect(rules.back().addTag=="Renamed 🎉","rename updates this account's rule action");
    expect(store.accountTags(a.id).size()==2,"account tags aggregate assigned names");
    rejects([&]{store.renameTag(a.id,"Renamed 🎉","Second");},"rename cannot silently merge existing tags");
    store.removeTag(a.id,"Renamed 🎉"); expect(!tagged(store,one,"Renamed 🎉") && !store.message(one).raw.empty(),"removing tag keeps every message");
    expect(store.smartFolders()[1].query.tag=="Renamed 🎉","deleting a tag never broadens a saved view");
    expect(store.filters().back().addTag.empty(),"removed tag is not re-added by an account rule");
    store.removeSmartFolder(smart.id); auto stale=smart; smart.id=0; smart.id=store.saveSmartFolder(smart);
    expect(smart.id!=stale.id,"deleted smart-folder IDs are never reused"); rejects([&]{other.saveSmartFolder(stale);},"stale smart editor cannot recreate deleted view");
    expect(store.conversations(smart.query,2000000000).empty(),"saved tag criteria with a removed tag returns no mail");

    std::vector<GmailLabel> labels={{"INBOX","Inbox",true},{"UNREAD","Unread",true},{"Label_1","Project",false},{"Label_2","Empty",false}};
    store.saveGmailLabels(g.id,labels); auto gm=gmail("g1"); auto id=store.ingestGmail(g.id,gm);
    expect(store.accountTags(g.id).size()==2,"Gmail tag manager includes unused user labels and excludes system labels");
    store.tag(id,"Project",false); auto oldMembership=store.pendingChanges(g.id)[0].id;
    store.renameTag(g.id,"Project","Work"); auto original=store.labelChanges(g.id)[0];
    expect(original.kind=="rename" && original.label=="Label_1" && original.name=="Work","rename journal uses immutable Gmail label identity");
    expect(store.pendingChanges(g.id)[0].kind=="tag:Work" && store.pendingChanges(g.id)[0].id!=oldMembership,"rename gives membership edits fresh operation IDs");
    store.finishLabelChange(original.id,"Offline"); store.saveGmailLabels(g.id,labels);
    expect(store.accountTags(g.id).back().name=="Work" && store.accountTags(g.id).back().error=="Offline","stale label catalogs preserve pending name and failure details");
    store.renameTag(g.id,"Work","Current"); auto newer=store.labelChanges(g.id)[0];
    store.finishLabelChange(original.id); expect(store.labelChanges(g.id)[0].id==newer.id,"old rename acknowledgement cannot erase a newer rename");
    store.ingestGmail(g.id,gm,oldMembership); expect(!tagged(store,id,"Current") && store.pendingChanges(g.id).size()==1,"older message acknowledgement cannot erase renamed pending removal");
    store.tag(id,"Current",true); store.ingestGmail(g.id,gm); expect(tagged(store,id,"Current"),"new membership uses pending renamed label");
    store.removeTag(g.id,"Current"); auto removal=store.labelChanges(g.id)[0];
    expect(removal.kind=="delete" && removal.id!=newer.id && store.pendingChanges(g.id).empty(),"delete replaces rename and removes obsolete membership work");
    store.finishLabelChange(newer.id); store.saveGmailLabels(g.id,labels); store.ingestGmail(g.id,gm);
    expect(!tagged(store,id,"Project") && !tagged(store,id,"Current") && !store.message(id).raw.empty(),"pending deletion suppresses stale remote membership without deleting messages");
    expect(store.accountTags(g.id)[0].pending=="delete","pending deletion remains visible to review");
    store.finishLabelChange(removal.id,"Temporary API failure");
    { Store reopened(path); expect(reopened.labelChanges(g.id)[0].error=="Temporary API failure","rename/delete work and failures survive process restart"); }
    store.finishLabelChange(removal.id); labels.erase(labels.begin()+2); store.saveGmailLabels(g.id,labels);
    expect(store.accountTags(g.id).size()==1 && store.labelChanges(g.id).empty(),"confirmed deletion removes only the label");
    rejects([&]{store.renameTag(g.id,"Inbox","Custom");},"system labels cannot be renamed through the tag manager");
    store.tag(id,"Offline",true); store.renameTag(g.id,"Offline","Fresh");
    expect(store.labelChanges(g.id).empty() && store.pendingChanges(g.id)[0].kind=="tag:Fresh","unsynced new tag renames its membership without creating a phantom label update");
    store.removeAccount(g.id); expect(store.labelChanges(g.id).empty(),"removing account cascades label work");
    expect(other.smartFolders().size()==2 && other.filters().size()==2,"saved organization remains visible across connections");
}
}
int main()
{
    char dir[]="/tmp/mail-organization-XXXXXX"; if(!mkdtemp(dir)) return 2;
    try { run(std::string(dir)+"/mail.db"); std::filesystem::remove_all(dir); std::cout<<checks<<" organization assertions passed\n"; return 0; }
    catch(const std::exception& e) { std::cerr<<"FAIL: "<<e.what()<<'\n'; std::filesystem::remove_all(dir); return 1; }
}
