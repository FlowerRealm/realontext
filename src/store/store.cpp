#include "store/store.h"

#include <cstring>

#include <blake3.h>

#include "parse/lang.h"

namespace store {

namespace {

constexpr const char* schema = R"sql(
PRAGMA journal_mode = WAL;
PRAGMA synchronous = NORMAL;

CREATE TABLE IF NOT EXISTS chunks (
    ord         INTEGER PRIMARY KEY,
    hash        BLOB NOT NULL UNIQUE,
    kind        INTEGER NOT NULL,
    symbol_path TEXT NOT NULL,
    content     TEXT NOT NULL,
    embedding   BLOB
);

-- (blob, lang) pairs already sent through parse/. A blob that yields no chunks
-- is still recorded here, or every branch would parse it again.
CREATE TABLE IF NOT EXISTS parsed (
    blob BLOB NOT NULL,
    lang INTEGER NOT NULL,
    PRIMARY KEY (blob, lang)
) WITHOUT ROWID;

CREATE TABLE IF NOT EXISTS blob_chunks (
    blob       BLOB NOT NULL,
    lang       INTEGER NOT NULL,
    chunk      INTEGER NOT NULL,
    start_line INTEGER NOT NULL,
    end_line   INTEGER NOT NULL,
    PRIMARY KEY (blob, lang, chunk, start_line)
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS blob_chunks_by_chunk ON blob_chunks (chunk);

CREATE TABLE IF NOT EXISTS files (
    ord  INTEGER PRIMARY KEY,
    path TEXT NOT NULL,
    blob BLOB NOT NULL,
    UNIQUE (path, blob)
);
CREATE INDEX IF NOT EXISTS files_by_blob ON files (blob);

CREATE TABLE IF NOT EXISTS branches (
    name       TEXT PRIMARY KEY,
    commit_sha BLOB NOT NULL,
    files      BLOB NOT NULL
);

CREATE TABLE IF NOT EXISTS meta (
    key   TEXT PRIMARY KEY,
    value
);
)sql";

std::array<unsigned char, BLAKE3_OUT_LEN> chunk_hash(parse::Lang lang, const parse::Chunk& c)
{
    blake3_hasher h;
    blake3_hasher_init(&h);
    const unsigned char tag[2] = {static_cast<unsigned char>(lang), 0};
    blake3_hasher_update(&h, tag, sizeof tag);
    blake3_hasher_update(&h, c.symbol.data(), c.symbol.size() + 1); // includes the NUL separator
    blake3_hasher_update(&h, c.content.data(), c.content.size());
    std::array<unsigned char, BLAKE3_OUT_LEN> out;
    blake3_hasher_finalize(&h, out.data(), out.size());
    return out;
}

Err sql_error(sqlite3* db, const std::string& what) { return Err{what + ": " + sqlite3_errmsg(db)}; }

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
            return sql_error(db_, "begin");
        open_ = true;
        return ok;
    }
    Status commit()
    {
        if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK)
            return sql_error(db_, "commit");
        open_ = false;
        return ok;
    }

private:
    sqlite3* db_;
    bool open_ = false;
};

// Find a row by key, inserting it when absent. Returns (rowid, inserted).
// Both statements get the same binds; SQLite ignores parameters a statement lacks.
struct Upsert {
    Stmt find, insert;

    Result<std::pair<int64_t, bool>> run(const std::function<void(Stmt&)>& bind)
    {
        find.reset();
        bind(find);
        int rc = find.step();
        if (rc == SQLITE_ROW) {
            int64_t id = find.int64(0);
            find.reset(); // an unfinished statement blocks COMMIT
            return std::pair{id, false};
        }
        if (rc != SQLITE_DONE)
            return Err{"lookup: " + find.error()};
        insert.reset();
        bind(insert);
        if (insert.step() != SQLITE_ROW)
            return Err{"insert: " + insert.error()};
        int64_t id = insert.int64(0);
        insert.reset();
        return std::pair{id, true};
    }
};

Result<Upsert> upsert(sqlite3* db, const char* find, const char* insert)
{
    auto f = Stmt::prepare(db, find);
    if (!f)
        return Err{f.error()};
    auto i = Stmt::prepare(db, insert);
    if (!i)
        return Err{i.error()};
    return Upsert{std::move(*f), std::move(*i)};
}

} // namespace

void Db::Close::operator()(sqlite3* db) const { sqlite3_close_v2(db); }

Result<Db> Db::open(const std::string& path)
{
    sqlite3* raw = nullptr;
    int rc = sqlite3_open_v2(path.c_str(), &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    Db db;
    db.db_.reset(raw);
    if (rc != SQLITE_OK)
        return sql_error(raw, "open " + path);
    sqlite3_busy_timeout(raw, 60000);
    if (auto s = db.exec(schema); !s)
        return Err{s.error()};
    return db;
}

Status Db::exec(const char* sql) const
{
    char* msg = nullptr;
    if (sqlite3_exec(db_.get(), sql, nullptr, nullptr, &msg) != SQLITE_OK) {
        std::string e = msg ? msg : "unknown";
        sqlite3_free(msg);
        return Err{"sql: " + e};
    }
    return ok;
}

Result<IndexStats> index_branch(Db& db, std::string_view branch, const Oid& commit,
                                const std::vector<FileVersion>& files, const ReadText& read)
{
    sqlite3* h = db.handle();
    Transaction tx(h);
    if (auto s = tx.begin(); !s)
        return Err{s.error()};

    auto file = upsert(h, "SELECT ord FROM files WHERE path = ?1 AND blob = ?2",
                       "INSERT INTO files (path, blob) VALUES (?1, ?2) RETURNING ord");
    auto chunk = upsert(h, "SELECT ord FROM chunks WHERE hash = ?1",
                        "INSERT INTO chunks (hash, kind, symbol_path, content) VALUES (?1, ?2, ?3, ?4) RETURNING ord");
    auto parsed = Stmt::prepare(h, "SELECT 1 FROM parsed WHERE blob = ?1 AND lang = ?2");
    auto mark = Stmt::prepare(h, "INSERT INTO parsed (blob, lang) VALUES (?1, ?2)");
    auto place = Stmt::prepare(h, "INSERT OR IGNORE INTO blob_chunks (blob, lang, chunk, start_line, end_line) "
                                  "VALUES (?1, ?2, ?3, ?4, ?5)");
    auto point = Stmt::prepare(h, "INSERT INTO branches (name, commit_sha, files) VALUES (?1, ?2, ?3) "
                                  "ON CONFLICT (name) DO UPDATE SET commit_sha = excluded.commit_sha, "
                                  "files = excluded.files");
    for (const std::string* e : {&file.error(), &chunk.error(), &parsed.error(), &mark.error(), &place.error(),
                                 &point.error()})
        if (!e->empty())
            return Err{*e};

    IndexStats stats;
    roaring::Roaring bitmap;
    for (const FileVersion& f : files) {
        auto lang = static_cast<int64_t>(parse::lang_of(f.path));
        auto ord = file->run([&](Stmt& s) { s.bind(1, f.path).bind_blob(2, f.blob.data(), f.blob.size()); });
        if (!ord)
            return Err{ord.error()};
        bitmap.add(static_cast<uint32_t>(ord->first));
        stats.files++;
        stats.new_files += ord->second;

        parsed->reset();
        parsed->bind_blob(1, f.blob.data(), f.blob.size()).bind(2, lang);
        bool seen = parsed->step() == SQLITE_ROW;
        parsed->reset();
        if (seen)
            continue;

        auto text = read(f.blob);
        if (!text)
            return Err{f.path + ": " + text.error()};
        std::vector<parse::Chunk> chunks = parse::chunk(static_cast<parse::Lang>(lang), *text);
        stats.parsed++;
        stats.chunks += chunks.size();
        for (const parse::Chunk& c : chunks) {
            auto hash = chunk_hash(static_cast<parse::Lang>(lang), c);
            auto id = chunk->run([&](Stmt& s) {
                s.bind_blob(1, hash.data(), hash.size())
                    .bind(2, static_cast<int64_t>(c.kind))
                    .bind(3, c.symbol)
                    .bind(4, c.content);
            });
            if (!id)
                return Err{id.error()};
            stats.new_chunks += id->second;
            place->reset();
            place->bind_blob(1, f.blob.data(), f.blob.size())
                .bind(2, lang)
                .bind(3, id->first)
                .bind(4, static_cast<int64_t>(c.start_line))
                .bind(5, static_cast<int64_t>(c.end_line));
            if (place->step() != SQLITE_DONE)
                return Err{"blob_chunks: " + place->error()};
        }
        mark->reset();
        mark->bind_blob(1, f.blob.data(), f.blob.size()).bind(2, lang);
        if (mark->step() != SQLITE_DONE)
            return Err{"parsed: " + mark->error()};
    }

    bitmap.runOptimize();
    std::string bytes(bitmap.getSizeInBytes(), '\0');
    bitmap.write(bytes.data());
    point->bind(1, branch)
        .bind_blob(2, commit.data(), commit.size())
        .bind_blob(3, reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
    if (point->step() != SQLITE_DONE)
        return Err{"branches: " + point->error()};
    if (auto s = tx.commit(); !s)
        return Err{s.error()};
    return stats;
}

Result<std::optional<Oid>> branch_commit(Db& db, std::string_view branch)
{
    auto s = Stmt::prepare(db.handle(), "SELECT commit_sha FROM branches WHERE name = ?1");
    if (!s)
        return Err{s.error()};
    s->bind(1, branch);
    if (s->step() != SQLITE_ROW)
        return std::optional<Oid>();
    Oid id{};
    std::string_view b = s->blob(0);
    std::memcpy(id.data(), b.data(), std::min(b.size(), id.size()));
    return std::optional<Oid>(id);
}

Result<roaring::Roaring> branch_files(Db& db, std::string_view branch)
{
    auto s = Stmt::prepare(db.handle(), "SELECT files FROM branches WHERE name = ?1");
    if (!s)
        return Err{s.error()};
    s->bind(1, branch);
    if (s->step() != SQLITE_ROW)
        return Err{"no such branch: " + std::string(branch)};
    std::string_view b = s->blob(0);
    return roaring::Roaring::readSafe(b.data(), b.size());
}

Result<Resolver> Resolver::make(Db& db, std::string_view branch)
{
    auto files = branch_files(db, branch);
    if (!files)
        return Err{files.error()};
    auto info = Stmt::prepare(db.handle(), "SELECT kind, symbol_path FROM chunks WHERE ord = ?1");
    auto where = Stmt::prepare(db.handle(),
                               "SELECT f.ord, f.path, bc.lang, bc.start_line, bc.end_line "
                               "FROM blob_chunks bc JOIN files f ON f.blob = bc.blob "
                               "WHERE bc.chunk = ?1 ORDER BY f.path, bc.start_line");
    if (!info)
        return Err{info.error()};
    if (!where)
        return Err{where.error()};
    return Resolver(std::move(*files), std::move(*info), std::move(*where));
}

Result<ChunkInfo> Resolver::resolve(uint32_t chunk)
{
    info_.reset();
    info_.bind(1, static_cast<int64_t>(chunk));
    if (info_.step() != SQLITE_ROW)
        return Err{"no such chunk: " + std::to_string(chunk)};
    ChunkInfo out{static_cast<parse::Kind>(info_.int64(0)), std::string(info_.text(1)), {}};

    where_.reset();
    where_.bind(1, static_cast<int64_t>(chunk));
    int rc;
    while ((rc = where_.step()) == SQLITE_ROW) {
        std::string_view path = where_.text(1);
        if (!files_.contains(static_cast<uint32_t>(where_.int64(0))) ||
            static_cast<int64_t>(parse::lang_of(path)) != where_.int64(2))
            continue;
        out.where.push_back({std::string(path), static_cast<uint32_t>(where_.int64(3)),
                             static_cast<uint32_t>(where_.int64(4))});
    }
    if (rc != SQLITE_DONE)
        return Err{"resolve: " + where_.error()};
    return out;
}

Result<std::vector<std::string>> function_names(Db& db, std::string_view branch)
{
    auto files = branch_files(db, branch);
    if (!files)
        return Err{files.error()};
    auto file = Stmt::prepare(db.handle(), "SELECT path, blob FROM files WHERE ord = ?1");
    auto names = Stmt::prepare(db.handle(),
                               "SELECT c.symbol_path FROM blob_chunks bc JOIN chunks c ON c.ord = bc.chunk "
                               "WHERE bc.blob = ?1 AND bc.lang = ?2 AND c.kind = 0 AND c.symbol_path != ''");
    if (!file)
        return Err{file.error()};
    if (!names)
        return Err{names.error()};
    std::vector<std::string> out;
    for (uint32_t ord : *files) {
        file->reset();
        file->bind(1, static_cast<int64_t>(ord));
        if (file->step() != SQLITE_ROW)
            return Err{"missing file " + std::to_string(ord)};
        std::string path(file->text(0));
        std::string blob(file->blob(1));
        names->reset();
        names->bind_blob(1, reinterpret_cast<const unsigned char*>(blob.data()), blob.size())
            .bind(2, static_cast<int64_t>(parse::lang_of(path)));
        while (names->step() == SQLITE_ROW)
            out.push_back(path + "::" + std::string(names->text(0)));
    }
    return out;
}

Result<int64_t> meta_int(Db& db, std::string_view key)
{
    auto s = Stmt::prepare(db.handle(), "SELECT value FROM meta WHERE key = ?1");
    if (!s)
        return Err{s.error()};
    s->bind(1, key);
    return s->step() == SQLITE_ROW ? s->int64(0) : int64_t{0};
}

Status set_meta_int(Db& db, std::string_view key, int64_t value)
{
    auto s = Stmt::prepare(db.handle(), "INSERT INTO meta (key, value) VALUES (?1, ?2) "
                                        "ON CONFLICT (key) DO UPDATE SET value = excluded.value");
    if (!s)
        return Err{s.error()};
    s->bind(1, key).bind(2, value);
    if (s->step() != SQLITE_DONE)
        return Err{"meta: " + s->error()};
    return ok;
}

} // namespace store
