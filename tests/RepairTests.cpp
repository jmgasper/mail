#include "mail/Repair.h"
#include "mail/Store.h"
#include <sqlite3.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

using namespace mail;
namespace fs = std::filesystem;
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
std::string query(const std::string& path,const char* sql)
{
    sqlite3* db=nullptr; std::string result;
    if (sqlite3_open_v2(path.c_str(),&db,SQLITE_OPEN_READWRITE,nullptr)==SQLITE_OK) {
        sqlite3_stmt* s=nullptr;
        if (sqlite3_prepare_v2(db,sql,-1,&s,nullptr)==SQLITE_OK && sqlite3_step(s)==SQLITE_ROW && sqlite3_column_text(s,0))
            result=reinterpret_cast<const char*>(sqlite3_column_text(s,0));
        sqlite3_finalize(s);
    }
    sqlite3_close(db); return result;
}
// Loses the end of the file as an unclean shutdown did on BFS: SQLite had
// written those pages, but the file's last extension never reached the disk.
void repairsLostPages(const std::string& dir,const std::string& shell)
{
    const auto path=dir+"/mail.db";
    Id account=0,first=0,last=0; const int count=120;
    {
        Store store(path);
        Account a; a.name="Remote"; a.email="remote@example.test"; a.provider=Provider::Imap;
        a.incoming={"mail.example.test",993,Security::TLS,"user",{}};
        a.outgoing={"mail.example.test",465,Security::TLS,"user",{}};
        account=store.saveAccount(a);
        for (int i=0;i<count;++i) {
            RemoteLocation location{"INBOX","1",std::to_string(i+1)};
            Message m; m.account=account; m.remoteKey=imapRemoteKey(location); m.messageId="<"+std::to_string(i)+"@test>";
            m.sender="Ada <ada@example.test>"; m.to="Pat <pat@example.test>"; m.received=100+i;
            m.subject="Report "+std::to_string(i); m.text=std::string(6000,static_cast<char>('a'+i%26));
            last=store.ingestRemote(m,location); if (!first) first=last;
        }
    }
    query(path,("INSERT INTO gmail_state VALUES("+std::to_string(account)+",'12345') RETURNING 1").c_str());
    expect(query(path,"SELECT count(*) FROM gmail_state")=="1","test cursor is stored");
    fs::resize_file(path,fs::file_size(path)-24*4096);
    expect(!StoreNeedsRepair(path),"an undamaged database needs no repair");
    {
        // Without a WAL saying otherwise, SQLite notices the short file on
        // opening; with one, only when a lost page is read.
        bool damaged=false;
        try { Store store(path); store.message(last); } catch (const StoreDamaged&) { damaged=true; }
        expect(damaged,"a short database file reports damage");
    }
    expect(StoreNeedsRepair(path),"damage is remembered for the next start");
    std::vector<std::string> steps;
    auto report=RepairStore(path,[&](const std::string& step) { steps.push_back(step); },shell);
    expect(!steps.empty(),"repair reports its steps");
    expect(!StoreNeedsRepair(path),"a repaired database needs no repair");
    expect(fs::exists(report.damagedCopy) && !fs::exists(path+".recovering"),"the damaged copy is kept and no work file remains");
    expect(report.messages>0 && report.messages<count,"undamaged messages are kept and lost ones are not invented");
    expect(query(path,"PRAGMA integrity_check")=="ok","the repaired database passes the integrity check");
    expect(query(path,"SELECT count(*) FROM pragma_foreign_key_check")=="0","no row refers to a lost message");
    expect(query(path,"SELECT count(*) FROM gmail_state")=="0","Gmail starts again with a full check");
    expect(query(path,"SELECT count(*) FROM messages m JOIN message_search s ON s.rowid=m.id WHERE message_search MATCH 'Report'")
        ==std::to_string(report.messages),"search covers exactly the kept messages");
    Store store(path);
    expect(store.message(first).subject=="Report 0","early messages read back intact");
    expect(store.findRemote(account,imapRemoteKey({"INBOX","1",std::to_string(count)}))==0,"a lost message is missing, so IMAP downloads it again");
    Message again; again.account=account; again.remoteKey=imapRemoteKey({"INBOX","1",std::to_string(count)});
    again.messageId="<again@test>"; again.subject="Downloaded again"; again.received=500;
    auto id=store.ingestRemote(again,{"INBOX","1",std::to_string(count)});
    expect(store.message(id).subject=="Downloaded again","the repaired database accepts new mail");
}
void refusesUnrecoverable(const std::string& dir,const std::string& shell)
{
    const auto path=dir+"/garbage.db";
    std::ofstream(path)<<std::string(8192,'x');
    rejects([&]{ RepairStore(path,{},shell); },"a file with nothing to recover is not replaced");
    expect(fs::file_size(path)==8192 && !fs::exists(path+".recovering"),"a failed repair leaves the damaged file as it was");
    auto aside=SetAsideStore(path);
    expect(!fs::exists(path) && fs::file_size(aside)==8192,"starting over keeps the damaged file aside");
}
}
int main()
{
    std::string shell=std::getenv("MAIL_SQLITE3") ? std::getenv("MAIL_SQLITE3") : "";
    for (const char* candidate : {"/bin/sqlite3","/usr/bin/sqlite3"}) if (shell.empty() && access(candidate,X_OK)==0) shell=candidate;
    if (shell.empty()) { std::cout<<"Skipped: repairing needs the sqlite3 command\n"; return 77; }
    char dir[]="mail-repair-XXXXXX";
    if (!mkdtemp(dir)) return 2;
    try {
        repairsLostPages(dir,shell); refusesUnrecoverable(dir,shell);
        fs::remove_all(dir);
        std::cout<<assertions<<" assertions passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<"FAIL: "<<error.what()<<"\nKept "<<dir<<" for inspection.\n"; return 1;
    }
}
