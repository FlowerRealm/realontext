#pragma once
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "result.h"
#include "store/store.h"
#include "vector/vector.h"

namespace match {

struct Candidate {
    uint32_t chunk;
    double score;
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

} // namespace match
