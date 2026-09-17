#include "match/match.h"

#include <algorithm>
#include <functional>
#include <unordered_map>
#include <unordered_set>

#include "ingest/filter.h"
#include "lexical/lexical.h"
#include "store/embed.h"

namespace match {

namespace {

// Reciprocal Rank Fusion constant, the value from the original RRF paper.
constexpr double rrf_k = 60.0;

// How far the ANN over-fetches before branch filtering. Grows until both sides
// are full rather than over-fetching by a fixed multiple, which no single value
// gets right (D24).
constexpr size_t ann_first = 1000;
constexpr size_t ann_growth = 4;
constexpr size_t ann_last = ann_first * ann_growth * ann_growth;

// Keeps only the locations belonging to `side`. False when none is left.
bool keep_side(store::ChunkInfo& info, Side side)
{
    std::erase_if(info.where, [&](const store::Location& l) { return ingest::is_test(l.path) != (side == Side::Tests); });
    return !info.where.empty();
}

// Reads a best-first ranking until k items are admitted. `admit` returns
// nullopt for a document with nothing on the branch and side.
template <class Item>
Result<std::vector<Item>> best(lexical::Ranking& ranking, size_t k,
                               const std::function<Result<std::optional<Item>>(const lexical::Ranking::Hit&)>& admit)
{
    std::vector<Item> kept;
    while (kept.size() < k) {
        auto hit = ranking.next();
        if (!hit)
            return Err{hit.error()};
        if (!*hit)
            break;
        auto item = admit(**hit);
        if (!item)
            return Err{item.error()};
        if (*item)
            kept.push_back(std::move(**item));
    }
    return kept;
}

struct Document {
    std::vector<std::string> paths;
    double score;
};

Result<Group> lexical_group(store::Db& db, store::Resolver& resolver, std::string_view query, size_t k, Side side)
{
    auto chunk_ranking = lexical::Ranking::search(db, query, lexical::Unit::Chunk);
    if (!chunk_ranking)
        return Err{chunk_ranking.error()};
    auto file_ranking = lexical::Ranking::search(db, query, lexical::Unit::File);
    if (!file_ranking)
        return Err{file_ranking.error()};

    auto chunks = best<Candidate>(*chunk_ranking, k, [&](const lexical::Ranking::Hit& h) -> Result<std::optional<Candidate>> {
        auto info = resolver.resolve(h.id);
        if (!info)
            return Err{info.error()};
        if (!keep_side(*info, side))
            return std::optional<Candidate>();
        return std::optional<Candidate>(Candidate{h.id, h.score, std::move(*info)});
    });
    if (!chunks)
        return Err{chunks.error()};

    auto documents = best<Document>(*file_ranking, k, [&](const lexical::Ranking::Hit& h) -> Result<std::optional<Document>> {
        auto paths = resolver.paths(h.id);
        if (!paths)
            return Err{paths.error()};
        std::erase_if(*paths, [&](const std::string& p) { return ingest::is_test(p) != (side == Side::Tests); });
        if (paths->empty())
            return std::optional<Document>();
        return std::optional<Document>(Document{std::move(*paths), h.score});
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

    Group out;
    for (auto& [path, score] : fused)
        out.files.push_back({path, score});
    std::sort(out.files.begin(), out.files.end(), [](const File& a, const File& b) {
        return a.score != b.score ? a.score > b.score : a.path < b.path;
    });

    // A chunk in a highly ranked file is more likely the one that matters: fuse
    // each chunk's own rank with the rank of the best file it occurs in.
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

// Every chunk must have a vector, or a vector ranking skips some without saying so.
Status all_embedded(store::Db& db)
{
    auto missing = store::unembedded(db);
    if (!missing)
        return Err{missing.error()};
    if (*missing)
        return Err{std::to_string(*missing) + " chunks have no embedding: run realontext embed"};
    return ok;
}

// Walks a best-first ranking, admitting each chunk into whichever sides it has
// locations on, until both sides hold k. `full` reports whether both did.
Result<Ranked> admit(store::Resolver& resolver, const std::vector<std::pair<float, uint32_t>>& scored, size_t k,
                     bool& full)
{
    Ranked out;
    std::unordered_set<std::string> seen; // a path is on one side only
    for (const auto& [score, chunk] : scored) {
        if (out.code.chunks.size() == k && out.tests.chunks.size() == k)
            break;
        auto info = resolver.resolve(chunk);
        if (!info)
            return Err{info.error()};
        for (Side side : {Side::Code, Side::Tests}) {
            Group& g = side == Side::Code ? out.code : out.tests;
            store::ChunkInfo mine = *info;
            if (g.chunks.size() == k || !keep_side(mine, side))
                continue;
            for (const store::Location& l : mine.where)
                if (seen.insert(l.path).second)
                    g.files.push_back({l.path, score});
            g.chunks.push_back({chunk, score, std::move(mine)});
        }
    }
    full = out.code.chunks.size() == k && out.tests.chunks.size() == k;
    return out;
}

} // namespace

Result<Ranked> retrieve(store::Db& db, std::string_view branch, std::string_view query, size_t k)
{
    auto resolver = store::Resolver::make(db, branch);
    if (!resolver)
        return Err{resolver.error()};
    auto code = lexical_group(db, *resolver, query, k, Side::Code);
    if (!code)
        return Err{code.error()};
    auto tests = lexical_group(db, *resolver, query, k, Side::Tests);
    if (!tests)
        return Err{tests.error()};
    return Ranked{std::move(*code), std::move(*tests)};
}

Result<Ranked> nearest(store::Db& db, std::string_view branch, std::span<const float> query, size_t k)
{
    if (auto s = all_embedded(db); !s)
        return Err{s.error()};
    auto resolver = store::Resolver::make(db, branch);
    if (!resolver)
        return Err{resolver.error()};

    std::vector<std::pair<float, uint32_t>> scored;
    auto scan = store::each_vector(db, static_cast<uint32_t>(query.size()), [&](uint32_t chunk, std::span<const float> v) {
        float dot = 0;
        for (size_t i = 0; i < v.size(); i++)
            dot += v[i] * query[i];
        scored.emplace_back(dot, chunk);
    });
    if (!scan)
        return Err{scan.error()};
    std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first > b.first : a.second < b.second;
    });

    bool full = false;
    return admit(*resolver, scored, k, full);
}

Result<Ranked> nearest_ann(store::Db& db, std::string_view branch, const vector::Index& index,
                           std::span<const float> query, size_t k)
{
    if (auto s = all_embedded(db); !s)
        return Err{s.error()};
    auto resolver = store::Resolver::make(db, branch);
    if (!resolver)
        return Err{resolver.error()};

    Ranked out;
    for (size_t wanted = ann_first; wanted <= ann_last; wanted *= ann_growth) {
        auto hits = index.search(query, wanted);
        if (!hits)
            return Err{hits.error()};
        std::vector<std::pair<float, uint32_t>> scored;
        scored.reserve(hits->size());
        for (const vector::Hit& h : *hits)
            scored.emplace_back(h.score, h.chunk);

        bool full = false;
        auto ranked = admit(*resolver, scored, k, full);
        if (!ranked)
            return Err{ranked.error()};
        out = std::move(*ranked);
        // Both sides full, or the index has nothing more to give: asking for a
        // larger k would return the same hits.
        if (full || hits->size() < wanted)
            break;
    }
    return out;
}

} // namespace match
