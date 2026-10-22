#include "mail/Mime.h"
#include "mail/Store.h"
#include <sqlite3.h>
#include <filesystem>
#include <iostream>
#include <random>
#include <stdexcept>
#include <unistd.h>

using namespace mail;
namespace {
int assertions=0;
void expect(bool test,const char* message)
{
    ++assertions;
    if (!test) throw std::runtime_error(message);
}
template<class F> void rejects(F f,const char* message)
{
    bool threw=false;
    try { f(); } catch (const std::exception&) { threw=true; }
    expect(threw,message);
}
Account account(const char* name)
{
    Account a; a.name=name; a.email=std::string(name)+"@example.test"; return a;
}
Message message(Id accountId,const char* key,const char* id,Timestamp when=100)
{
    Message m; m.account=accountId; m.remoteKey=key; m.messageId=id; m.received=when;
    m.sender="Ada <ada@example.test>"; m.to="Pat <pat@example.test>";
    m.subject="Release notes"; m.text="The release has Unicode café and emoji 🎉.";
    return m;
}
void storage(const std::string& path)
{
    Id a,b,root,reply,starred,hidden;
    {
        Store store(path);
        a=store.saveAccount(account("Personal")); b=store.saveAccount(account("Work"));
        Account privateAccount=account("Private"); privateAccount.unified=false;
        hidden=store.saveAccount(privateAccount);
        auto child=message(a,"reply","<reply@test>",200); child.references={"<root@test>"};
        reply=store.ingest(child);
        root=store.ingest(message(a,"root","<root@test>"));
        expect(store.message(root).thread==store.message(reply).thread,"out-of-order references must thread");
        store.ingest(message(b,"root","<root@test>"));
        store.ingest(message(hidden,"root","<root@test>"));
        auto separate=message(a,"different","<other@test>",300);
        starred=store.ingest(separate);
        expect(store.message(root).thread!=store.message(starred).thread,"equal subjects must not merge unrelated mail");
        auto rows=store.conversations({},500);
        expect(rows.size()==3,"unified inbox isolates accounts and excludes opted-out account");
        expect(rows.front().latest==300,"conversations sort newest first");
        Query privateQuery; privateQuery.account=hidden;
        expect(store.conversations(privateQuery,500).size()==1,"private accounts remain individually accessible");

        // A later bridge message merges two previously independent reference trees.
        auto bridge=message(a,"bridge","<bridge@test>",400); bridge.references={"<root@test>","<other@test>"};
        store.ingest(bridge);
        expect(store.thread(store.message(root).thread).size()==4,"late ancestry merges entire conversations");
        expect(store.conversations({},500).size()==2,"merged conversations do not leave duplicate rows");

        store.setRead(root,true); store.setStarred(root,true); store.tag(root,"Projects",true);
        store.snooze(root,1000); store.ingest(message(a,"root","<root@test>"));
        auto saved=store.message(root);
        expect(!saved.unread && saved.starred && saved.tags==std::vector<std::string>{"Projects"} && saved.snoozedUntil==1000,
            "re-ingestion must preserve local actions");
        Query snoozed; snoozed.snoozedOnly=true;
        expect(store.conversations(snoozed,999).size()==1,"snoozed view includes future messages");
        expect(store.conversations(snoozed,1000).empty(),"snooze expires at the exact UTC boundary");
        Query tag; tag.tag="Projects";
        expect(store.conversations(tag,999).empty(),"snoozed items stay hidden from ordinary views");
        expect(store.conversations(tag,1000).size()==1,"expired messages return without modifying mailbox data");
        store.move(root,"archive"); expect(store.message(root).snoozedUntil==0,"moving a message clears snooze");
        store.ingest(message(a,"root","<root@test>"));
        expect(store.message(root).folder=="archive","re-ingestion must not undo an archive");

        Query search; search.folder=""; search.text="cafe release";
        expect(store.conversations(search,1000).size()==2,"Unicode FTS searches words across fields and accounts");
        search.text="\" OR delete";
        expect(store.conversations(search,1000).empty(),"search text is not executed as query syntax");
        search.text="  \t ";
        expect(!store.conversations(search,1000).empty(),"whitespace search is valid");
        SmartFolder folder; folder.name="Projects"; folder.query=tag;
        auto folderId=store.saveSmartFolder(folder); folder.id=folderId; folder.query.includeSnoozed=true;
        expect(store.saveSmartFolder(folder)==folderId,"smart folder updates keep their identity");
        expect(store.smartFolders().size()==1 && store.smartFolders()[0].query.includeSnoozed,"saved queries retain all flags");

        Filter filter; filter.account=a; filter.name="Newsletters"; filter.senderContains="NEWS@";
        filter.addTag="Reading"; filter.moveTo="archive"; filter.markRead=true; filter.stop=true;
        store.saveFilter(filter);
        Filter later; later.name="Later rule"; later.subjectContains="Release"; later.star=true;
        store.saveFilter(later);
        auto news=message(a,"news","<news@test>"); news.sender="News <news@example.test>";
        auto newsId=store.ingest(news); auto stored=store.message(newsId);
        expect(stored.folder=="archive" && !stored.unread && !stored.starred && stored.tags[0]=="Reading","ordered filters apply atomically and honor stop");
        news.account=b;
        stored=store.message(store.ingest(news));
        expect(stored.folder=="inbox" && stored.starred,"account-specific filters never affect another account");
        store.setStarred(newsId,true); store.ingest(message(a,"news","<news@test>"));
        expect(store.message(newsId).starred,"filters do not rerun on an existing message");

        auto bad=message(a,"bad","<bad@test>"); bad.tags={""};
        auto before=store.conversations({},500).size();
        rejects([&]{store.ingest(bad);},"invalid metadata must fail ingestion");
        expect(store.conversations({},500).size()==before,"failed ingestion rolls back message and thread records");
        bad.tags.clear(); expect(store.ingest(bad)>0,"connection remains usable after rollback");

        auto binary=message(a,"binary","<binary@test>");
        binary.attachments.push_back({"image.png","image/png","image1",std::string("a\0b",3),true});
        auto binaryId=store.ingest(binary); auto loaded=store.message(binaryId);
        expect(loaded.attachments.size()==1 && loaded.attachments[0].data==std::string("a\0b",3),"binary attachments must survive storage");
        store.ingest(binary);
        expect(store.message(binaryId).attachments.size()==1,"repeated ingestion does not duplicate attachments");
        Query attachments; attachments.attachments=true;
        expect(store.conversations(attachments,500).size()==1,"attachment smart filter finds the correct conversation");
        auto gmail=message(a,"g1","<g1@test>"); gmail.providerThread="abc";
        auto g1=store.ingest(gmail); gmail.remoteKey="g2"; gmail.messageId="<g2@test>";
        gmail.references={"<root@test>"};
        auto g2=store.ingest(gmail);
        expect(store.message(g1).thread==store.message(g2).thread && store.message(g1).thread!=store.message(root).thread,
            "provider thread IDs are authoritative");
    }
    {
        Store reopened(path);
        expect(reopened.accounts().size()==3 && reopened.smartFolders().size()==1 && reopened.filters().size()==2,"accounts and organization survive restart");
        expect(reopened.message(root).folder=="archive" && reopened.message(root).tags[0]=="Projects","message edits survive restart");
    }
    sqlite3* db=nullptr;
    expect(sqlite3_open(path.c_str(),&db)==SQLITE_OK,"open test database");
    sqlite3_exec(db,"PRAGMA user_version=999",nullptr,nullptr,nullptr); sqlite3_close(db);
    rejects([&]{Store newer(path);},"older apps cannot open a future schema");
}
void mime()
{
    const std::string raw="From: =?UTF-8?Q?Ada_=F0=9F=8C=BF?= <ada@example.test>\r\n"
        "To: Pat <pat@example.test>\r\nSubject: =?UTF-8?B?UmVsZWFzZSDwn46J?=\r\n"
        "\t=?UTF-8?Q?_notes?=\r\nDate: Wed, 21 Oct 2026 11:30:00 +1100\r\n"
        "Message-ID: <hello@example.test>\r\nReferences: <first@test>\r\nIn-Reply-To: <second@test>\r\n"
        "MIME-Version: 1.0\r\nContent-Type: multipart/mixed; boundary=outer\r\n\r\nPreamble\r\n"
        "--outer\r\nContent-Type: multipart/alternative; boundary=inner\r\n\r\n"
        "--inner\r\nContent-Type: text/plain; charset=iso-8859-1\r\nContent-Transfer-Encoding: quoted-printable\r\n\r\n"
        "Caf=E9 =\r\nrelease\r\n--inner\r\nContent-Type: text/html; charset=utf-8\r\n\r\n<p>Café release 🎉</p>\r\n--inner--\r\n"
        "--outer\r\nContent-Type: image/png\r\nContent-ID: <inline@test>\r\n"
        "Content-Disposition: inline; filename*=utf-8''..%2Fpic%20%F0%9F%8C%BF.png\r\n"
        "Content-Transfer-Encoding: base64\r\n\r\nYQBi\r\n--outer--\r\nEpilogue";
    auto m=parseMessage(raw);
    expect(m.subject=="Release 🎉 notes","folded adjacent encoded words decode");
    expect(m.sender=="Ada 🌿 <ada@example.test>","UTF-8 sender decodes");
    expect(m.text=="Café release","quoted printable soft breaks and legacy charsets decode");
    expect(m.html=="<p>Café release 🎉</p>","multipart alternative retains HTML");
    expect(m.references==std::vector<std::string>{"<first@test>","<second@test>"},"reply references are retained");
    expect(m.received==1792542600,"mail dates honor timezone offsets");
    expect(m.raw==raw,"raw message remains byte-exact");
    expect(m.attachments.size()==1 && m.attachments[0].data==std::string("a\0b",3),"MIME attachments decode binary data");
    expect(m.attachments[0].name=="pic 🌿.png" && m.attachments[0].inlineImage && m.attachments[0].contentId=="inline@test","RFC2231 names are safe leaf names and CID is retained");
    expect(plainFromHtml("<head><style>bad</style></head><p>Hello &amp; café &#x1f389;</p><script>bad</script><img src='https://tracker'>")
        =="Hello & café 🎉","HTML fallback omits executable and tracking content");
    auto html=parseMessage("Subject: HTML\nContent-Type: text/html\n\n<p>Hello</p><p>World</p>");
    expect(html.text=="Hello\nWorld","HTML-only mail has a readable fallback");
    expect(escapeHtml("<b>&\"'")=="&lt;b&gt;&amp;&quot;&#39;","HTML escaping covers attribute metacharacters");
    std::string bytes; for (int i=0;i<256;++i) bytes+=static_cast<char>(i);
    for (std::size_t n=0;n<=bytes.size();++n)
        expect(base64Decode(base64Encode(bytes.substr(0,n)))==bytes.substr(0,n),"all binary lengths round-trip through base64");
    rejects([]{base64Decode("YQ=Z");},"misplaced padding is rejected");
    rejects([]{base64Decode("YQ");},"truncated base64 is rejected");
    rejects([]{parseMessage("not an email");},"non-mail input is rejected");
    rejects([]{parseMessage("Content-Type: multipart/mixed; boundary=x\n\n--x\n\ntruncated");},"incomplete multipart is not silently imported");
    rejects([]{parseMessage("Subject: "+std::string(65536,'x')+"\n\nBody");},"oversized headers are bounded");
    std::mt19937 rng(42);
    // Exercise malformed transfer data and structure under the sanitizers.
    for (int i=0;i<500;++i) {
        std::string mutated=raw;
        for (int j=0;j<4;++j) mutated[rng()%mutated.size()]=static_cast<char>(rng()%256);
        try { parseMessage(mutated); } catch (const std::exception&) { }
    }
}
void synchronization()
{
    Store store(":memory:");
    auto a=account("Remote"); a.provider=Provider::Imap;
    a.incoming={"mail.example.test",993,Security::TLS,"user",{}};
    a.outgoing={"mail.example.test",465,Security::TLS,"user",{}};
    a.id=store.saveAccount(a);
    RemoteLocation location{"INBOX","7","10"};
    auto m=message(a.id,"ignored","<sync@test>"); m.remoteKey=imapRemoteKey(location);
    auto id=store.ingestRemote(m,location);
    MailboxSnapshot snapshot{{"INBOX","Inbox","inbox"},"7",{{m.remoteKey,"10",false,true}}};
    store.reconcileMailbox(a.id,snapshot,false);
    expect(!store.message(id).unread && store.message(id).starred,"server read/star changes update cached messages");
    store.setRead(id,false);
    auto pending=store.pendingChanges(a.id);
    expect(pending.size()==1 && pending[0].kind=="read" && pending[0].value=="0","local read changes are journaled");
    store.reconcileMailbox(a.id,snapshot,false);
    expect(store.message(id).unread,"snapshot cannot overwrite a pending read action");
    auto old=pending[0].id;
    store.setRead(id,true); auto replacement=store.pendingChanges(a.id)[0].id;
    store.acknowledgeChange(old);
    expect(replacement!=old && store.pendingChanges(a.id).size()==1,"stale acknowledgement cannot erase a newer edit");
    store.acknowledgeChange(replacement);
    expect(store.pendingChanges(a.id).empty(),"acknowledgement clears only its operation");
    store.tag(id,"Keep",true); store.move(id,"archive");
    pending=store.pendingChanges(a.id);
    expect(pending.size()==1 && pending[0].kind=="move","server moves are durably queued");
    RemoteLocation destination{"Archive","8","25"};
    RemoteMove planned{pending[0].location,destination,"25","moved",false};
    expect(store.beginMove(pending[0],planned),"move acknowledgement requires a claimed journal");
    store.acknowledgeMove(pending[0],destination);
    expect(store.findRemote(a.id,imapRemoteKey(destination))==id && store.message(id).tags[0]=="Keep","move changes remote identity while preserving local message and tags");
    snapshot.messages.clear(); store.reconcileMailbox(a.id,snapshot,false);
    expect(store.message(id).folder=="archive" && store.thread(store.message(id).thread).size()==1,"source reconciliation cannot hide an acknowledged destination");
    MailboxSnapshot removed{{"Archive","Archive","archive"},"9",{}};
    store.reconcileMailbox(a.id,removed,false);
    expect(store.thread(store.message(id).thread).empty(),"UIDVALIDITY changes hide stale cached identities");
    expect(store.message(id).raw==m.raw,"missing remote messages retain their recoverable cache");
    auto changed=a; changed.incoming.username="another-user";
    rejects([&]{store.saveAccount(changed);},"changing a downloaded account's identity cannot send mutations to another mailbox");

    auto pop=a; pop.id=0; pop.provider=Provider::Pop3; pop.name="POP3"; pop.id=store.saveAccount(pop);
    auto local=message(pop.id,"pop3:stable","<pop@test>");
    auto popId=store.ingestRemote(local,{"INBOX","","1"});
    store.move(popId,"archive"); store.setRead(popId,true);
    expect(store.pendingChanges(pop.id).empty(),"POP3 organization is local and never queues invalid server operations");
    MailboxSnapshot popSnapshot{{"INBOX","Inbox","inbox"},"",{{"pop3:stable","9",true,false}}};
    store.reconcileMailbox(pop.id,popSnapshot,true);
    expect(store.message(popId).folder=="archive" && !store.message(popId).unread,"POP3 refresh preserves local folder and read state");
    popSnapshot.messages.clear(); store.reconcileMailbox(pop.id,popSnapshot,true);
    expect(store.thread(store.message(popId).thread).size()==1,"POP3 server deletion does not delete the downloaded copy");
    store.saveRemoteFolders(a.id,{{"INBOX","Inbox","inbox"},{"Archive","Archive","archive"}});
    expect(store.remoteFolders(a.id).size()==2,"remote folder catalog is persistent");
    auto removedMail=message(a.id,imapRemoteKey({"Removed","1","1"}).c_str(),"<removed@test>");
    auto removedId=store.ingestRemote(removedMail,{"Removed","1","1"});
    store.saveRemoteFolders(a.id,{{"INBOX","Inbox","inbox"}});
    expect(store.thread(store.message(removedId).thread).empty(),"deleted IMAP folders disappear from mailbox views");
    expect(store.message(removedId).messageId=="<removed@test>","deleted folders retain recoverable cached content");
    std::string longFolder="remote:"+std::string(700,'x');
    auto longMail=message(pop.id,"pop3:long-folder","<long@test>"); longMail.folder=longFolder;
    expect(store.message(store.ingest(longMail)).folder==longFolder,"long server folder paths survive storage");
    store.removeAccount(a.id);
    expect(store.accounts().size()==1 && store.remoteFolders(a.id).empty(),"removing an account cleans its cache and leaves other accounts intact");
    expect(store.message(popId).account==pop.id,"account removal preserves the other account's mail");
}
void migrations(const std::string& directory)
{
    std::string previousCredential;
    for (int version : {1,2,3,4,5,6,7,8,9,10,11,12,13}) {
        auto path=directory+"/v"+std::to_string(version)+".db";
        Id id,draftId=0,reminderId=0;
        {
            Store store(path);
            auto a=account("Migration"); a.id=store.saveAccount(a);
            id=store.ingest(message(a.id,"saved","<migration@test>"));
            store.tag(id,"Keep",true);
            if (version>=4) { Draft draft; draft.account=a.id; draft.subject="Keep draft"; draft.text="Draft migration"; draftId=store.saveDraft(draft); }
            if (version==8) {
                store.snooze(id,100); auto reminders=store.reminders(100);
                expect(reminders.size()==1,"prepare existing schema-8 reminder"); reminderId=reminders[0].id;
                store.markRemindersNotified({reminderId});
            }
        }
        sqlite3* db=nullptr;
        expect(sqlite3_open(path.c_str(),&db)==SQLITE_OK,"open migration fixture");
        auto sql="PRAGMA user_version="+std::to_string(version)+";";
        sql+="DROP TABLE gmail_draft_matches;";
        if (version<13) sql+="DROP TABLE draft_editors;";
        if (version<12) sql+="DROP TABLE gmail_drafts; ALTER TABLE drafts DROP COLUMN revision;";
        else if (version<13) sql+="ALTER TABLE gmail_drafts DROP COLUMN conflict; ALTER TABLE gmail_drafts DROP COLUMN missing; ALTER TABLE gmail_drafts DROP COLUMN observed_id; ALTER TABLE gmail_drafts DROP COLUMN observed_thread; ALTER TABLE gmail_drafts DROP COLUMN observed_raw;";
        if (version<11) sql+="ALTER TABLE pending_changes DROP COLUMN error;";
        if (version<10) sql+="DROP TABLE remote_moves;";
        else if (version<11) sql+="ALTER TABLE remote_moves DROP COLUMN active; ALTER TABLE remote_moves DROP COLUMN revision; ALTER TABLE remote_moves DROP COLUMN error;";
        if (version<9) sql+="DROP TABLE draft_images;";
        if (version<8) sql+="DROP TRIGGER snooze_updated; DROP TRIGGER snooze_inserted; DROP TABLE snooze_reminders;";
        if (version<7) sql+="DROP TABLE label_changes; ALTER TABLE filters DROP COLUMN position;";
        if (version<6) sql+="DROP TABLE sent_copies; ALTER TABLE accounts DROP COLUMN sent_mode; ALTER TABLE accounts DROP COLUMN sent_folder;";
        if (version<5) sql+="DROP VIEW effective_tags; DROP TABLE gmail_message_labels; DROP TABLE gmail_labels; DROP TABLE gmail_state;";
        if (version<3) sql+="ALTER TABLE accounts DROP COLUMN credential_id;";
        if (version==1) sql+="DROP TABLE pending_changes; DROP TABLE remote_messages; DROP TABLE remote_folders;";
        if (version<4) sql+="DROP TABLE outbox_recipients; DROP TABLE outbox; DROP TABLE draft_runs; DROP TABLE draft_refs; DROP TABLE draft_attachments; DROP TABLE drafts;";
        auto result=sqlite3_exec(db,sql.c_str(),nullptr,nullptr,nullptr); sqlite3_close(db);
        expect(result==SQLITE_OK,"prepare historical schema fixture");
        std::string credential;
        {
            Store store(path);
            auto a=store.accounts()[0]; credential=a.credentialId;
            expect(credential.size()==32 && credential!=previousCredential,"migration creates a distinct opaque credential identifier per database");
            expect(store.message(id).tags==std::vector<std::string>{"Keep"},"schema migration preserves messages and organization");
            if (version>=4) expect(store.draft(draftId).text=="Draft migration","Gmail schema migration preserves existing drafts");
            if (version==8) {
                auto reminders=store.reminders(100);
                expect(reminders.size()==1 && reminders[0].id==reminderId && reminders[0].notified && reminders[0].due==100,
                    "image migration preserves expired reminders and notification acknowledgements");
                auto draft=store.draft(draftId);
                expect(draft.images.empty(),"old drafts have no positional image metadata after migration");
                store.snooze(id,200);
                expect(store.reminders(100).empty() && store.reminders(200).size()==1,"schema-8 snooze triggers still work after image migration");
            }
            a.name="Renamed"; store.saveAccount(a);
            expect(store.accounts()[0].credentialId==credential,"editing account settings preserves its key-store identity");
        }
        {
            Store store(path);
            expect(store.accounts()[0].credentialId==credential,"credential identity survives reopening");
            auto a=store.accounts()[0]; store.removeAccount(a.id); a.id=0;
            store.saveAccount(a);
            expect(store.accounts()[0].credentialId!=credential,"reusing an account row ID never reuses a removed account's credentials");
        }
        previousCredential=credential;
    }
}
}
int main()
{
    char path[]="mail-test-XXXXXX";
    if (!mkdtemp(path)) return 2;
    auto cleanup=[&] {
        // Collect before unlinking: older Haiku BFS iteration can skip entries
        // if the directory is modified during a traversal.
        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::directory_iterator(path)) files.push_back(entry.path());
        for (const auto& file : files) std::filesystem::remove(file);
        std::filesystem::remove(path);
    };
    try {
        mime(); storage(std::string(path)+"/mail.db"); synchronization(); migrations(path);
        cleanup();
        std::cout<<assertions<<" assertions passed; malformed MIME corpus completed\n";
        return 0;
    } catch (const std::exception& error) {
        cleanup();
        std::cerr<<"FAIL: "<<error.what()<<'\n'; return 1;
    }
}
