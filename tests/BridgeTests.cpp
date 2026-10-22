#include "mail/Sync.h"
#include "mail/Outbox.h"
#include "mail/Mime.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

using namespace mail;
namespace {
int checks=0;
void check(bool ok,const char* why) {++checks;if(!ok) throw std::runtime_error(why);}
std::vector<Message> folder(Store& store,Id account,const std::string& name)
{
    Query query; query.account=account; query.folder=name; query.includeSnoozed=true;
    std::vector<Message> result;
    for(const auto& conversation:store.conversations(query,std::time(nullptr)))
        for(auto message:store.thread(conversation.id)) if(message.account==account && message.folder==name) result.push_back(std::move(message));
    return result;
}
template<class F> void eventually(F fn,const char* why)
{
    auto end=std::chrono::steady_clock::now()+std::chrono::seconds(35);
    do {if(fn()) {check(true,why);return;} std::this_thread::sleep_for(std::chrono::milliseconds(100));} while(std::chrono::steady_clock::now()<end);
    check(false,why);
}
}
int main(int argc,char** argv)
{
    if(argc!=3) return 2;
    try {
        std::ifstream input(argv[1]); nlohmann::json config; input>>config;
        const auto directory=config.at("directory").get<std::string>();
        Store store(directory+"/mail.db"); std::vector<Account> accounts; std::vector<std::string> passwords;
        auto saved=store.accounts();
        for(std::size_t i=0;i<config.at("accounts").size();++i) {
            const auto& source=config["accounts"][i]; Account a; a.provider=Provider::ProtonBridge; a.name="Bridge fixture "+std::to_string(i+1); a.email=source.at("email");
            a.incoming={"127.0.0.1",config.at("imap"),Security::StartTLS,a.email,config.at("certificate")};
            a.outgoing={"127.0.0.1",config.at("smtp"),Security::StartTLS,a.email,config.at("certificate")};
            if(!saved.empty()) {check(i<saved.size() && saved[i].email==a.email,"restart keeps account identities"); a.id=saved[i].id;}
            a.id=store.saveAccount(a); accounts.push_back(a); passwords.push_back(source.at("password"));
        }
        check(accounts.size()==3,"three independent Bridge identities"); std::atomic<bool> cancelled{false};
        auto sync=[&](std::size_t i) {auto r=syncAccount(store,accounts[i],passwords[i],cancelled); if(!r.errors.empty()) throw std::runtime_error(r.errors[0]);return r;};
        auto passwordFor=[&](const Account& a) {for(std::size_t i=0;i<accounts.size();++i) if(accounts[i].id==a.id) return passwords[i];throw std::runtime_error("unexpected sending account");};
        if(std::string(argv[2])=="exercise") {
            auto untrusted=accounts[0]; untrusted.incoming.certificate.clear(); bool denied=false;
            try {connectMail(untrusted,passwords[0],cancelled)->folders();} catch(const NetworkError& e) {denied=e.reason==NetworkFailure::Certificate;}
            check(denied,"untrusted actual Bridge certificate is rejected"); denied=false;
            try {connectMail(accounts[0],"incorrect-disposable-password",cancelled)->folders();} catch(const NetworkError& e) {denied=e.reason==NetworkFailure::Authentication;}
            check(denied,"incorrect Bridge password is rejected");
            for(std::size_t i=0;i<accounts.size();++i) {
                check(sync(i).downloaded==0,"new Bridge account has no mail"); auto folders=store.remoteFolders(accounts[i].id);
                for(const char* role:{"inbox","sent","archive","trash"}) check(std::any_of(folders.begin(),folders.end(),[&](const RemoteFolder& f){return f.role==role;}),"Bridge standard folders are mapped");
                check(sentCopyMode(accounts[i])==SentCopyMode::Server,"Bridge uses server-managed Sent");
            }
            Draft d; d.account=accounts[0].id; d.to=accounts[1].email; d.bcc=accounts[2].email; d.subject="Mail and Bridge — café 🎉"; d.messageId=newMessageId();
            d.text="Hello café 🎉 before "; d.runs={{0,true,true,false,"https://example.test/"},{5,false,false,false,{}}};
            Attachment png{"colours café.png","image/png","bridge-image@test",base64Decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+a5e8AAAAASUVORK5CYII="),true};
            d.attachments={png,{"binary.txt","application/octet-stream",{},std::string("A\0B\xff",4),false}};
            auto marker=imagePlaceholder(png); d.images={{d.text.size(),marker.size(),png.contentId}}; d.text+=marker+" after.\n.leading dot\nM1 OK not a response\n";
            store.enqueueDraft(store.saveDraft(d),std::time(nullptr)); auto delivery=deliverQueued(store,passwordFor,cancelled);
            check(delivery.sent==1 && !delivery.failed && !delivery.uncertain && store.outbox().empty(),"actual Bridge accepts one rich SMTP delivery");
            eventually([&]{for(std::size_t i=0;i<accounts.size();++i) sync(i);return folder(store,accounts[1].id,"inbox").size()==1 && folder(store,accounts[2].id,"inbox").size()==1 && store.sentCopies().empty();},"receive, BCC delivery and server Sent reconciliation");
            check(folder(store,accounts[0].id,"inbox").empty(),"sender does not receive the original");
            auto received=folder(store,accounts[1].id,"inbox")[0];
            check(received.subject==d.subject && received.messageId==d.messageId,"Bridge preserves Unicode subject and Message-ID");
            check(received.text.find("café 🎉")!=std::string::npos && received.text.find(".leading dot")!=std::string::npos,"decrypted body retains Unicode and SMTP dot transparency");
            check(received.attachments.size()==2,"decrypted message retains both attachments");
            for(const auto& expected:d.attachments) check(std::any_of(received.attachments.begin(),received.attachments.end(),[&](const Attachment& a){return a.name==expected.name && a.data==expected.data;}),"Bridge retains binary file bytes and Unicode filenames");
            check(received.html.find("before")<received.html.find("<img") && received.html.find("<img")<received.html.find(" after"),"Bridge retains the positioned inline image");
            check(received.raw.substr(0,received.raw.find("\r\n\r\n")).find(accounts[2].email)==std::string::npos,"visible recipient cannot see the BCC address");
            store.setRead(received.id,true); store.setStarred(received.id,true); store.move(received.id,"archive"); sync(1);
            check(store.pendingChanges(accounts[1].id).empty() && store.message(received.id).folder=="archive","read, star and archive reach real Bridge");
            store.move(received.id,"trash"); sync(1); check(store.pendingChanges(accounts[1].id).empty(),"trash move reaches Bridge");
            store.move(received.id,"inbox"); sync(1); check(store.pendingChanges(accounts[1].id).empty(),"restore to inbox reaches Bridge");
            auto reply=replyDraft(store.message(received.id),accounts[1],false); reply.text="Reply from the second Bridge identity 🎉"; reply.runs.clear(); reply.images.clear(); reply.attachments.clear();
            store.enqueueDraft(store.saveDraft(reply),std::time(nullptr)); delivery=deliverQueued(store,passwordFor,cancelled);
            check(delivery.sent==1 && store.outbox().empty(),"reply uses the second account's SMTP identity");
            eventually([&]{sync(0);sync(1);return folder(store,accounts[0].id,"inbox").size()==1 && store.sentCopies().empty();},"reply arrives and both Sent copies reconcile");
            auto answer=folder(store,accounts[0].id,"inbox")[0]; check(answer.sender.find(accounts[1].email)!=std::string::npos,"reply sender identity retained");
            auto sent=folder(store,accounts[0].id,"sent"); check(sent.size()==1 && answer.thread==sent[0].thread,"received reply threads with the sender's original Sent message");
        } else if(std::string(argv[2])!="reopen") throw std::runtime_error("Unknown fixture phase");
        for(std::size_t i=0;i<accounts.size();++i) {
            sync(i); check(folder(store,accounts[i].id,"inbox").size()==1,"all inboxes retain their own message after synchronization");
            check(folder(store,accounts[i].id,"sent").size()==(i<2?1u:0u),"no duplicate Sent copies per identity");
            check(store.pendingChanges(accounts[i].id).empty(),"no unresolved server mutations");
        }
        auto received=folder(store,accounts[1].id,"inbox")[0]; check(!received.unread && received.starred,"read and star survive move/restart");
        check(store.sentCopies().empty() && store.outbox().empty(),"no pending delivery or copy work");
        check(deliverQueued(store,passwordFor,cancelled).sent==0,"rechecking never resends the completed mail");
        std::cout<<"Native Mail / real Bridge: "<<checks<<" checks passed ("<<argv[2]<<")\n"; return 0;
    } catch(const std::exception& e) {std::cerr<<"FAIL Mail / real Bridge: "<<e.what()<<'\n'; return 1;}
}
