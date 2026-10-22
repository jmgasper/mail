#include "mail/Store.h"
#include "StoreSql.h"
#include <set>
#include <stdexcept>

namespace mail {
using namespace storage;
void Store::snooze(const std::vector<Id>& ids,Timestamp until)
{
    if(until<0 || until>253402300799LL) throw std::invalid_argument("Choose a valid snooze time.");
    std::lock_guard<std::mutex> guard(mutex_); Transaction transaction(db_);
    for(Id id:std::set<Id>(ids.begin(),ids.end())) {
        Statement exists(db_,"SELECT 1 FROM messages WHERE id=?"); exists.bind(1,id);
        if(!exists.step()) throw std::invalid_argument("A selected message no longer exists. Select the conversation again.");
        Statement update(db_,"UPDATE messages SET snoozed_until=? WHERE id=? AND (?>0 AND folder NOT IN('trash','junk') OR ?=0)");
        update.bind(1,until); update.bind(2,id); update.bind(3,until); update.bind(4,until); update.step();
    }
    transaction.commit();
}
std::vector<SnoozeReminder> Store::reminders(Timestamp now)
{
    std::lock_guard<std::mutex> guard(mutex_);
    Statement s(db_,R"SQL(SELECT s.id,m.id,m.thread_id,m.account_id,s.due,m.subject,a.name,s.notified
FROM snooze_reminders s JOIN messages m ON m.id=s.message_id JOIN accounts a ON a.id=m.account_id
WHERE s.due<=? AND m.folder NOT IN('trash','junk')
AND NOT EXISTS(SELECT 1 FROM remote_messages r WHERE r.message_id=m.id AND r.present=0)
ORDER BY s.due,s.id)SQL"); s.bind(1,now);
    std::vector<SnoozeReminder> result;
    while(s.step()) result.push_back({s.number(0),s.number(1),s.number(2),s.number(3),s.number(4),s.text(5),s.text(6),s.number(7)!=0});
    return result;
}
void Store::markRemindersNotified(const std::vector<Id>& ids)
{
    std::lock_guard<std::mutex> guard(mutex_); Transaction transaction(db_);
    for(Id id:ids) { Statement s(db_,"UPDATE snooze_reminders SET notified=1 WHERE id=?"); s.bind(1,id); s.step(); }
    transaction.commit();
}
void Store::dismissReminders(const std::vector<Id>& ids)
{
    std::lock_guard<std::mutex> guard(mutex_); Transaction transaction(db_);
    for(Id id:ids) { Statement s(db_,"DELETE FROM snooze_reminders WHERE id=?"); s.bind(1,id); s.step(); }
    transaction.commit();
}
}
