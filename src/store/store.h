#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <roaring/roaring.hh>

#include "oid.h"
#include "parse/chunk.h"
#include "result.h"
#include "store/sql.h"

struct sqlite3;

namespace store {

// Everything that turns a blob into the text model/ embeds: parse/ chunking,
// the grammar pins in cmake/grammars.cmake, embed_text(). Bump it when any of
// them changes. A database holding vectors of another version refuses to open.
inline constexpr int64_t input_version = 1;

// One SQLite file per Project (D13). Tables: docs/modules/store.md.
class Db {
public:
    static Result<Db> open(const std::string& path);

    sqlite3* handle() const { return db_.get(); }
    Status exec(const char* sql) const;

private:
    struct Close {
        void operator()(sqlite3* db) const;
    };
    std::unique_ptr<sqlite3, Close> db_;
};

// Blob content by id. Supplied by the caller so store/ never touches git.
using ReadText = std::function<Result<std::string>(const Oid&)>;

struct IndexStats {
    size_t files = 0;       // file versions on the branch
    size_t new_files = 0;   // (path, blob) pairs never seen on any branch
    size_t parsed = 0;      // (blob, language) pairs sent to parse/
    size_t chunks = 0;      // chunks produced by those parses
    size_t new_chunks = 0;  // of which content-addressed storage had never seen
};

// Point `branch` at `commit` whose tree is `files`. One transaction: the branch
// either moves completely or not at all.
Result<IndexStats> index_branch(Db& db, std::string_view branch, const Oid& commit,
                                const std::vector<FileVersion>& files, const ReadText& read);

// The commit a branch was last indexed at, if any.
Result<std::optional<Oid>> branch_commit(Db& db, std::string_view branch);

Result<roaring::Roaring> branch_files(Db& db, std::string_view branch);

struct Location {
    std::string path;
    uint32_t start_line;
    uint32_t end_line;
};

struct ChunkInfo {
    parse::Kind kind;
    std::string symbol;
    std::vector<Location> where; // every place the chunk occurs on the branch; empty = invisible
};

// Visibility and path resolution are one lookup:
// chunk -> blobs containing it -> file versions of those blobs -> ∩ branch bitmap.
class Resolver {
public:
    static Result<Resolver> make(Db& db, std::string_view branch);
    Result<ChunkInfo> resolve(uint32_t chunk);
    // Paths on the branch holding the parsed (blob, lang) document `parsed`.
    Result<std::vector<std::string>> paths(uint32_t parsed);

private:
    Resolver(roaring::Roaring files, Stmt info, Stmt where, Stmt doc)
        : files_(std::move(files)), info_(std::move(info)), where_(std::move(where)), doc_(std::move(doc)) {}
    roaring::Roaring files_;
    Stmt info_, where_, doc_;
};

// A chunk's stored text. serve/ hands it back verbatim until curate/ is the one
// deciding fidelity.
Result<std::string> chunk_text(Db& db, uint32_t chunk);

// Every function chunk on the branch as path::symbol.
Result<std::vector<std::string>> function_names(Db& db, std::string_view branch);

// Small key/value table for modules that keep bookkeeping in the same file.
Result<int64_t> meta_int(Db& db, std::string_view key);
Status set_meta_int(Db& db, std::string_view key, int64_t value);

} // namespace store
