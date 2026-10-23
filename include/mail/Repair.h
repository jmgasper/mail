#pragma once
#include <functional>
#include <stdexcept>
#include <string>

namespace mail {
// SQLite found damage in the database file itself. The damage is recorded
// beside the database so that Mail repairs it the next time it starts.
struct StoreDamaged : std::runtime_error { using std::runtime_error::runtime_error; };
struct RepairReport {
    std::string damagedCopy; // The damaged database, kept under this name.
    long long messages=0;    // Messages in the repaired database.
    long long removed=0;     // Rows dropped because what they belong to was lost.
};
void MarkStoreDamaged(const std::string& database);
bool StoreNeedsRepair(const std::string& database);
// Rebuilds the database from every readable page with the sqlite3 shell's
// .recover, checks the result, and only then replaces the damaged files.
// Lost messages download again: Gmail history restarts with a full check
// and IMAP compares every folder anyway. Throws if the copy is unusable,
// leaving the damaged database untouched.
RepairReport RepairStore(const std::string& database,const std::function<void(const std::string&)>& progress={},std::string shell={});
// Last resort when a repair fails: moves the damaged files aside so that
// Mail starts with an empty database. Returns where they went.
std::string SetAsideStore(const std::string& database);
}
