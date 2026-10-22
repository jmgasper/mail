#include "mail/Store.h"
#include "mail/Mime.h"
#include "mail/Snooze.h"
#include <sqlite3.h>
#include <ctime>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <unistd.h>
using namespace mail;
namespace {
int checks=0;
void check(bool value,const char* why) { ++checks; if(!value) throw std::runtime_error(why); }
template<class F> void rejects(F f,const char* why) { bool rejected=false; try { f(); } catch(const std::exception&) { rejected=true; } check(rejected,why); }
void zone(const char* value) { setenv("TZ",value,1); tzset(); }
void clockTests()
{
    zone("UTC0");
    check(localSnoozeTimes("2024-02-29","12:30")==std::vector<Timestamp>{1709209800},"leap day has exact UTC instant");
    for(const auto& value:std::vector<std::pair<std::string,std::string>>{{"2023-02-29","12:30"},{"2024-04-31","12:30"},{"2024-12-01","24:00"},{"2024-12-01","12:60"},{"2024-1-01","09:00"},{"2024-12-01","9:00"},{"2024-12-01","12:00Z"},{"2024-12-0x","12:00"}})
        rejects([&]{localSnoozeTimes(value.first,value.second);},"invalid civil time is rejected");
    rejects([]{snoozePreset(std::numeric_limits<Timestamp>::max(),SnoozePreset::Hour);},"preset rejects timestamp overflow");
    zone(":Australia/Hobart");
    rejects([]{localSnoozeTimes("2026-10-04","02:30");},"Hobart spring clock gap is rejected instead of normalized");
    auto repeated=localSnoozeTimes("2026-04-05","02:30");
    check(repeated.size()==2 && repeated[1]-repeated[0]==3600,"Hobart autumn clock repeat offers two instants");
    check(localSnoozeClock(repeated[0])=="02:30" && localSnoozeClock(repeated[1])=="02:30","both repeated instants preserve requested civil time");
    check(describeSnoozeTime(repeated[0]).find("+1100")!=std::string::npos && describeSnoozeTime(repeated[1]).find("+1000")!=std::string::npos,"repeated time descriptions distinguish offsets");
    auto before=localSnoozeTimes("2026-10-03","09:00")[0],tomorrow=snoozePreset(before,SnoozePreset::Tomorrow);
    check(tomorrow-before==23*3600 && localSnoozeClock(tomorrow)=="09:00","tomorrow uses calendar day over spring transition");
    before=localSnoozeTimes("2026-04-04","09:00")[0]; tomorrow=snoozePreset(before,SnoozePreset::Tomorrow);
    check(tomorrow-before==25*3600 && localSnoozeDate(tomorrow)=="2026-04-05","tomorrow uses calendar day over autumn transition");
    auto monday=localSnoozeTimes("2026-10-05","09:00")[0];
    check(localSnoozeDate(snoozePreset(monday,SnoozePreset::Monday))=="2026-10-12","next Monday advances a week when today is Monday");
    check(snoozePreset(repeated[0],SnoozePreset::Hour)==repeated[1],"one hour is elapsed time during a repeated clock hour");
    zone(":Australia/Lord_Howe");
    auto half=localSnoozeTimes("2026-04-05","01:45"); check(half.size()==2 && half.back()-half.front()==1800,"half-hour daylight saving repeat exposes both instants");
    rejects([]{localSnoozeTimes("2026-10-04","02:15");},"half-hour clock gap is rejected");
    zone(":Pacific/Apia");
    rejects([]{localSnoozeTimes("2011-12-30","09:00");},"skipped calendar day is rejected");
    zone("UTC0");
}
Id message(Store& store,Id account,const char* key,const char* references="")
{
    auto m=parseMessage(std::string("From: author@example.test\r\nSubject: Reminder\r\nMessage-ID: <")+key+"@example.test>\r\n"+references+"\r\nBody\r\n");
    m.account=account; m.remoteKey=key; m.received=100; return store.ingest(m);
}
void storage(const std::string& path)
{
    Store store(path); Account a; a.name="Local"; a.id=store.saveAccount(a); Account b=a; b.id=0; b.name="Separate"; b.unified=false; b.id=store.saveAccount(b);
    auto first=message(store,a.id,"first"),reply=message(store,a.id,"reply","References: <first@example.test>\r\n"),other=message(store,b.id,"other");
    store.snooze(std::vector<Id>{first,reply,first},1000); store.snooze(other,1100);
    check(store.reminders(999).empty(),"future reminders are not due");
    auto due=store.reminders(1000); check(due.size()==2 && due[0].thread==due[1].thread,"all thread members become due at the exact boundary");
    Query q; check(store.conversations(q,999).empty() && store.conversations(q,1000).size()==1,"snooze expiry reveals the original conversation without a notification worker");
    check(store.message(first).unread && store.message(first).folder=="inbox","expiry preserves read state and folder");
    auto old=due[0].id; store.markRemindersNotified({old});
    { Store reopened(path); auto saved=reopened.reminders(1200); check(saved.size()==3 && saved[0].notified && saved[2].account==b.id,"overdue and notification state survive restart, including unified opt-out accounts"); }
    store.snooze(first,2000); store.markRemindersNotified({old}); store.dismissReminders({old});
    due=store.reminders(2000); bool found=false;
    for(const auto& item:due) if(item.message==first) found=item.id!=old && !item.notified;
    check(found,"older notification acknowledgement cannot erase or silence a newer snooze");
    store.snooze(reply,0); check(store.message(reply).snoozedUntil==0 && store.reminders(2000).size()==2,"end snooze clears reminder and deadline");
    store.move(first,"archive"); check(store.reminders(2000).size()==1 && store.message(first).snoozedUntil==0,"explicit folder move cancels snooze and reminders");
    store.snooze(first,3000); store.snooze(reply,3000);
    rejects([&]{store.snooze(std::vector<Id>{first,999999},4000);},"batch snooze rejects missing messages");
    check(store.message(first).snoozedUntil==3000,"invalid batch rolls back earlier messages");
    auto reminders=store.reminders(3000); std::vector<Id> ids; for(const auto& item:reminders) ids.push_back(item.id);
    store.dismissReminders(ids); check(store.reminders(3000).empty() && !store.message(first).raw.empty(),"dismissing reminders keeps message bytes and folders");
    store.move(reply,"trash"); store.snooze(reply,5000); check(store.message(reply).snoozedUntil==0,"Trash is not snoozed");
    rejects([&]{store.snooze(first,-1);},"negative deadline rejected");
    store.snooze(other,9000); store.removeAccount(b.id); check(store.reminders(9000).empty(),"account removal cascades its reminders");
    // Model a schema-7 database with both stale expired and still-active snoozes.
    auto future=static_cast<Timestamp>(std::time(nullptr))+86400; store.snooze(first,future);
    auto expired=message(store,a.id,"expired"); store.snooze(expired,1000);
    sqlite3* db=nullptr; check(sqlite3_open(path.c_str(),&db)==SQLITE_OK,"open old-schema fixture");
    auto rc=sqlite3_exec(db,"DROP TABLE gmail_draft_matches; DROP TABLE draft_editors; DROP TABLE gmail_drafts; ALTER TABLE drafts DROP COLUMN revision; DROP TRIGGER snooze_updated; DROP TRIGGER snooze_inserted; DROP TABLE snooze_reminders; DROP TABLE draft_images; DROP TABLE remote_moves; ALTER TABLE pending_changes DROP COLUMN error; PRAGMA user_version=7",nullptr,nullptr,nullptr); sqlite3_close(db);
    check(rc==SQLITE_OK,"prepare old snooze schema");
    { Store migrated(path); auto rows=migrated.reminders(future); check(rows.size()==1 && rows[0].message==first,"migration keeps active snoozes without notifying every historical expired snooze"); }
}
}
int main()
{
    char dir[]="/tmp/mail-snooze-XXXXXX"; if(!mkdtemp(dir)) return 2;
    try { clockTests(); storage(std::string(dir)+"/mail.db"); std::filesystem::remove_all(dir); std::cout<<checks<<" snooze assertions passed\n"; return 0; }
    catch(const std::exception& e) { std::cerr<<"FAIL: "<<e.what()<<'\n'; std::filesystem::remove_all(dir); return 1; }
}
