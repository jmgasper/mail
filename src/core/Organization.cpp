#include "mail/Store.h"
#include "StoreSql.h"
#include <algorithm>
#include <map>
#include <stdexcept>

namespace mail {
using namespace storage;
namespace {
void require(bool value,const char* text) { if (!value) throw std::invalid_argument(text); }
void name(const std::string& value)
{
    require(!value.empty() && value.size()<=256 && value.find_first_not_of(' ')!=std::string::npos
        && std::none_of(value.begin(),value.end(),[](unsigned char c){return c<32 || c==127;}),"Enter a name between 1 and 256 bytes without control characters.");
}
Provider accountProvider(sqlite3* db,Id account)
{
    Statement s(db,"SELECT provider FROM accounts WHERE id=?"); s.bind(1,account);
    require(s.step(),"This account no longer exists."); return static_cast<Provider>(s.number(0));
}
std::string labelId(sqlite3* db,Id account,const std::string& value)
{
    Statement s(db,"SELECT label_id FROM gmail_labels WHERE account_id=? AND name=? AND system=0"); s.bind(1,account); s.bind(2,value);
    return s.step()?s.text(0):"";
}
void queueLabel(sqlite3* db,Id account,const std::string& label,const std::string& kind,const std::string& value)
{
    Statement remove(db,"DELETE FROM label_changes WHERE account_id=? AND label_id=?"); remove.bind(1,account); remove.bind(2,label); remove.step();
    Statement add(db,"INSERT INTO label_changes(account_id,label_id,kind,name) VALUES(?,?,?,?)");
    add.bind(1,account); add.bind(2,label); add.bind(3,kind); add.bind(4,value); add.step();
}
bool hasTag(sqlite3* db,Id account,const std::string& value)
{
    Statement s(db,"SELECT 1 FROM effective_tags t JOIN messages m ON m.id=t.message_id WHERE m.account_id=? AND t.name=? LIMIT 1");
    s.bind(1,account); s.bind(2,value); return s.step();
}
}
void Store::removeSmartFolder(Id id)
{
    std::lock_guard<std::mutex> guard(mutex_); Statement s(db_,"DELETE FROM smart_folders WHERE id=?"); s.bind(1,id); s.step();
}
void Store::removeFilter(Id id)
{
    std::lock_guard<std::mutex> guard(mutex_); Statement s(db_,"DELETE FROM filters WHERE id=?"); s.bind(1,id); s.step();
}
void Store::moveFilter(Id id,int direction)
{
    require(direction==-1 || direction==1,"Choose a rule direction.");
    std::lock_guard<std::mutex> guard(mutex_); Transaction transaction(db_);
    std::vector<Id> ids; Statement s(db_,"SELECT id FROM filters ORDER BY position,id"); while(s.step()) ids.push_back(s.number(0));
    auto found=std::find(ids.begin(),ids.end(),id); require(found!=ids.end(),"This rule no longer exists.");
    auto index=found-ids.begin(); auto target=index+direction;
    if (target>=0 && target<static_cast<decltype(index)>(ids.size())) std::swap(ids[index],ids[target]);
    for (std::size_t i=0;i<ids.size();++i) { Statement update(db_,"UPDATE filters SET position=? WHERE id=?"); update.bind(1,i+1); update.bind(2,ids[i]); update.step(); }
    transaction.commit();
}
std::vector<TagInfo> Store::accountTags(Id account)
{
    std::lock_guard<std::mutex> guard(mutex_); accountProvider(db_,account);
    std::map<std::string,TagInfo> tags;
    Statement local(db_,"SELECT t.name,COUNT(*) FROM effective_tags t JOIN messages m ON m.id=t.message_id WHERE m.account_id=? GROUP BY t.name"); local.bind(1,account);
    while(local.step()) tags[local.text(0)]={local.text(0),{},local.number(1),{}, {}};
    Statement labels(db_,"SELECT label_id,name FROM gmail_labels WHERE account_id=? AND system=0"); labels.bind(1,account);
    while(labels.step()) { auto& tag=tags[labels.text(1)]; tag.name=labels.text(1); tag.label=labels.text(0); }
    Statement pending(db_,"SELECT label_id,kind,name,error FROM label_changes WHERE account_id=? ORDER BY id"); pending.bind(1,account);
    while(pending.step()) { auto& tag=tags[pending.text(2)]; tag.name=pending.text(2); tag.label=pending.text(0); tag.pending=pending.text(1); tag.error=pending.text(3); }
    std::vector<TagInfo> result; for (auto& pair:tags) result.push_back(std::move(pair.second)); return result;
}
void Store::renameTag(Id account,const std::string& oldName,const std::string& newName)
{
    name(oldName); name(newName); std::lock_guard<std::mutex> guard(mutex_); Transaction transaction(db_);
    auto provider=accountProvider(db_,account); auto label=labelId(db_,account,oldName);
    require(!label.empty() || hasTag(db_,account,oldName),"This tag was removed or renamed. Refresh the list.");
    if (oldName==newName) { transaction.commit(); return; }
    require(labelId(db_,account,newName).empty() && !hasTag(db_,account,newName),"That tag already exists in this account. Choose another name.");
    Statement deleting(db_,"SELECT 1 FROM label_changes WHERE account_id=? AND kind='delete' AND name IN(?,?)");
    deleting.bind(1,account); deleting.bind(2,oldName); deleting.bind(3,newName); require(!deleting.step(),"Wait for the pending tag removal to finish.");
    Statement local(db_,"UPDATE tags SET name=? WHERE name=? AND message_id IN(SELECT id FROM messages WHERE account_id=?)");
    local.bind(1,newName); local.bind(2,oldName); local.bind(3,account); local.step();
    // Fresh operation IDs keep a reply to an older membership request from
    // acknowledging an edit made while the rename was in flight.
    Statement pending(db_,"SELECT p.message_id,p.value FROM pending_changes p JOIN messages m ON m.id=p.message_id WHERE m.account_id=? AND p.kind=?");
    pending.bind(1,account); pending.bind(2,"tag:"+oldName); std::vector<std::pair<Id,std::string>> edits;
    while(pending.step()) edits.emplace_back(pending.number(0),pending.text(1));
    Statement clear(db_,"DELETE FROM pending_changes WHERE kind=? AND message_id IN(SELECT id FROM messages WHERE account_id=?)");
    clear.bind(1,"tag:"+oldName); clear.bind(2,account); clear.step();
    for (const auto& edit:edits) queueChange(edit.first,"tag:"+newName,edit.second);
    for (const char* sql:{"UPDATE filters SET add_tag=? WHERE account_id=? AND add_tag=?", "UPDATE smart_folders SET tag=? WHERE account_id=? AND tag=?"}) {
        Statement update(db_,sql); update.bind(1,newName); update.bind(2,account); update.bind(3,oldName); update.step();
    }
    if (provider==Provider::Gmail && !label.empty()) { queueLabel(db_,account,label,"rename",newName); overlayLabelChanges(account); }
    transaction.commit();
}
void Store::removeTag(Id account,const std::string& tag)
{
    name(tag); std::lock_guard<std::mutex> guard(mutex_); Transaction transaction(db_); auto provider=accountProvider(db_,account);
    auto label=labelId(db_,account,tag);
    require(!label.empty() || hasTag(db_,account,tag),"This tag was removed or renamed. Refresh the list.");
    Statement local(db_,"DELETE FROM tags WHERE name=? AND message_id IN(SELECT id FROM messages WHERE account_id=?)"); local.bind(1,tag); local.bind(2,account); local.step();
    Statement pending(db_,"DELETE FROM pending_changes WHERE message_id IN(SELECT id FROM messages WHERE account_id=?) AND (kind=? OR (kind='move' AND value=?))");
    pending.bind(1,account); pending.bind(2,"tag:"+tag); pending.bind(3,label.empty()?"":"remote:"+label); pending.step();
    Statement rules(db_,"UPDATE filters SET add_tag='',enabled=CASE WHEN move_to='' AND mark_read=0 AND star=0 AND stop=0 THEN 0 ELSE enabled END WHERE account_id=? AND add_tag=?");
    rules.bind(1,account); rules.bind(2,tag); rules.step();
    // Retain smart-folder criteria: clearing its tag would unexpectedly widen
    // a saved view to every message. It remains editable with no matches.
    if (provider==Provider::Gmail && !label.empty()) { queueLabel(db_,account,label,"delete",tag); overlayLabelChanges(account); }
    transaction.commit();
}
void Store::overlayLabelChanges(Id account)
{
    Statement changes(db_,"SELECT label_id,kind,name FROM label_changes WHERE account_id=? ORDER BY id"); changes.bind(1,account);
    while(changes.step()) {
        const auto label=changes.text(0),kind=changes.text(1),value=changes.text(2);
        for (const char* sql:{"DELETE FROM gmail_labels WHERE account_id=? AND label_id=?","DELETE FROM remote_folders WHERE account_id=? AND remote_id=?"}) {
            Statement clear(db_,sql); clear.bind(1,account); clear.bind(2,label); clear.step();
        }
        if (kind=="rename") {
            Statement add(db_,"INSERT INTO gmail_labels VALUES(?,?,?,0)"); add.bind(1,account); add.bind(2,label); add.bind(3,value); add.step();
            Statement folder(db_,"INSERT INTO remote_folders VALUES(?,?,?,'')"); folder.bind(1,account); folder.bind(2,label); folder.bind(3,value); folder.step();
        } else {
            Statement remove(db_,"DELETE FROM gmail_message_labels WHERE label_id=? AND message_id IN(SELECT id FROM messages WHERE account_id=?)"); remove.bind(1,label); remove.bind(2,account); remove.step();
        }
    }
}
std::vector<LabelChange> Store::labelChanges(Id account)
{
    std::lock_guard<std::mutex> guard(mutex_); Statement s(db_,"SELECT id,account_id,label_id,kind,name,error FROM label_changes WHERE account_id=? ORDER BY id"); s.bind(1,account);
    std::vector<LabelChange> result; while(s.step()) result.push_back({s.number(0),s.number(1),s.text(2),s.text(3),s.text(4),s.text(5)}); return result;
}
void Store::finishLabelChange(Id id,const std::string& error)
{
    std::lock_guard<std::mutex> guard(mutex_);
    if (error.empty()) { Statement s(db_,"DELETE FROM label_changes WHERE id=?"); s.bind(1,id); s.step(); }
    else { Statement s(db_,"UPDATE label_changes SET error=? WHERE id=?"); s.bind(1,error); s.bind(2,id); s.step(); }
}
}
