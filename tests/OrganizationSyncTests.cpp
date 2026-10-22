#include "mail/Sync.h"
#include <nlohmann/json.hpp>
#include <iostream>
using namespace mail;
using Json=nlohmann::json;
namespace {
int checks=0;
void expect(bool value,const char* text) { ++checks; if(!value) throw std::runtime_error(text); }
template<class F> void rejects(F fn,const char* text) { bool failed=false; try { fn(); } catch(const std::exception&) { failed=true; } expect(failed,text); }
}
int main(int argc,char** argv)
{
    if(argc!=4) return 2;
    try {
        std::atomic<bool> cancel{false}; auto base="https://"+std::string(argv[1])+":"+argv[3];
        GoogleEndpoints endpoints; endpoints.gmail=base+"/gmail"; endpoints.certificate=argv[2];
        auto fixture=[&](const std::string& path,const Json& body=Json()) {
            HttpRequest request; request.url=base+"/fixture/"+path; request.certificate=argv[2];
            if(!body.is_null()) { request.method="POST"; request.contentType="application/json"; request.body=body.dump(); }
            auto response=requestHttps(request,cancel); requireHttpSuccess(response); return Json::parse(response.body);
        };
        Store store(":memory:"); Account a; a.name="Google"; a.email="user@example.test"; a.provider=Provider::Gmail; a.id=store.saveAccount(a);
        GmailApi api([](bool){return "fixture-access";},cancel,endpoints);
        auto sync=[&]{return syncGmailAccount(store,a,api,cancel);};
        expect(sync().downloaded==2,"initial organization fixture downloads two messages");
        auto one=store.findRemote(a.id,"gmail:a"),two=store.findRemote(a.id,"gmail:b");
        store.renameTag(a.id,"Project","Planning 🎉"); store.tag(two,"Planning 🎉",true);
        fixture("config",{{"fail","rename"}});
        rejects(sync,"a rejected label rename remains pending");
        expect(!store.labelChanges(a.id)[0].error.empty() && store.pendingChanges(a.id).size()==1,"rename failure retains both metadata and dependent membership edits");
        expect(fixture("state")["creates"]==0,"failure never creates a duplicate label under its new name");
        fixture("config",Json::object()); auto report=sync(); auto state=fixture("state");
        expect(report.applied==2 && store.labelChanges(a.id).empty() && store.pendingChanges(a.id).empty(),"rename and dependent membership reconcile on recovery");
        expect(state["labels"]["Label_1"]["name"]=="Planning 🎉" && state["memberships"]["b"].size()==3,"server preserves immutable label identity and membership");
        expect(store.message(one).tags==std::vector<std::string>{"Planning 🎉"},"received metadata uses renamed label");

        store.renameTag(a.id,"Planning 🎉","Lost response"); fixture("config",{{"drop",true}});
        rejects(sync,"lost label response is retained for idempotent recovery"); expect(store.labelChanges(a.id).size()==1,"lost response cannot acknowledge queued rename");
        fixture("config",Json::object()); sync();
        expect(store.labelChanges(a.id).empty() && fixture("state")["creates"]==0,"retry updates the same label rather than creating another");

        store.renameTag(a.id,"Lost response","First in flight"); int calls=0;
        GmailApi concurrent([&](bool) { if(++calls==3) store.renameTag(a.id,"First in flight","Latest edit"); return "fixture-access"; },cancel,endpoints);
        syncGmailAccount(store,a,concurrent,cancel);
        expect(store.labelChanges(a.id).size()==1 && store.labelChanges(a.id)[0].name=="Latest edit","old HTTP acknowledgement cannot erase a newer rename");
        expect(store.message(one).tags==std::vector<std::string>{"Latest edit"},"old post-write catalog cannot replace pending local name");
        sync(); expect(fixture("state")["labels"]["Label_1"]["name"]=="Latest edit","next check sends newest rename");

        store.renameTag(a.id,"Latest edit","Receipt check"); fixture("config",{{"bad_receipt",true}});
        rejects(sync,"mismatched response identity is not accepted"); expect(!store.labelChanges(a.id).empty(),"bad receipt keeps rename recoverable");
        fixture("config",Json::object()); sync();
        store.removeTag(a.id,"Receipt check"); fixture("config",{{"fail","delete"}});
        rejects(sync,"failed Gmail label removal remains pending");
        expect(store.message(one).tags.empty() && !store.message(one).raw.empty(),"pending removal hides tag and retains message data");
        expect(store.accountTags(a.id).back().pending=="delete","pending removal remains visible in the manager");
        fixture("config",Json::object()); sync(); state=fixture("state");
        expect(store.labelChanges(a.id).empty() && !state["labels"].contains("Label_1") && state["messages"].size()==2,"confirmed removal affects label only");

        store.removeTag(a.id,"Empty"); fixture("config",{{"wrong",true}});
        rejects(sync,"profile identity is verified before destructive label operations");
        expect(fixture("state")["labels"].contains("Label_2"),"wrong identity never receives label deletion");
        fixture("config",{{"remove","Label_2"}}); sync();
        expect(store.labelChanges(a.id).empty() && store.accountTags(a.id).empty(),"a label removed by another client acknowledges DELETE 404 safely");
        cancel=true; rejects(sync,"cancellation prevents an organization check");
        std::cout<<checks<<" organization API assertions passed\n"; return 0;
    } catch(const std::exception& error) { std::cerr<<"FAIL: "<<error.what()<<'\n'; return 1; }
}
