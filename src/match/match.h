#pragma once
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "model/model.h"
#include "result.h"
#include "store/store.h"
#include "vector/vector.h"

namespace match {

struct Candidate {
    uint32_t chunk;
    double score; // fused rank score, comparable only within a Group
    store::ChunkInfo info; // info.where holds only locations on the queried branch
};

struct File {
    std::string path;
    double score;
};

// Test code and the code it tests answer different questions, so they rank
// apart: code is where a change goes, tests show what exercises that behaviour
// and what failed to catch it. Test paths follow ingest::is_test.
enum class Side { Code, Tests };

struct Group {
    std::vector<Candidate> chunks; // best first, every one visible on the branch
    std::vector<File> files;       // best first
};

struct Ranked {
    Group code;
    Group tests;
};

// Up to `k` candidates per side visible on `branch`. Ranks, never truncates for
// budget: that is curate/'s decision (docs/modules/match.md).
Result<Ranked> retrieve(store::Db& db, std::string_view branch, std::string_view query, size_t k);

// The k chunks visible on `branch` nearest to `query` by cosine, over every
// stored vector. An exact scan: slow on large corpora, and the reference an ANN
// index in vector/ is measured against. Files rank by their best chunk; up to k per side.
Result<Ranked> nearest(store::Db& db, std::string_view branch, std::span<const float> query, size_t k);

// The same ranking through vector/'s index. Over-fetches by growing k and
// asking again until both sides hold k visible chunks or the index runs out
// (D24): a hot function's many versions can fill the head of the ranking while
// only one of them is visible, so no fixed multiple is enough.
Result<Ranked> nearest_ann(store::Db& db, std::string_view branch, const vector::Index& index,
                           std::span<const float> query, size_t k);

// Reordering what a route assembled, with the query and the candidates' own
// text in front of the model (D27). Orthogonal to the routes: it applies to
// whatever any of them produced, so it is a switch rather than a route name,
// and the rerank delta on each route can be measured separately.
struct Reranking {
    model::Reranker* model;
    size_t pool;      // candidates per side handed to the model
    size_t max_bytes; // one document is cut here, on a UTF-8 boundary
    // What to do with the order the fuser already had. Replacing it spends the
    // set to buy the top: a candidate the fuser had inside k that the model
    // ranks past k leaves the ranking entirely. Fusing keeps both as ranks,
    // the way every other signal in this module is combined.
    bool fuse = false;
    // What happens to the file ranking afterwards. The model ranks chunks and
    // never sees a file, so deriving one from its order invents an opinion it
    // did not give; keeping the fuser's leaves the file ranking as it was.
    enum class Files { Derive, Fuse, Keep };
    Files files = Files::Derive;
};

// Reorders `ranked` in place. It can only reorder: what the pool already holds
// is the ceiling, so the score report leads with the pool's recall.
//
// Candidates past the pool keep their fused order behind the reranked ones and
// score below every reranked one, so the score stays non-increasing down the
// ranking. Nothing reads the sign: a reranker may score a poor match negative
// and that must not be confused with never having been read. A failure is an
// error; falling back to the fused order would hand back a differently ranked
// list with nothing to say so. An error leaves `ranked` half-reordered — there
// is no ranking to hand back either way.
Status rerank(store::Db& db, std::string_view query, Ranked& ranked, const Reranking& r);

// Both routes at once, fused by rank (D26). The lexical route finds the
// identifiers and error strings a vector cannot place, the vector route the
// code that never spells the query's words; each is blind where the other sees.
Result<Ranked> hybrid(store::Db& db, std::string_view branch, std::string_view text, const vector::Index& index,
                      std::span<const float> query, size_t k);

} // namespace match
