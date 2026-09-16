#pragma once
#include <string>
#include <string_view>
#include <vector>

#include "result.h"
#include "store/store.h"

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

struct Ranked {
    std::vector<Candidate> chunks; // best first, every one visible on the branch
    std::vector<File> files;       // best first
};

// Up to `k` candidates visible on `branch`. Ranks, never truncates for budget:
// that is curate/'s decision (docs/modules/match.md).
Result<Ranked> retrieve(store::Db& db, std::string_view branch, std::string_view query, size_t k);

} // namespace match
