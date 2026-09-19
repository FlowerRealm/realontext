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

// How much a route's ranks count against the other's. The vector route is the
// stronger of the two on this corpus, so equal weights let the weaker one drag
// it down; 0.5 against 1.0 is where the sweep flattened out (D26).
constexpr double lexical_weight = 0.5;
constexpr double vector_weight = 1.0;

// How far the ANN over-fetches before branch filtering. Grows until both sides
// are full rather than over-fetching by a fixed multiple, which no single value
// gets right (D24).
constexpr size_t ann_first = 1000;
constexpr size_t ann_growth = 4;
constexpr size_t ann_last = ann_first * ann_growth * ann_growth;

double rrf(size_t rank)
{
    return 1.0 / (rrf_k + static_cast<double>(rank + 1));
}

// Keeps only the locations belonging to `side`. False when none is left.
bool keep_side(store::ChunkInfo& info, Side side)
{
    std::erase_if(info.where, [&](const store::Location& l) { return ingest::is_test(l.path) != (side == Side::Tests); });
    return !info.where.empty();
}

// What one route offers for one side: its chunks best first, and the path
// rankings it can order. Only the order is ever read — a route's own scores are
// cosines or BM25, which are not comparable with each other's.
struct Lane {
    std::vector<Candidate> chunks;
    std::vector<std::vector<std::string>> files;
};

// One route's output, and how heavily it counts in the fusion.
struct Route {
    Lane code, tests;
    double weight = 1.0;
};

// Fuses one side of every route into a ranking of at most k chunks.
//
// Files first, because a chunk's rank depends on its file's: a function that
// does not stand out alone is likelier the one to change when its file is at
// the top. Each route's file rankings share that route's weight, so a route
// that offers two of them does not thereby count twice.
Group fuse_side(const std::vector<std::pair<const Lane*, double>>& lanes, size_t k)
{
    std::unordered_map<std::string, double> file_score;
    for (const auto& [lane, weight] : lanes) {
        if (lane->files.empty())
            continue;
        double each = weight / static_cast<double>(lane->files.size());
        for (const std::vector<std::string>& list : lane->files)
            for (size_t i = 0; i < list.size(); i++)
                file_score[list[i]] += each * rrf(i);
    }

    Group out;
    out.files.reserve(file_score.size());
    for (auto& [path, score] : file_score)
        out.files.push_back({path, score});
    std::sort(out.files.begin(), out.files.end(), [](const File& a, const File& b) {
        return a.score != b.score ? a.score > b.score : a.path < b.path;
    });

    std::unordered_map<std::string, size_t> file_rank;
    for (size_t i = 0; i < out.files.size(); i++)
        file_rank.emplace(out.files[i].path, i);

    // The union of the routes' candidates: one absent from a route scores
    // nothing there, which is the whole of RRF's tolerance for a blind route.
    double total = 0;
    for (const auto& [lane, weight] : lanes)
        total += weight;
    std::unordered_map<uint32_t, size_t> at;
    std::vector<size_t> best_rank; // the best rank any route gave it, for ties
    for (const auto& [lane, weight] : lanes) {
        for (size_t i = 0; i < lane->chunks.size(); i++) {
            const Candidate& c = lane->chunks[i];
            auto [it, fresh] = at.emplace(c.chunk, out.chunks.size());
            if (fresh) {
                out.chunks.push_back(c);
                out.chunks.back().score = 0;
                best_rank.push_back(i);
            }
            out.chunks[it->second].score += weight * rrf(i) / total;
            best_rank[it->second] = std::min(best_rank[it->second], i);
        }
    }
    for (size_t i = 0; i < out.chunks.size(); i++) {
        size_t best_file = out.files.size();
        for (const store::Location& l : out.chunks[i].info.where)
            best_file = std::min(best_file, file_rank.at(l.path));
        out.chunks[i].score += rrf(best_file);
    }

    std::vector<size_t> order(out.chunks.size());
    for (size_t i = 0; i < order.size(); i++)
        order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        if (out.chunks[a].score != out.chunks[b].score)
            return out.chunks[a].score > out.chunks[b].score;
        if (best_rank[a] != best_rank[b])
            return best_rank[a] < best_rank[b];
        return out.chunks[a].chunk < out.chunks[b].chunk;
    });
    std::vector<Candidate> sorted;
    sorted.reserve(std::min(order.size(), k));
    for (size_t i = 0; i < order.size() && i < k; i++)
        sorted.push_back(std::move(out.chunks[order[i]]));
    out.chunks = std::move(sorted);
    return out;
}

Ranked fuse(const std::vector<Route>& routes, size_t k)
{
    std::vector<std::pair<const Lane*, double>> code, tests;
    for (const Route& r : routes) {
        code.emplace_back(&r.code, r.weight);
        tests.emplace_back(&r.tests, r.weight);
    }
    return {fuse_side(code, k), fuse_side(tests, k)};
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

// BM25 over one side. Two file rankings, because they fail differently: a file
// ranked by its best chunk is precise but blind to evidence spread over a large
// file, and the whole file as one document is the reverse.
Result<Lane> lexical_lane(store::Db& db, store::Resolver& resolver, std::string_view query, size_t k, Side side)
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

    std::vector<std::string> by_chunk;
    for (const Candidate& c : *chunks)
        for (const store::Location& l : c.info.where)
            if (std::find(by_chunk.begin(), by_chunk.end(), l.path) == by_chunk.end())
                by_chunk.push_back(l.path);

    using Paths = std::vector<std::string>;
    auto documents = best<Paths>(*file_ranking, k, [&](const lexical::Ranking::Hit& h) -> Result<std::optional<Paths>> {
        auto paths = resolver.paths(h.id);
        if (!paths)
            return Err{paths.error()};
        std::erase_if(*paths, [&](const std::string& p) { return ingest::is_test(p) != (side == Side::Tests); });
        if (paths->empty())
            return std::optional<Paths>();
        return std::optional<Paths>(std::move(*paths));
    });
    if (!documents)
        return Err{documents.error()};

    std::vector<std::string> by_file;
    for (Paths& paths : *documents)
        for (std::string& p : paths)
            by_file.push_back(std::move(p));

    return Lane{std::move(*chunks), {std::move(by_chunk), std::move(by_file)}};
}

Result<Route> lexical_route(store::Db& db, store::Resolver& resolver, std::string_view query, size_t k)
{
    auto code = lexical_lane(db, resolver, query, k, Side::Code);
    if (!code)
        return Err{code.error()};
    auto tests = lexical_lane(db, resolver, query, k, Side::Tests);
    if (!tests)
        return Err{tests.error()};
    return Route{std::move(*code), std::move(*tests), lexical_weight};
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

bool both_full(const Route& r, size_t k)
{
    return r.code.chunks.size() == k && r.tests.chunks.size() == k;
}

// Walks a best-first cosine ranking, admitting each chunk into whichever side
// it has locations on, until both sides hold k. Files follow the chunks: there
// is no whole-file vector to rank a file by on its own.
Result<Route> vector_route(store::Resolver& resolver, const std::vector<std::pair<float, uint32_t>>& scored, size_t k)
{
    Route out;
    out.weight = vector_weight;
    std::unordered_set<std::string> seen; // a path belongs to one side only
    for (const auto& [score, chunk] : scored) {
        if (both_full(out, k))
            break;
        auto info = resolver.resolve(chunk);
        if (!info)
            return Err{info.error()};
        for (Side side : {Side::Code, Side::Tests}) {
            Lane& lane = side == Side::Code ? out.code : out.tests;
            store::ChunkInfo mine = *info;
            if (lane.chunks.size() == k || !keep_side(mine, side))
                continue;
            if (lane.files.empty())
                lane.files.emplace_back();
            for (const store::Location& l : mine.where)
                if (seen.insert(l.path).second)
                    lane.files.front().push_back(l.path);
            lane.chunks.push_back({chunk, score, std::move(mine)});
        }
    }
    return out;
}

Result<std::vector<std::pair<float, uint32_t>>> exact_scan(store::Db& db, std::span<const float> query)
{
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
    return scored;
}

Result<Route> ann_route(store::Resolver& resolver, const vector::Index& index, std::span<const float> query, size_t k)
{
    Route out;
    out.weight = vector_weight;
    for (size_t wanted = ann_first; wanted <= ann_last; wanted *= ann_growth) {
        auto hits = index.search(query, wanted);
        if (!hits)
            return Err{hits.error()};
        std::vector<std::pair<float, uint32_t>> scored;
        scored.reserve(hits->size());
        for (const vector::Hit& h : *hits)
            scored.emplace_back(h.score, h.chunk);

        auto route = vector_route(resolver, scored, k);
        if (!route)
            return Err{route.error()};
        out = std::move(*route);
        // Both sides full, or the index has nothing more to give: asking for a
        // larger k would return the same hits.
        if (both_full(out, k) || hits->size() < wanted)
            break;
    }
    return out;
}

// Reranks one side in place. The documents are the same text the embedder saw,
// cut at the reranker's own limit: one definition of "a chunk as text", and one
// that no stored vector depends on.
Status rerank_side(store::Db& db, std::string_view query, Group& group, const Reranking& r)
{
    size_t pool = std::min(r.pool, group.chunks.size());
    if (pool == 0)
        return ok;
    std::vector<std::string> documents;
    documents.reserve(pool);
    for (size_t i = 0; i < pool; i++) {
        auto text = store::chunk_text(db, group.chunks[i].chunk);
        if (!text)
            return Err{text.error()};
        documents.push_back(store::embed_text(group.chunks[i].info.symbol, *text, r.max_bytes));
    }

    auto scored = r.model->rank(std::string(query), documents);
    if (!scored)
        return Err{scored.error()};

    std::vector<Candidate> chunks;
    chunks.reserve(group.chunks.size());
    if (r.fuse) {
        // RRF over the two rankings of the same pool: the rank the fuser gave a
        // candidate and the rank the model gave it, k = 60 as everywhere else.
        std::vector<std::pair<double, size_t>> by_score;
        by_score.reserve(pool);
        for (size_t j = 0; j < scored->size(); j++)
            by_score.emplace_back(rrf(scored->at(j).index) + rrf(j), scored->at(j).index);
        std::sort(by_score.begin(), by_score.end(), [](const auto& a, const auto& b) {
            return a.first != b.first ? a.first > b.first : a.second < b.second;
        });
        for (const auto& [score, i] : by_score) {
            chunks.push_back(std::move(group.chunks[i]));
            chunks.back().score = score;
        }
    } else {
        for (const model::Relevance& rel : *scored) {
            chunks.push_back(std::move(group.chunks[rel.index]));
            chunks.back().score = rel.score;
        }
    }
    // Below the pool's worst, whatever that was: this reranker scores a bad
    // match negative, so a fixed negative floor would interleave with it.
    double floor = chunks.empty() ? 0.0 : chunks.back().score;
    for (size_t i = pool; i < group.chunks.size(); i++) {
        chunks.push_back(std::move(group.chunks[i]));
        chunks.back().score = floor - static_cast<double>(i - pool + 1);
    }
    group.chunks = std::move(chunks);

    if (r.files == Reranking::Files::Keep)
        return ok;

    // Files follow the chunks: the model read the text, the fuser only had
    // ranks. Files no surviving candidate sits in keep their fused order behind
    // them — dropping them would throw away recall the pool still holds.
    std::vector<File> files;
    files.reserve(group.files.size());
    std::unordered_set<std::string> placed;
    for (const Candidate& c : group.chunks)
        for (const store::Location& l : c.info.where)
            if (placed.insert(l.path).second)
                files.push_back({l.path, c.score});
    double under = group.chunks.empty() ? 0.0 : group.chunks.back().score;
    for (const File& f : group.files)
        if (placed.insert(f.path).second)
            files.push_back({f.path, under - static_cast<double>(files.size() + 1)});

    if (r.files == Reranking::Files::Fuse) {
        // The order above and the fuser's own, as two rankings of the same
        // files. A file whose evidence is spread thin over its chunks is what
        // the whole-file ranking is there for, and no chunk's rank carries it.
        std::unordered_map<std::string, double> score;
        for (size_t i = 0; i < files.size(); i++)
            score[files[i].path] += rrf(i);
        for (size_t i = 0; i < group.files.size(); i++)
            score[group.files[i].path] += rrf(i);
        files.clear();
        for (const auto& [path, s] : score)
            files.push_back({path, s});
        std::sort(files.begin(), files.end(), [](const File& a, const File& b) {
            return a.score != b.score ? a.score > b.score : a.path < b.path;
        });
    }
    group.files = std::move(files);
    return ok;
}

} // namespace

Status rerank(store::Db& db, std::string_view query, Ranked& ranked, const Reranking& r)
{
    // One call per side: the two answer different questions, and a shared
    // ranking would let the tests crowd out the code again (D23).
    if (auto s = rerank_side(db, query, ranked.code, r); !s)
        return s;
    if (!r.tests)
        return ok;
    return rerank_side(db, query, ranked.tests, r);
}

Result<Ranked> retrieve(store::Db& db, std::string_view branch, std::string_view query, size_t k)
{
    auto resolver = store::Resolver::make(db, branch);
    if (!resolver)
        return Err{resolver.error()};
    auto route = lexical_route(db, *resolver, query, k);
    if (!route)
        return Err{route.error()};
    return fuse({std::move(*route)}, k);
}

Result<Ranked> nearest(store::Db& db, std::string_view branch, std::span<const float> query, size_t k)
{
    if (auto s = all_embedded(db); !s)
        return Err{s.error()};
    auto resolver = store::Resolver::make(db, branch);
    if (!resolver)
        return Err{resolver.error()};
    auto scored = exact_scan(db, query);
    if (!scored)
        return Err{scored.error()};
    auto route = vector_route(*resolver, *scored, k);
    if (!route)
        return Err{route.error()};
    return fuse({std::move(*route)}, k);
}

Result<Ranked> nearest_ann(store::Db& db, std::string_view branch, const vector::Index& index,
                           std::span<const float> query, size_t k)
{
    if (auto s = all_embedded(db); !s)
        return Err{s.error()};
    auto resolver = store::Resolver::make(db, branch);
    if (!resolver)
        return Err{resolver.error()};
    auto route = ann_route(*resolver, index, query, k);
    if (!route)
        return Err{route.error()};
    return fuse({std::move(*route)}, k);
}

Result<Ranked> hybrid(store::Db& db, std::string_view branch, std::string_view text, const vector::Index& index,
                      std::span<const float> query, size_t k)
{
    if (auto s = all_embedded(db); !s)
        return Err{s.error()};
    auto resolver = store::Resolver::make(db, branch);
    if (!resolver)
        return Err{resolver.error()};
    auto words = lexical_route(db, *resolver, text, k);
    if (!words)
        return Err{words.error()};
    auto vectors = ann_route(*resolver, index, query, k);
    if (!vectors)
        return Err{vectors.error()};
    return fuse({std::move(*words), std::move(*vectors)}, k);
}

} // namespace match
