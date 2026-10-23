#include "mail/Repair.h"
#include "StoreSql.h"

#include <spawn.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

extern char** environ;

namespace mail {
using namespace storage;
namespace fs = std::filesystem;
namespace {
const char* const kSidecars[] = {"", "-wal", "-shm", "-journal"};
std::string marker(const std::string& database) { return database + ".needs-repair"; }
bool exists(const std::string& path) { std::error_code ec; return fs::exists(path, ec); }
std::uintmax_t size(const std::string& path) { std::error_code ec; auto n = fs::file_size(path, ec); return ec ? 0 : n; }
void removeDatabase(const std::string& path)
{
    std::error_code ec;
    for (const char* s : kSidecars) fs::remove(path + s, ec);
    fs::remove(marker(path), ec);
}
// Moves a database with its journals, keeping the names SQLite pairs them by.
void moveDatabase(const std::string& from, const std::string& to)
{
    for (const char* s : kSidecars) {
        std::error_code ec;
        if (exists(from + s)) fs::rename(from + s, to + s, ec);
        if (ec) throw std::runtime_error("Could not move the damaged mail database aside: " + ec.message());
    }
}
std::string asideName(const std::string& database)
{
    char stamp[32]; std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", std::localtime(&now));
    std::string base = database + ".damaged-" + stamp, result = base;
    for (int n = 2; exists(result); ++n) result = base + "-" + std::to_string(n);
    return result;
}
std::string findShell()
{
    for (const char* path : {"/bin/sqlite3", "/boot/system/bin/sqlite3", "/usr/bin/sqlite3"})
        if (access(path, X_OK) == 0) return path;
    return {};
}
int finished(pid_t pid)
{
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) if (errno != EINTR) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
// Pipes the SQL that ".recover" writes for the damaged database into a new
// one. The damaged database is opened read-only, so it stays as it was.
void recoverInto(const std::string& shell, const std::string& source, const std::string& target)
{
    int ends[2];
    if (pipe(ends) != 0) throw std::runtime_error("Could not start the database repair.");
    // An application started without standard streams can be handed 0-2 here.
    for (int& end : ends) if (end <= STDERR_FILENO) { int moved = fcntl(end, F_DUPFD, STDERR_FILENO + 1); close(end); end = moved; }
    auto spawn = [&](pid_t& pid, int end, int stream, std::vector<std::string> args) {
        posix_spawn_file_actions_t actions; posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, end, stream);
        posix_spawn_file_actions_addclose(&actions, ends[0]); posix_spawn_file_actions_addclose(&actions, ends[1]);
        // The importing shell echoes settings; only errors are worth keeping.
        if (stream == STDIN_FILENO) posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
        std::vector<char*> argv;
        for (auto& arg : args) argv.push_back(arg.data());
        argv.push_back(nullptr);
        int rc = end < 0 ? EBADF : posix_spawn(&pid, shell.c_str(), &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions); return rc == 0;
    };
    pid_t reader = -1, writer = -1;
    // -init /dev/null keeps a personal ~/.sqliterc out of the repair.
    bool started = spawn(reader, ends[1], STDOUT_FILENO, {shell, "-init", "/dev/null", "-readonly", source, ".recover"});
    started = started && spawn(writer, ends[0], STDIN_FILENO, {shell, "-init", "/dev/null", target});
    close(ends[0]); close(ends[1]);
    int read = reader > 0 ? finished(reader) : -1;
    // The importing shell's status is not checked: it also counts statements
    // that failed for a damaged row. The checks that follow decide whether
    // the copy is usable, as they do for rows on lost pages.
    if (writer > 0) finished(writer);
    if (!started) throw std::runtime_error("Could not start sqlite3 to repair the mail database.");
    if (read != 0) throw std::runtime_error("sqlite3 could not read the damaged mail database.");
}
bool hasTable(sqlite3* db, const char* name)
{
    Statement s(db, "SELECT 1 FROM sqlite_schema WHERE type='table' AND name=?"); s.bind(1, std::string(name)); return s.step();
}
long long finish(const std::string& target, long long& removed, const std::function<void(const char*)>& say)
{
    sqlite3* raw = nullptr;
    int rc = sqlite3_open_v2(target.c_str(), &raw, SQLITE_OPEN_READWRITE, nullptr);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, &sqlite3_close);
    check(db.get(), rc);
    sqlite3_busy_timeout(db.get(), 5000);
    { Statement version(db.get(), "PRAGMA user_version"); version.step();
      if (version.number(0) < 1 || !hasTable(db.get(), "messages")) throw std::runtime_error("Nothing could be recovered from the damaged mail database."); }
    // Rows SQLite could not place belong to nothing Mail can show.
    exec(db.get(), "PRAGMA foreign_keys=ON; DROP TABLE IF EXISTS lost_and_found;");
    say("Rebuilding the search index…");
    exec(db.get(), "INSERT INTO message_search(message_search) VALUES('rebuild')");
    // Remove rows whose parent was on a lost page, such as the folder state
    // of a lost message, so that it downloads again. Deleting cascades.
    say("Removing incomplete messages…");
    for (int pass = 0; pass < 8; ++pass) {
        std::vector<std::pair<std::string, Id>> orphans;
        { Statement s(db.get(), "PRAGMA foreign_key_check"); while (s.step()) orphans.emplace_back(s.text(0), s.number(1)); }
        if (orphans.empty()) break;
        Transaction transaction(db.get());
        for (const auto& [table, row] : orphans) {
            std::string quoted;
            for (char c : table) { quoted += c; if (c == '"') quoted += '"'; }
            Statement remove(db.get(), ("DELETE FROM \"" + quoted + "\" WHERE rowid=?").c_str());
            remove.bind(1, row); remove.step(); removed += sqlite3_changes(db.get());
        }
        transaction.commit();
    }
    // Without a history cursor, the next Gmail check lists every message.
    if (hasTable(db.get(), "gmail_state")) exec(db.get(), "DELETE FROM gmail_state");
    say("Checking the repaired database…");
    { Statement s(db.get(), "PRAGMA integrity_check"); s.step();
      if (s.text(0) != "ok") throw std::runtime_error("The repaired mail database failed SQLite's integrity check: " + s.text(0)); }
    { Statement s(db.get(), "PRAGMA foreign_key_check"); if (s.step()) throw std::runtime_error("The repaired mail database still has incomplete rows in " + s.text(0) + "."); }
    Statement count(db.get(), "SELECT count(*) FROM messages"); count.step(); return count.number(0);
}
}

void MarkStoreDamaged(const std::string& database) { std::ofstream(marker(database)) << "SQLite reported damage in " << database << '\n'; }
bool StoreNeedsRepair(const std::string& database) { return exists(marker(database)); }

RepairReport RepairStore(const std::string& database, const std::function<void(const std::string&)>& progress, std::string shell)
{
    auto say = [&](const char* step) { if (progress) progress(step); };
    if (shell.empty()) shell = findShell();
    if (shell.empty()) throw std::runtime_error("Repairing the mail database needs the sqlite3 command, which is not installed.");
    if (!exists(database)) throw std::runtime_error("The mail database is missing.");
    // The copy takes about as much space as the damaged database and its log.
    const std::uintmax_t needed = size(database) + size(database + "-wal") + (64u << 20);
    std::error_code ec;
    auto space = fs::space(fs::path(database).parent_path(), ec);
    if (!ec && space.available < needed)
        throw std::runtime_error("Repairing the mail database needs " + std::to_string(needed >> 20) + " MB of free disk space.");
    const std::string target = database + ".recovering";
    removeDatabase(target);
    RepairReport report;
    try {
        say("Reading every undamaged part of the database…");
        recoverInto(shell, database, target);
        report.messages = finish(target, report.removed, say);
    } catch (...) { removeDatabase(target); throw; }
    report.damagedCopy = asideName(database);
    moveDatabase(database, report.damagedCopy);
    fs::rename(target, database, ec);
    if (ec) {
        moveDatabase(report.damagedCopy, database);
        throw std::runtime_error("Could not put the repaired mail database in place: " + ec.message());
    }
    fs::remove(marker(database), ec);
    return report;
}

std::string SetAsideStore(const std::string& database)
{
    auto aside = asideName(database);
    moveDatabase(database, aside);
    std::error_code ec; fs::remove(marker(database), ec);
    return aside;
}
}
