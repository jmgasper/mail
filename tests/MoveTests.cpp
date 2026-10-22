#include "mail/Sync.h"
#include "mail/Mime.h"
#include <filesystem>
#include <iostream>
#include <sqlite3.h>
#include <unistd.h>

using namespace mail;
namespace {
int checks=0;
void expect(bool value,const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F action,const char* message)
{
    bool rejected=false; try { action(); } catch (const std::exception&) { rejected=true; } expect(rejected,message);
}
void storeActions(const std::filesystem::path& directory)
{
    auto path=(directory/"review.db").string(); Store store(path);
    Account account; account.name="Review account"; account.email="review@example.test"; account.provider=Provider::Imap;
    account.incoming={"example.test",993,Security::TLS,account.email,""}; account.outgoing=account.incoming; account.id=store.saveAccount(account);
    store.saveRemoteFolders(account.id,{{"INBOX","Inbox","inbox"},{"Archive","Archive","archive"},{"Trash","Trash","trash"}});
    auto message=parseMessage("Message-ID: <review@example.test>\r\nSubject: Review fixture\r\n\r\nOriginal bytes\r\n");
    message.account=account.id; message.folder="inbox"; message.remoteKey=imapRemoteKey({"INBOX","41","1"});
    auto id=store.ingestRemote(message,{"INBOX","41","1"});
    store.move(id,"archive"); auto queued=store.moveReviews().front();
    expect(queued.remote.stage=="queued" && queued.accountName==account.name,"queued reviews include account and state");
    store.recordMoveError(queued.change,"Server lacks UIDPLUS");
    Store second(path);
    expect(second.moveReviews().front().error=="Server lacks UIDPLUS","preflight error persists across connections");
    store.setStarred(id,true); store.tag(id,"Keep",true);
    second.cancelQueuedMove(queued.change);
    expect(store.message(id).folder=="inbox" && store.message(id).starred && store.message(id).tags==std::vector<std::string>{"Keep"},"cancel restores folder without losing other edits");
    RemoteMove plan{queued.change.location,{"Archive","77",""},"6","ready",false};
    expect(!store.beginMove(queued.change,plan),"cancelled preflight cannot start a move");
    store.move(id,"archive"); queued=store.moveReviews().front(); store.move(id,"trash");
    rejects([&]{second.cancelQueuedMove(queued.change);},"stale queued choice cannot cancel a newer edit");
    expect(store.message(id).folder=="trash","newer local folder survives stale action");
    store.move(id,"archive"); queued=store.moveReviews().front();
    expect(store.beginMove(queued.change,plan),"begin claims journal before copying");
    auto active=second.pendingMoves().front();
    expect(active.active && !second.claimMove(active),"second connection cannot claim active move");
    expect(!second.beginMove(queued.change,plan),"second preflight cannot replace active journal");
    rejects([&]{second.cancelQueuedMove(queued.change);},"queued cancel cannot race an active move");
    rejects([&]{second.keepMoveLocal(active);},"active move cannot be detached");
    rejects([&]{second.retryMove(active,true);},"active move cannot be retried");
    plan.stage="started"; store.saveMove(queued.change,plan); store.releaseMove(queued.change,"Lost receipt");
    auto uncertain=second.pendingMoves().front();
    expect(!uncertain.active && uncertain.error=="Lost receipt","failed worker releases claim and persists reason");
    rejects([&]{second.retryMove(uncertain);},"uncertain copy requires explicit confirmation");
    expect(store.claimMove(uncertain),"one worker can claim saved progress");
    rejects([&]{second.keepMoveLocal(uncertain);},"confirmation cannot detach a newly claimed move");
    store.releaseMove(queued.change,"Still uncertain");
    rejects([&]{second.retryMove(uncertain,true);},"confirmation from before another completed check is stale");
    uncertain=second.pendingMoves().front(); second.retryMove(uncertain,true);
    auto retry=store.pendingMoves().front(); expect(retry.remote.stage=="retry" && !retry.active,"explicit retry queues a fresh preflight");
    expect(store.claimMove(retry),"retry can be claimed");
    plan.stage="copied"; plan.destination.locator="8"; plan.firstUid="8";
    store.saveMove(queued.change,plan); store.releaseMove(queued.change,"Source removal interrupted");
    auto copied=second.pendingMoves().front();
    rejects([&]{second.retryMove(copied,true);},"a recorded destination cannot be copied again");
    expect(store.claimMove(copied),"saved destination can be resumed");
    { Store reopened(path); expect(reopened.pendingMoves().front().active,"opening another connection never resets claims"); }
    // Simulate application startup after the owning worker has stopped.
    store.recoverOutgoing(); auto recovered=second.pendingMoves().front();
    expect(!recovered.active && recovered.remote.stage=="copied" && recovered.remote.destination.locator=="8","startup clears the claim but preserves destination receipt");
    rejects([&]{second.keepMoveLocal(copied);},"pre-crash review revision cannot mutate recovered work");
    store.move(id,"trash"); store.setRead(id,true);
    second.keepMoveLocal(recovered);
    auto local=store.message(id);
    expect(local.raw==message.raw && local.folder=="trash" && !local.unread && local.starred && local.tags==std::vector<std::string>{"Keep"},"keep local preserves bytes and newer edits");
    expect(store.pendingMoves().empty() && store.pendingChanges(account.id).empty() && store.moveReviews().empty(),"keep local retires move and pending mutations");
    expect(!store.findRemote(account.id,message.remoteKey),"keep local releases the former server identity");
    store.move(id,"archive"); store.setStarred(id,false);
    expect(store.pendingChanges(account.id).empty(),"later edits to detached copy stay local");
    expect(store.thread(local.thread).size()==1,"detached copy remains visible");
    auto serverId=store.ingestRemote(message,{"INBOX","41","1"});
    expect(serverId!=id && store.message(id).raw==message.raw,"later server copy cannot overwrite detached local content");
    store.move(serverId,"archive"); auto removed=store.moveReviews().front();
    expect(store.beginMove(removed.change,plan),"seed account cleanup journal");
    store.removeAccount(account.id);
    expect(store.pendingMoves().empty() && store.moveReviews().empty(),"account deletion clears all review states");
    rejects([&]{second.keepMoveLocal(removed);},"review of removed account cannot affect another row");
}
void migrateMove(const std::filesystem::path& directory)
{
    auto path=(directory/"schema10.db").string(); Id id=0;
    {
        Store store(path); Account a; a.name="Migration"; a.email="migration@example.test"; a.provider=Provider::Imap;
        a.incoming={"example.test",993,Security::TLS,a.email,""}; a.outgoing=a.incoming; a.id=store.saveAccount(a);
        auto m=parseMessage("Subject: Saved move\r\n\r\nKeep these bytes\r\n"); m.account=a.id; m.folder="inbox"; m.remoteKey=imapRemoteKey({"INBOX","41","1"});
        id=store.ingestRemote(m,{"INBOX","41","1"}); store.tag(id,"Keep",true); store.move(id,"archive");
        auto change=store.pendingChanges(a.id).front();
        expect(store.beginMove(change,{change.location,{"Archive","77","8"},"6","copied",false}),"seed durable schema-10 copy receipt");
    }
    sqlite3* db=nullptr; expect(sqlite3_open(path.c_str(),&db)==SQLITE_OK,"open historical move journal");
    auto result=sqlite3_exec(db,"DROP TABLE gmail_draft_matches; DROP TABLE draft_editors; DROP TABLE gmail_drafts; ALTER TABLE drafts DROP COLUMN revision; ALTER TABLE remote_moves DROP COLUMN active; ALTER TABLE remote_moves DROP COLUMN revision; ALTER TABLE remote_moves DROP COLUMN error; ALTER TABLE pending_changes DROP COLUMN error; PRAGMA user_version=10",nullptr,nullptr,nullptr);
    sqlite3_close(db); expect(result==SQLITE_OK,"prepare schema-10 journal");
    Store store(path); auto move=store.pendingMoves().front();
    expect(!move.active && move.remote.stage=="copied" && move.remote.destination.locator=="8" && move.remote.firstUid=="6","schema migration preserves confirmed destination and recovery baseline");
    expect(store.message(id).tags==std::vector<std::string>{"Keep"} && store.message(id).raw.find("Keep these bytes")!=std::string::npos,"schema migration preserves moved content and tags");
    rejects([&]{store.retryMove(move,true);},"migrated receipt cannot copy again");
    expect(store.claimMove(move),"migrated journal is available for recovery");
    store.acknowledgeMove(move.change,move.remote.destination);
    expect(store.moveReviews().empty() && store.findRemote(move.change.account,imapRemoteKey(move.remote.destination))==id,"migrated move can finish without losing original identity");
}
void check(Store& store,const Account& account,const std::atomic<bool>& cancel,bool success)
{
    try {
        auto report=syncAccount(store,account,"test-password",cancel);
        if (success!=report.errors.empty()) {
            std::cerr<<"Sync report for "<<account.name<<" (expected success="<<success<<"): ";
            for (const auto& error:report.errors) std::cerr<<error<<"; ";
            std::cerr<<'\n';
        }
        expect(success==report.errors.empty(),"unexpected sync report");
    } catch (const NetworkError& error) {
        if (success || error.reason!=NetworkFailure::Connection)
            std::cerr<<"Sync error for "<<account.name<<": "<<error.what()<<" (reason="<<int(error.reason)<<")\n";
        expect(!success && error.reason==NetworkFailure::Connection,"unexpected connection failure");
    }
}
}
int main(int argc,char** argv)
{
    if (argc!=9) return 2;
    char temporary[]="/tmp/mail-moves-XXXXXX";
    if (!mkdtemp(temporary)) return 2;
    const std::filesystem::path directory(temporary);
    try {
        storeActions(directory);
        migrateMove(directory);
        std::atomic<bool> cancel{false};
        for (const std::string scenario:{"fallback","atomic","rev2","copy_disconnect","move_disconnect",
            "store_disconnect","expunge_disconnect","copy_no","copy_no_receipt","move_no_receipt",
            "bad_receipt","conflicting_receipt","no_copy","duplicate","different_bytes","target_reset",
            "source_reset","no_uidplus","newer_edit","checkpoint_crash"}) {
            const bool blocked=scenario=="no_copy" || scenario=="duplicate" || scenario=="different_bytes"
                || scenario=="target_reset" || scenario=="source_reset" || scenario=="no_uidplus";
            const bool firstSuccess=scenario=="fallback" || scenario=="atomic" || scenario=="rev2"
                || scenario=="copy_no_receipt" || scenario=="move_no_receipt";
            auto path=(directory/(scenario+".db")).string();
            Account a; Id id=0;
            {
                Store store(path); a.name=scenario; a.email="user@example.test"; a.provider=Provider::Imap;
                a.incoming={argv[1],std::stoi(argv[3]),Security::TLS,scenario,argv[2]};
                a.outgoing=a.incoming; a.id=store.saveAccount(a);
                check(store,a,cancel,true);
                id=store.findRemote(a.id,imapRemoteKey({"INBOX","41","1"}));
                expect(id!=0,"source downloaded");
                store.tag(id,"Keep",true); store.move(id,"archive");
                if (scenario=="newer_edit" || scenario=="checkpoint_crash") {
                    auto provider=connectMail(a,"test-password",cancel);
                    auto change=store.pendingChanges(a.id).front();
                    auto remote=provider->planMove(change.location,{"Archive","Archive","archive"});
                    expect(store.beginMove(change,remote),"move journal created");
                    bool interrupted=false;
                    try {
                        provider->resumeMove(remote,store.message(id).raw,[&](const RemoteMove& progress) {
                            store.saveMove(change,progress);
                            if (progress.stage=="copied") {
                                if (scenario=="newer_edit") { store.move(id,"trash"); store.setStarred(id,true); }
                                throw std::runtime_error("simulated process exit after durable COPYUID");
                            }
                        });
                    } catch (const std::runtime_error&) { interrupted=true; }
                    expect(interrupted,"copy checkpoint interrupted before source removal");
                    expect(store.pendingMoves(a.id).front().remote.stage=="copied","known copy persisted before interruption");
                } else check(store,a,cancel,firstSuccess);
                if (!firstSuccess && scenario!="no_uidplus") expect(store.pendingMoves(a.id).size()==1,"interrupted operation stays durable");
            }
            // Reopen SQLite and create a new TLS connection, retaining only disk state.
            {
                Store store(path);
                store.recoverOutgoing();
                check(store,a,cancel,!blocked);
                if (scenario=="newer_edit") check(store,a,cancel,true);
                if (blocked) {
                    expect(!store.pendingChanges(a.id).empty(),"unconfirmed move remains pending");
                    expect(store.findRemote(a.id,imapRemoteKey({"INBOX","41","1"}))==id,"unconfirmed move retains source cache identity");
                    check(store,a,cancel,false);
                } else {
                    const bool newer=scenario=="newer_edit";
                    expect(store.pendingMoves(a.id).empty() && store.pendingChanges(a.id).empty(),"completed move clears only acknowledged work");
                    expect(store.findRemote(a.id,imapRemoteKey({newer?"Trash":"Archive",newer?"88":"77",newer?"1":"6"}))==id,
                        "destination keeps original local message ID");
                    expect(store.message(id).folder==(newer?"trash":"archive"),"newer folder choice is preserved");
                    if (newer) expect(store.message(id).starred,"flag edit applies to resumed destination");
                }
                expect(store.message(id).tags==std::vector<std::string>{"Keep"},"local tag survives recovery");
                expect(!store.message(id).raw.empty(),"raw mail remains available");
                store.removeAccount(a.id);
                expect(store.pendingMoves(a.id).empty(),"removing account cleans its move journal");
            }
            std::cout<<"Move scenario passed: "<<scenario<<std::endl;
        }
        for (const std::string scenario:{"retry_missing","retry_duplicate","retry_reset_source","keep_local","queued_cancel"}) {
            auto path=(directory/(scenario+".db")).string(); Store store(path);
            Account a; a.name=scenario; a.email="review@example.test"; a.provider=Provider::Imap;
            a.incoming={argv[1],std::stoi(argv[3]),Security::TLS,scenario,argv[2]}; a.outgoing=a.incoming; a.id=store.saveAccount(a);
            check(store,a,cancel,true); auto id=store.findRemote(a.id,imapRemoteKey({"INBOX","41","1"}));
            expect(id!=0,"review source downloaded"); auto raw=store.message(id).raw;
            store.tag(id,"Keep",true); store.move(id,"archive"); check(store,a,cancel,false); check(store,a,cancel,false);
            Store reopened(path); auto review=reopened.moveReviews().front();
            expect(!review.active && !review.error.empty(),"review preserves failure and clears worker claim");
            if (scenario=="queued_cancel") {
                expect(review.remote.stage=="queued","unsupported move remains cancellable");
                reopened.cancelQueuedMove(review.change); check(store,a,cancel,true);
                expect(store.message(id).folder=="inbox" && store.moveReviews().empty(),"cancel never started a wire move");
            } else if (scenario=="keep_local") {
                store.move(id,"trash"); store.setStarred(id,true); reopened.keepMoveLocal(review); check(store,a,cancel,true);
                expect(store.message(id).folder=="trash" && store.message(id).starred && store.moveReviews().empty(),"keep local preserves newer edits while server sync resumes");
                expect(store.findRemote(a.id,imapRemoteKey({"INBOX","41","1"}))!=id,"server copy is downloaded separately");
            } else {
                rejects([&]{reopened.retryMove(review);},"wire retry needs confirmation");
                reopened.retryMove(review,true);
                check(store,a,cancel,scenario=="retry_missing");
                if (scenario=="retry_duplicate") {
                    auto retry=store.pendingMoves().front();
                    expect(retry.remote.firstUid=="8" && retry.remote.stage=="started","retry recovery baseline excludes previous ambiguous copies");
                    check(store,a,cancel,true);
                }
                if (scenario=="retry_reset_source") {
                    auto retry=store.pendingMoves().front();
                    expect(retry.remote.stage=="retry" && !retry.active,"source epoch mismatch blocks explicitly requested copy");
                    check(store,a,cancel,false); store.keepMoveLocal(store.pendingMoves().front()); check(store,a,cancel,true);
                } else {
                    expect(store.moveReviews().empty(),"explicit copy retry eventually completes");
                    expect(store.findRemote(a.id,imapRemoteKey({"Archive","77",scenario=="retry_duplicate"?"8":"6"}))==id,"retry binds only the new attempt's destination");
                }
            }
            expect(store.message(id).raw==raw && store.message(id).tags==std::vector<std::string>{"Keep"},"review action preserves original content and tags");
            expect(store.outbox().empty(),"folder move review creates no SMTP work");
            std::cout<<"Move review scenario passed: "<<scenario<<std::endl;
        }
        std::filesystem::remove_all(directory);
        std::cout<<checks<<" move recovery checks passed\n"; return 0;
    } catch (const std::exception& error) {
        std::cerr<<"FAIL: "<<error.what()<<" ("<<directory<<")\n";
        return 1;
    }
}
