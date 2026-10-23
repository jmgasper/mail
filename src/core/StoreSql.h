#pragma once
#include "mail/Model.h"
#include "mail/Repair.h"
#include <sqlite3.h>
#include <stdexcept>

namespace mail::storage {
inline void check(sqlite3* db, int rc)
{
    if (rc == SQLITE_OK || rc == SQLITE_DONE || rc == SQLITE_ROW) return;
    std::string message = std::string("Mail storage: ") + sqlite3_errmsg(db);
    if ((rc & 0xff) == SQLITE_CORRUPT || (rc & 0xff) == SQLITE_NOTADB) {
        const char* path = db ? sqlite3_db_filename(db, "main") : nullptr;
        if (path && *path) MarkStoreDamaged(path);
        throw StoreDamaged(message + ". Quit Mail and open it again to repair its database.");
    }
    throw std::runtime_error(message);
}
inline void exec(sqlite3* db, const char* sql) { check(db, sqlite3_exec(db, sql, nullptr, nullptr, nullptr)); }
class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db) { check(db, sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr)); }
    ~Statement() { sqlite3_finalize(stmt_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    void bind(int n, const std::string& v) { check(db_, sqlite3_bind_text(stmt_, n, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT)); }
    void bind(int n, Id v) { check(db_, sqlite3_bind_int64(stmt_, n, v)); }
    void blob(int n, const std::string& v) { check(db_, sqlite3_bind_blob(stmt_, n, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT)); }
    bool step() { int rc = sqlite3_step(stmt_); check(db_, rc); return rc == SQLITE_ROW; }
    Id number(int n) { return sqlite3_column_int64(stmt_, n); }
    std::string text(int n) {
        const auto* p = static_cast<const char*>(sqlite3_column_blob(stmt_, n));
        return p ? std::string(p, sqlite3_column_bytes(stmt_, n)) : std::string();
    }
private:
    sqlite3* db_;
    sqlite3_stmt* stmt_ = nullptr;
};
class Transaction {
public:
    explicit Transaction(sqlite3* db) : db_(db) { exec(db, "BEGIN IMMEDIATE"); }
    ~Transaction() { if (!done_) sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    void commit() { exec(db_, "COMMIT"); done_ = true; }
private:
    sqlite3* db_;
    bool done_ = false;
};
}
