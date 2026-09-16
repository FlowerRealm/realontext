#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "result.h"
#include "store/sql.h"
#include "store/store.h"

namespace lexical {

// Registers the tokenizer and creates the FTS table. Once per connection.
Status attach(store::Db& db);

// Indexes chunks store/ added since the last sync. Chunks are append-only, so
// the highest synced ordinal is the whole state.
Status sync(store::Db& db);

// Query terms: every whole word and sub-word, stopwords removed, first occurrence order.
std::vector<std::string> terms(std::string_view query);

// Chunks in BM25 order, best first, across every branch. IDF is corpus-wide
// (docs/modules/lexical.md); branch visibility is match/'s concern.
class Ranking {
public:
    static Result<Ranking> search(store::Db& db, std::string_view query);

    struct Hit {
        uint32_t chunk;
        double score; // higher is better
    };
    // A hit, or nullopt when exhausted.
    Result<std::optional<Hit>> next();

private:
    explicit Ranking(std::optional<store::Stmt> stmt) : stmt_(std::move(stmt)) {}
    std::optional<store::Stmt> stmt_; // none when the query has no terms
};

} // namespace lexical
