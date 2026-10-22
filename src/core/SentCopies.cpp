#include "mail/Store.h"
#include "StoreSql.h"
#include <stdexcept>

namespace mail {
using namespace storage;
namespace {
bool active(const std::string& state) { return state=="checking" || state=="uploading"; }
void require(bool condition,const char* text) { if (!condition) throw std::invalid_argument(text); }
}
SentCopy Store::readSentCopy(Id id)
{
    Statement s(db_,"SELECT c.message_id,m.account_id,c.state,c.error,m.subject,m.message_id,c.destination,c.may_append,c.confirmed FROM sent_copies c JOIN messages m ON m.id=c.message_id WHERE c.message_id=?");
    s.bind(1,id); if (!s.step()) return {};
    return {s.number(0),s.number(1),s.text(2),s.text(3),s.text(4),s.text(5),s.text(6),s.number(7)!=0,s.number(8)!=0};
}
std::vector<SentCopy> Store::sentCopies(Id account)
{
    std::lock_guard<std::mutex> guard(mutex_);
    Statement s(db_,"SELECT c.message_id FROM sent_copies c JOIN messages m ON m.id=c.message_id WHERE (?=0 OR m.account_id=?) AND c.state NOT IN('complete','local') ORDER BY c.message_id");
    s.bind(1,account); s.bind(2,account); std::vector<SentCopy> copies;
    while (s.step()) copies.push_back(readSentCopy(s.number(0)));
    return copies;
}
SentCopy Store::claimSentCopy(Id id)
{
    std::lock_guard<std::mutex> guard(mutex_); Transaction transaction(db_);
    auto copy=readSentCopy(id);
    if (!copy.id || active(copy.state) || copy.state=="complete" || copy.state=="local") { transaction.commit(); return {}; }
    Statement update(db_,"UPDATE sent_copies SET state='checking',error='' WHERE message_id=?"); update.bind(1,id); update.step();
    transaction.commit(); return copy;
}
void Store::beginSentUpload(Id id,const std::string& destination)
{
    std::lock_guard<std::mutex> guard(mutex_);
    Statement s(db_,"UPDATE sent_copies SET state='uploading',may_append=0,destination=? WHERE message_id=? AND state='checking' AND may_append=1 AND confirmed=0");
    s.bind(1,destination); s.bind(2,id); s.step();
    require(sqlite3_changes(db_)==1,"This Sent copy is already being saved or needs review.");
}
void Store::releaseSentCopy(Id id,const std::string& error,bool mayAppend,bool confirmed)
{
    std::lock_guard<std::mutex> guard(mutex_);
    Statement s(db_,"UPDATE sent_copies SET state=?1,error=?2,may_append=?3,confirmed=?4,destination=CASE WHEN ?3 THEN '' ELSE destination END WHERE message_id=?5 AND state IN('checking','uploading')");
    s.bind(1,confirmed?"saved":(!mayAppend?"uncertain":(error.empty()?"pending":"failed")));
    s.bind(2,error); s.bind(3,mayAppend && !confirmed); s.bind(4,confirmed); s.bind(5,id); s.step();
}
void Store::bindSentCopyLocked(Id id,const RemoteLocation& location)
{
    auto copy=readSentCopy(id); require(copy.id!=0,"This Sent copy no longer exists.");
    if (copy.state=="complete" || copy.state=="local") return;
    const auto key=imapRemoteKey(location);
    Statement existing(db_,"SELECT id,message_id FROM messages WHERE account_id=? AND remote_key=?");
    existing.bind(1,copy.account); existing.bind(2,key);
    if (existing.step() && existing.number(0)!=id) {
        require(existing.text(1)==copy.messageId,"The server Sent identifier belongs to a different message.");
        const auto duplicate=existing.number(0);
        // Preserve the stable local ID and user organization if a receive worker
        // cached this very server copy before its delivery was committed.
        Statement tags(db_,"INSERT OR IGNORE INTO tags SELECT ?,name FROM tags WHERE message_id=?");
        tags.bind(1,id); tags.bind(2,duplicate); tags.step();
        Statement remove(db_,"DELETE FROM messages WHERE id=?"); remove.bind(1,duplicate); remove.step();
    }
    Statement update(db_,"UPDATE messages SET remote_key=? WHERE id=?"); update.bind(1,key); update.bind(2,id); update.step();
    Statement remote(db_,"INSERT INTO remote_messages VALUES(?,?,?,?,1) ON CONFLICT(message_id) DO UPDATE SET folder=excluded.folder,epoch=excluded.epoch,locator=excluded.locator,present=1");
    remote.bind(1,id); remote.bind(2,location.folder); remote.bind(3,location.epoch); remote.bind(4,location.locator); remote.step();
    // The local Sent message starts read; retain explicit edits made while its
    // server identity was unknown, and send them to the adopted copy.
    auto message=readMessage(id);
    queueChange(id,"read",message.unread?"0":"1"); queueChange(id,"star",message.starred?"1":"0");
    if (message.folder!="sent") queueChange(id,"move",message.folder);
    Statement done(db_,"UPDATE sent_copies SET state='complete',error='',destination=?,may_append=0,confirmed=1 WHERE message_id=?");
    done.bind(1,location.folder); done.bind(2,id); done.step();
}
void Store::bindSentCopy(Id id,const RemoteLocation& location)
{
    std::lock_guard<std::mutex> guard(mutex_); Transaction transaction(db_);
    bindSentCopyLocked(id,location); transaction.commit();
}
void Store::retrySentCopy(Id id,bool acceptUncertain)
{
    std::lock_guard<std::mutex> guard(mutex_); Transaction transaction(db_); auto copy=readSentCopy(id);
    require(copy.id && !active(copy.state) && copy.state!="complete" && copy.state!="local" && !copy.confirmed,
        "Wait for the Sent copy check to finish. A confirmed copy only needs a server check.");
    require(copy.mayAppend || acceptUncertain,"The server may already have this Sent copy. Confirm before saving another copy.");
    Statement s(db_,"UPDATE sent_copies SET state='pending',error='',may_append=1,destination='' WHERE message_id=?"); s.bind(1,id); s.step(); transaction.commit();
}
void Store::keepSentCopyLocal(Id id)
{
    std::lock_guard<std::mutex> guard(mutex_); Transaction transaction(db_); auto copy=readSentCopy(id);
    require(copy.id && !active(copy.state) && copy.state!="complete","Wait for the Sent copy check to finish.");
    Statement s(db_,"UPDATE sent_copies SET state='local',error='' WHERE message_id=?"); s.bind(1,id); s.step();
    Statement clear(db_,"DELETE FROM pending_changes WHERE message_id=? AND NOT EXISTS(SELECT 1 FROM remote_messages WHERE message_id=?)");
    clear.bind(1,id); clear.bind(2,id); clear.step(); transaction.commit();
}
}
