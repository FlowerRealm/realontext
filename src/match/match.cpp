#include "match/match.h"

#include <algorithm>
#include <unordered_map>

#include "lexical/lexical.h"

namespace match {

Result<Ranked> retrieve(store::Db& db, std::string_view branch, std::string_view query, size_t k)
{
    auto resolver = store::Resolver::make(db, branch);
    if (!resolver)
        return Err{resolver.error()};
    auto ranking = lexical::Ranking::search(db, query);
    if (!ranking)
        return Err{ranking.error()};

    // The ranking spans every branch, so it is read lazily until k chunks are
    // visible here. A fixed over-fetch fails when many versions of one hot
    // function crowd the top of the list and only one of them is on this branch.
    Ranked out;
    while (out.chunks.size() < k) {
        auto hit = ranking->next();
        if (!hit)
            return Err{hit.error()};
        if (!*hit)
            break;
        auto info = resolver->resolve((*hit)->chunk);
        if (!info)
            return Err{info.error()};
        if (!info->where.empty())
            out.chunks.push_back({(*hit)->chunk, (*hit)->score, std::move(*info)});
    }

    // A file scores as its best chunk: summing would reward size, not relevance.
    std::unordered_map<std::string, double> best;
    for (const Candidate& c : out.chunks)
        for (const store::Location& l : c.info.where)
            best.try_emplace(l.path, c.score);
    for (auto& [path, score] : best)
        out.files.push_back({path, score});
    std::sort(out.files.begin(), out.files.end(), [](const File& a, const File& b) {
        return a.score != b.score ? a.score > b.score : a.path < b.path;
    });
    return out;
}

} // namespace match
