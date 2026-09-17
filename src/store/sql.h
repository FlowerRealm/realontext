#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <sqlite3.h>

#include "result.h"

namespace store {

// A prepared statement. Binds are 1-based, columns 0-based, as in SQLite.
class Stmt {
public:
    static Result<Stmt> prepare(sqlite3* db, std::string_view sql)
    {
        sqlite3_stmt* raw = nullptr;
        if (sqlite3_prepare_v3(db, sql.data(), static_cast<int>(sql.size()), SQLITE_PREPARE_PERSISTENT, &raw,
                               nullptr) != SQLITE_OK)
            return Err{std::string("prepare: ") + sqlite3_errmsg(db) + " in " + std::string(sql)};
        Stmt s;
        s.stmt_.reset(raw);
        return s;
    }

    Stmt& bind(int i, int64_t v) { sqlite3_bind_int64(stmt_.get(), i, v); return *this; }
    Stmt& bind(int i, std::string_view v)
    {
        sqlite3_bind_text(stmt_.get(), i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
        return *this;
    }
    Stmt& bind_blob(int i, const unsigned char* data, size_t n)
    {
        sqlite3_bind_blob(stmt_.get(), i, data, static_cast<int>(n), SQLITE_TRANSIENT);
        return *this;
    }

    // SQLITE_ROW, SQLITE_DONE, or an error code.
    int step() { return sqlite3_step(stmt_.get()); }
    void reset() { sqlite3_reset(stmt_.get()); sqlite3_clear_bindings(stmt_.get()); }

    int64_t int64(int col) const { return sqlite3_column_int64(stmt_.get(), col); }
    double real(int col) const { return sqlite3_column_double(stmt_.get(), col); }
    std::string_view text(int col) const
    {
        auto p = reinterpret_cast<const char*>(sqlite3_column_text(stmt_.get(), col));
        return p ? std::string_view(p, sqlite3_column_bytes(stmt_.get(), col)) : std::string_view();
    }
    std::string_view blob(int col) const
    {
        auto p = static_cast<const char*>(sqlite3_column_blob(stmt_.get(), col));
        return p ? std::string_view(p, sqlite3_column_bytes(stmt_.get(), col)) : std::string_view();
    }
    std::string error() const { return sqlite3_errmsg(sqlite3_db_handle(stmt_.get())); }

private:
    struct Finalize {
        void operator()(sqlite3_stmt* s) const { sqlite3_finalize(s); }
    };
    std::unique_ptr<sqlite3_stmt, Finalize> stmt_;
};

// Rolls back unless commit() ran. Every early return leaves the database untouched.
class Transaction {
public:
    explicit Transaction(sqlite3* db) : db_(db) {}
    ~Transaction()
    {
        if (open_)
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    Status begin()
    {
        if (sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK)
            return Err{std::string("begin: ") + sqlite3_errmsg(db_)};
        open_ = true;
        return ok;
    }
    Status commit()
    {
        if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK)
            return Err{std::string("commit: ") + sqlite3_errmsg(db_)};
        open_ = false;
        return ok;
    }

private:
    sqlite3* db_;
    bool open_ = false;
};

} // namespace store
