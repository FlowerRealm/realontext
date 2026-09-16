#include "match/match.h"

#include <algorithm>
#include <functional>
#include <queue>
#include <unordered_map>

#include "ingest/filter.h"
#include "lexical/lexical.h"

namespace match {

namespace {

// Test code stays retrievable but ranks below implementation: a task
// description asks where behaviour lives, and a test that reproduces the
// behaviour shares its vocabulary almost word for word.
constexpr double test_weight = 0.5;

// Reciprocal Rank Fusion constant, the value from the original RRF paper.
constexpr double rrf_k = 60.0;

bool all_tests(const std::vector<std::string>& paths)
{
    return std::all_of(paths.begin(), paths.end(), [](const std::string& p) { return ingest::is_test(p); });
}

// Reads a best-first ranking and keeps the k best by adjusted score, where
// adjusted = raw × weight and weight ≤ 1. Once k are kept and the next raw
// score is no higher than the k-th adjusted one, nothing further can enter.
// `admit` returns the adjusted score, or nullopt for a document not on the branch.
template <class Item>
Result<std::vector<Item>> best(lexical::Ranking& ranking, size_t k,
                               const std::function<Result<std::optional<Item>>(const lexical::Ranking::Hit&)>& admit)
{
    std::vector<Item> kept;
    std::priority_queue<double, std::vector<double>, std::greater<>> floor; // k best adjusted scores
    for (;;) {
        auto hit = ranking.next();
        if (!hit)
            return Err{hit.error()};
        if (!*hit || (floor.size() == k && (*hit)->score <= floor.top()))
            break;
        auto item = admit(**hit);
        if (!item)
            return Err{item.error()};
        if (!*item)
            continue;
        floor.push((*item)->score);
        if (floor.size() > k)
            floor.pop();
        kept.push_back(std::move(**item));
    }
    std::stable_sort(kept.begin(), kept.end(), [](const Item& a, const Item& b) { return a.score > b.score; });
    if (kept.size() > k)
        kept.resize(k);
    return kept;
}

struct Document {
    std::vector<std::string> paths;
    double score;
};

std::vector<std::string> paths_of(const store::ChunkInfo& info)
{
    std::vector<std::string> out;
    for (const store::Location& l : info.where)
        out.push_back(l.path);
    return out;
}

} // namespace

Result<Ranked> retrieve(store::Db& db, std::string_view branch, std::string_view query, size_t k)
{
    auto resolver = store::Resolver::make(db, branch);
    if (!resolver)
        return Err{resolver.error()};
    auto chunk_ranking = lexical::Ranking::search(db, query, lexical::Unit::Chunk);
    if (!chunk_ranking)
        return Err{chunk_ranking.error()};
    auto file_ranking = lexical::Ranking::search(db, query, lexical::Unit::File);
    if (!file_ranking)
        return Err{file_ranking.error()};

    auto chunks = best<Candidate>(*chunk_ranking, k, [&](const lexical::Ranking::Hit& h) -> Result<std::optional<Candidate>> {
        auto info = resolver->resolve(h.id);
        if (!info)
            return Err{info.error()};
        if (info->where.empty())
            return std::optional<Candidate>();
        double w = all_tests(paths_of(*info)) ? test_weight : 1.0;
        return std::optional<Candidate>(Candidate{h.id, h.score * w, std::move(*info)});
    });
    if (!chunks)
        return Err{chunks.error()};

    auto documents = best<Document>(*file_ranking, k, [&](const lexical::Ranking::Hit& h) -> Result<std::optional<Document>> {
        auto paths = resolver->paths(h.id);
        if (!paths)
            return Err{paths.error()};
        if (paths->empty())
            return std::optional<Document>();
        double w = all_tests(*paths) ? test_weight : 1.0;
        return std::optional<Document>(Document{std::move(*paths), h.score * w});
    });
    if (!documents)
        return Err{documents.error()};

    // Two file rankings: by a file's best chunk (precise, blind to evidence spread
    // over a large file) and by the whole file as one document (the reverse).
    // Fused by rank, since the two BM25 scores are on different scales.
    std::vector<std::string> by_chunk;
    for (const Candidate& c : *chunks)
        for (const store::Location& l : c.info.where)
            if (std::find(by_chunk.begin(), by_chunk.end(), l.path) == by_chunk.end())
                by_chunk.push_back(l.path);
    std::vector<std::string> by_file;
    for (const Document& d : *documents)
        for (const std::string& p : d.paths)
            by_file.push_back(p);

    std::unordered_map<std::string, double> fused;
    for (const auto* list : {&by_chunk, &by_file})
        for (size_t i = 0; i < list->size(); i++)
            fused[(*list)[i]] += 1.0 / (rrf_k + static_cast<double>(i + 1));

    Ranked out;
    for (auto& [path, score] : fused)
        out.files.push_back({path, score});
    std::sort(out.files.begin(), out.files.end(), [](const File& a, const File& b) {
        return a.score != b.score ? a.score > b.score : a.path < b.path;
    });

    // A chunk in a highly ranked file is more likely the one to change: fuse each
    // chunk's own rank with the rank of the best file it occurs in.
    std::unordered_map<std::string, size_t> file_rank;
    for (size_t i = 0; i < out.files.size(); i++)
        file_rank.emplace(out.files[i].path, i);
    out.chunks = std::move(*chunks);
    for (size_t i = 0; i < out.chunks.size(); i++) {
        size_t best_file = out.files.size();
        for (const store::Location& l : out.chunks[i].info.where)
            best_file = std::min(best_file, file_rank.at(l.path));
        out.chunks[i].score = 1.0 / (rrf_k + static_cast<double>(i + 1)) +
                              1.0 / (rrf_k + static_cast<double>(best_file + 1));
    }
    std::stable_sort(out.chunks.begin(), out.chunks.end(),
                     [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
    return out;
}

} // namespace match
