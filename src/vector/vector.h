#pragma once
// Approximate nearest neighbours over the vectors store/ holds. Answers only
// "give me the top k" — branch visibility and the requery loop belong to match/
// (docs/modules/vector.md).
//
// The index is a derived cache: chunks.embedding is the truth, the index is
// rebuilt from it whole, and it stays out of the embedding fingerprint (D24).
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "result.h"
#include "store/store.h"

namespace vector {

// What the graph costs to build and how well it recalls. Every field is a sweep
// target (open-questions C5, C7); a sweep rebuilds the index and never re-embeds.
struct Params {
    size_t connectivity = 16;     // HNSW M
    size_t expansion_add = 128;   // efConstruction
    size_t expansion_search = 64; // efSearch
    std::string scalar = "f32";   // index precision: f32, f16, bf16 or i8
};

// The index belonging to a database: `<db path>.usearch`.
std::string index_path(std::string_view db_path);

struct BuildStats {
    size_t vectors = 0;
    size_t bytes = 0; // the index file
};

// Rebuilds the index from every stored vector and records what it was built
// from, so a later open can tell it went stale. Whole-index: there is no
// incremental path, and none is needed until GC exists (D24).
Result<BuildStats> build(store::Db& db, uint32_t dim, const Params& params, std::string_view db_path,
                         const std::function<void(size_t added, size_t total)>& progress);

struct Hit {
    uint32_t chunk;
    float score; // cosine, the same scale an exact scan reports
};

class Index {
public:
    // Fails when no index was built, when the file is gone, or when the database
    // has been embedded further since: a stale index silently drops chunks.
    static Result<Index> open(store::Db& db, std::string_view db_path, size_t expansion_search);
    ~Index();
    Index(Index&&) noexcept;
    Index& operator=(Index&&) noexcept;

    // The `k` nearest, best first. Fewer than `k` only when the index is smaller.
    Result<std::vector<Hit>> search(std::span<const float> query, size_t k) const;
    size_t size() const;
    uint32_t dim() const;

private:
    struct Impl;
    explicit Index(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace vector
