#include "mail/Store.h"
#include "StoreSql.h"
#include <algorithm>

namespace mail {
using namespace storage;
namespace {
std::vector<PendingMove> readMoves(sqlite3* db,Id account)
{
    Statement s(db,R"SQL(SELECT r.message_id,r.change_id,r.value,r.source_folder,r.source_epoch,r.source_uid,
 r.target_folder,r.target_epoch,r.target_uid,r.first_uid,r.stage,r.atomic_move,
 r.active,r.revision,r.error,m.account_id,m.subject,a.name
 FROM remote_moves r JOIN messages m ON m.id=r.message_id JOIN accounts a ON a.id=m.account_id
 WHERE (?1=0 OR m.account_id=?1) ORDER BY r.change_id)SQL");
    s.bind(1,account); std::vector<PendingMove> result;
    while (s.step()) {
        PendingMove move;
        move.remote={{s.text(3),s.text(4),s.text(5)},{s.text(6),s.text(7),s.text(8)},s.text(9),s.text(10),s.number(11)!=0};
        move.change={s.number(1),s.number(0),s.number(15),"move",s.text(2),move.remote.source};
        move.active=s.number(12)!=0; move.revision=s.number(13); move.error=s.text(14);
        move.subject=s.text(16); move.accountName=s.text(17); result.push_back(std::move(move));
    }
    return result;
}
void requireReview(sqlite3* db,const PendingMove& move)
{
    Statement s(db,"SELECT 1 FROM remote_moves WHERE message_id=? AND change_id=? AND revision=? AND active=0");
    s.bind(1,move.change.message); s.bind(2,move.change.id); s.bind(3,move.revision);
    if (!s.step()) throw std::runtime_error("This folder move changed or is being checked. Review its current status before trying again.");
}
}

bool Store::beginMove(const PendingChange& change,const RemoteMove& remote)
{
    std::lock_guard<std::mutex> guard(mutex_);
    Transaction transaction(db_);
    Statement current(db_,R"SQL(SELECT 1 FROM pending_changes p JOIN remote_messages r ON r.message_id=p.message_id
 WHERE p.id=? AND p.message_id=? AND p.kind='move' AND r.folder=? AND r.epoch=? AND r.locator=?
 AND NOT EXISTS(SELECT 1 FROM remote_moves WHERE message_id=p.message_id))SQL");
    current.bind(1,change.id); current.bind(2,change.message); current.bind(3,remote.source.folder);
    current.bind(4,remote.source.epoch); current.bind(5,remote.source.locator);
    if (!current.step()) return false; // The plan was cancelled, superseded or claimed.
    Statement s(db_,R"SQL(INSERT INTO remote_moves(message_id,change_id,value,source_folder,source_epoch,source_uid,
 target_folder,target_epoch,target_uid,first_uid,stage,atomic_move,active,revision) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,1,1))SQL");
    s.bind(1,change.message); s.bind(2,change.id); s.bind(3,change.value);
    s.bind(4,remote.source.folder); s.bind(5,remote.source.epoch); s.bind(6,remote.source.locator);
    s.bind(7,remote.destination.folder); s.bind(8,remote.destination.epoch); s.bind(9,remote.destination.locator);
    s.bind(10,remote.firstUid); s.bind(11,remote.stage); s.bind(12,remote.atomic); s.step();
    transaction.commit(); return true;
}

std::vector<PendingMove> Store::pendingMoves(Id account)
{
    std::lock_guard<std::mutex> guard(mutex_); return readMoves(db_,account);
}

std::vector<PendingMove> Store::moveReviews()
{
    std::lock_guard<std::mutex> guard(mutex_);
    auto result=readMoves(db_,0);
    Statement s(db_,R"SQL(SELECT p.id,p.message_id,m.account_id,p.value,r.folder,r.epoch,r.locator,p.error,m.subject,a.name
 FROM pending_changes p JOIN messages m ON m.id=p.message_id JOIN accounts a ON a.id=m.account_id
 JOIN remote_messages r ON r.message_id=m.id WHERE p.kind='move' AND a.provider IN(1,4)
 AND NOT EXISTS(SELECT 1 FROM remote_moves WHERE message_id=m.id))SQL");
    while (s.step()) {
        PendingMove move;
        move.change={s.number(0),s.number(1),s.number(2),"move",s.text(3),{s.text(4),s.text(5),s.text(6)}};
        move.remote.source=move.change.location; move.remote.stage="queued";
        move.error=s.text(7); move.subject=s.text(8); move.accountName=s.text(9); result.push_back(std::move(move));
    }
    std::sort(result.begin(),result.end(),[](const PendingMove& a,const PendingMove& b){return a.change.id<b.change.id;});
    return result;
}

bool Store::claimMove(const PendingMove& move)
{
    std::lock_guard<std::mutex> guard(mutex_);
    Statement s(db_,"UPDATE remote_moves SET active=1,revision=revision+1 WHERE message_id=? AND change_id=? AND revision=? AND active=0");
    s.bind(1,move.change.message); s.bind(2,move.change.id); s.bind(3,move.revision); s.step();
    return sqlite3_changes(db_)==1;
}

void Store::saveMove(const PendingChange& change,const RemoteMove& remote)
{
    std::lock_guard<std::mutex> guard(mutex_);
    Statement s(db_,R"SQL(UPDATE remote_moves SET stage=?,target_epoch=?,target_uid=?,first_uid=?,atomic_move=?,revision=revision+1
 WHERE message_id=? AND change_id=? AND active=1 AND source_folder=? AND source_epoch=? AND source_uid=? AND target_folder=?)SQL");
    s.bind(1,remote.stage); s.bind(2,remote.destination.epoch); s.bind(3,remote.destination.locator);
    s.bind(4,remote.firstUid); s.bind(5,remote.atomic); s.bind(6,change.message); s.bind(7,change.id);
    s.bind(8,remote.source.folder); s.bind(9,remote.source.epoch); s.bind(10,remote.source.locator); s.bind(11,remote.destination.folder); s.step();
    if (sqlite3_changes(db_)!=1) throw std::runtime_error("The pending folder move changed or is no longer being checked.");
}

void Store::releaseMove(const PendingChange& change,const std::string& error)
{
    std::lock_guard<std::mutex> guard(mutex_);
    Statement s(db_,"UPDATE remote_moves SET active=0,error=?,revision=revision+1 WHERE message_id=? AND change_id=? AND active=1");
    s.bind(1,error); s.bind(2,change.message); s.bind(3,change.id); s.step();
}

void Store::recordMoveError(const PendingChange& change,const std::string& error)
{
    std::lock_guard<std::mutex> guard(mutex_);
    Statement s(db_,"UPDATE pending_changes SET error=? WHERE id=? AND message_id=? AND kind='move'");
    s.bind(1,error); s.bind(2,change.id); s.bind(3,change.message); s.step();
}

void Store::retryMove(const PendingMove& move,bool acceptUncertain)
{
    if (!acceptUncertain) throw std::runtime_error("Check the destination folder before retrying. Another copy may already exist.");
    std::lock_guard<std::mutex> guard(mutex_);
    Transaction transaction(db_); requireReview(db_,move);
    Statement s(db_,R"SQL(UPDATE remote_moves SET stage='retry',error='',revision=revision+1
 WHERE message_id=? AND change_id=? AND stage='started' AND target_uid='')SQL");
    s.bind(1,move.change.message); s.bind(2,move.change.id); s.step();
    if (sqlite3_changes(db_)!=1) throw std::runtime_error("This move does not need another copy. Check the server to finish the saved progress.");
    transaction.commit();
}

void Store::keepMoveLocal(const PendingMove& move)
{
    std::lock_guard<std::mutex> guard(mutex_);
    Transaction transaction(db_); requireReview(db_,move);
    // Keep the user's current folder, flags, tags and original bytes. Detach the
    // identity so subsequent snapshots can represent the server independently.
    Statement key(db_,"UPDATE messages SET remote_key=? WHERE id=?");
    key.bind(1,"local-move:"+std::to_string(move.change.message)); key.bind(2,move.change.message); key.step();
    for (const char* sql:{"DELETE FROM pending_changes WHERE message_id=?","DELETE FROM remote_moves WHERE message_id=?","DELETE FROM remote_messages WHERE message_id=?"}) {
        Statement s(db_,sql); s.bind(1,move.change.message); s.step();
    }
    transaction.commit();
}

void Store::cancelQueuedMove(const PendingChange& change)
{
    std::lock_guard<std::mutex> guard(mutex_);
    Transaction transaction(db_);
    Statement current(db_,R"SQL(SELECT r.folder,COALESCE(f.role,'') FROM pending_changes p
 JOIN messages m ON m.id=p.message_id JOIN remote_messages r ON r.message_id=m.id
 LEFT JOIN remote_folders f ON f.account_id=m.account_id AND f.remote_id=r.folder
 WHERE p.id=? AND p.message_id=? AND p.kind='move' AND NOT EXISTS(SELECT 1 FROM remote_moves WHERE message_id=p.message_id))SQL");
    current.bind(1,change.id); current.bind(2,change.message);
    if (!current.step()) throw std::runtime_error("This folder move changed or has already started. Review its current status before trying again.");
    auto folder=current.text(1).empty()?"remote:"+current.text(0):current.text(1);
    Statement restore(db_,"UPDATE messages SET folder=? WHERE id=?");
    restore.bind(1,folder); restore.bind(2,change.message); restore.step();
    Statement done(db_,"DELETE FROM pending_changes WHERE id=?"); done.bind(1,change.id); done.step();
    transaction.commit();
}
}
