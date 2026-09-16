#include "lexical/lexical.h"

#include <algorithm>
#include <array>
#include <unordered_set>

#include "lexical/tokenizer.h"

namespace lexical {

namespace {

// English function words. Sorted: looked up by binary search.
constexpr std::array<std::string_view, 127> stopwords = {
    "about", "above", "after", "again", "against", "all", "am", "an", "and", "any", "are", "as", "at",
    "be", "because", "been", "before", "being", "below", "between", "both", "but", "by", "can", "could",
    "did", "do", "does", "doing", "down", "during", "each", "few", "for", "from", "further", "had",
    "has", "have", "having", "he", "her", "here", "hers", "herself", "him", "himself", "his", "how",
    "if", "in", "into", "is", "it", "its", "itself", "just", "me", "might", "more", "most", "must", "my",
    "myself", "no", "nor", "not", "now", "of", "off", "on", "once", "only", "or", "other", "our", "ours",
    "ourselves", "out", "over", "own", "same", "shall", "she", "should", "so", "some", "such", "than",
    "that", "the", "their", "theirs", "them", "themselves", "then", "there", "these", "they", "this",
    "those", "through", "to", "too", "under", "until", "up", "very", "was", "we", "were", "what", "when",
    "where", "which", "while", "who", "whom", "why", "will", "with", "would", "you", "your", "yours",
    "yourself", "yourselves",
};

bool stopword(std::string_view w) { return std::binary_search(stopwords.begin(), stopwords.end(), w); }

// Column weights for bm25(): a hit in the definition's name says more than a
// hit somewhere in its body.
constexpr const char* ranked_chunks = "SELECT rowid, bm25(chunk_fts, 2.0, 1.0) FROM chunk_fts "
                                      "WHERE chunk_fts MATCH ?1 ORDER BY 2, 1";
constexpr const char* ranked_files = "SELECT rowid, bm25(file_fts) FROM file_fts "
                                     "WHERE file_fts MATCH ?1 ORDER BY 2, 1";

} // namespace

Status attach(store::Db& db)
{
    if (auto s = register_tokenizer(db.handle()); !s)
        return s;
    if (auto s = db.exec("CREATE VIRTUAL TABLE IF NOT EXISTS chunk_fts USING fts5("
                         "symbol_path, content, content = 'chunks', content_rowid = 'ord', tokenize = 'code')");
        !s)
        return s;
    // Contentless: the text already lives in chunks, only the index is needed.
    return db.exec("CREATE VIRTUAL TABLE IF NOT EXISTS file_fts USING fts5("
                   "content, content = '', tokenize = 'code')");
}

Status sync(store::Db& db)
{
    if (auto s = db.exec("BEGIN IMMEDIATE"); !s)
        return s;
    auto done = [&](Status s) {
        db.exec(s ? "COMMIT" : "ROLLBACK");
        return s;
    };
    // Both tables grow append-only by ordinal, so each keeps one high-water mark.
    struct Feed {
        const char* key;
        const char* insert;
        const char* top;
    };
    constexpr Feed feeds[] = {
        {"fts_synced", "INSERT INTO chunk_fts (rowid, symbol_path, content) "
                       "SELECT ord, symbol_path, content FROM chunks WHERE ord > ?1",
         "SELECT coalesce(max(ord), 0) FROM chunks"},
        {"file_fts_synced", "INSERT INTO file_fts (rowid, content) "
                            "SELECT p.ord, group_concat(c.content, char(10)) FROM parsed p "
                            "JOIN blob_chunks bc ON bc.blob = p.blob AND bc.lang = p.lang "
                            "JOIN chunks c ON c.ord = bc.chunk WHERE p.ord > ?1 GROUP BY p.ord",
         "SELECT coalesce(max(ord), 0) FROM parsed"},
    };
    for (const Feed& f : feeds) {
        auto synced = store::meta_int(db, f.key);
        if (!synced)
            return done(Err{synced.error()});
        auto insert = store::Stmt::prepare(db.handle(), f.insert);
        auto top = store::Stmt::prepare(db.handle(), f.top);
        if (!insert)
            return done(Err{insert.error()});
        if (!top)
            return done(Err{top.error()});
        insert->bind(1, *synced);
        if (insert->step() != SQLITE_DONE)
            return done(Err{std::string("fts sync ") + f.key + ": " + insert->error()});
        top->step();
        if (auto s = store::set_meta_int(db, f.key, top->int64(0)); !s)
            return done(s);
    }
    return done(ok);
}

std::vector<std::string> terms(std::string_view query)
{
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    tokenize(query, true, [&](std::string_view t, bool, size_t, size_t) {
        if (!stopword(t) && seen.insert(std::string(t)).second)
            out.emplace_back(t);
        return true;
    });
    return out;
}

Result<Ranking> Ranking::search(store::Db& db, std::string_view query, Unit unit)
{
    std::string match;
    for (const std::string& t : terms(query)) {
        if (!match.empty())
            match += " OR ";
        match += '"' + t + '"';
    }
    if (match.empty())
        return Ranking(std::nullopt);
    auto stmt = store::Stmt::prepare(db.handle(), unit == Unit::Chunk ? ranked_chunks : ranked_files);
    if (!stmt)
        return Err{stmt.error()};
    stmt->bind(1, match);
    return Ranking(std::move(*stmt));
}

Result<std::optional<Ranking::Hit>> Ranking::next()
{
    if (!stmt_)
        return std::optional<Hit>();
    int rc = stmt_->step();
    if (rc == SQLITE_DONE)
        return std::optional<Hit>();
    if (rc != SQLITE_ROW)
        return Err{"search: " + stmt_->error()};
    return std::optional<Hit>(Hit{static_cast<uint32_t>(stmt_->int64(0)), -stmt_->real(1)});
}

} // namespace lexical
