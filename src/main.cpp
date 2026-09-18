// realontext CLI. Stages 1–3: index branches, embed their chunks, build the ANN
// index, query one branch (docs/roadmap.md).
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ingest/git.h"
#include "lexical/lexical.h"
#include "match/match.h"
#include "model/model.h"
#include "store/embed.h"
#include "serve/mcp.h"
#include "store/store.h"
#include "vector/vector.h"

namespace {

constexpr const char* usage = R"(usage:
  realontext index   --db FILE --git MIRROR [--rev REV --as BRANCH]...
                     without --rev: every branch of the mirror
  realontext embed   --db FILE [--endpoint URL] [--model NAME] [--task TASK] [--dim N]
                     [--max-bytes N] [--batch-bytes N] [--jobs N] [--rpm N] [--tpm N] [--dry-run 1]
                     key from JINA_API_KEY; interrupt and rerun to resume
  realontext build-index --db FILE [--connectivity N] [--expansion-add N]
                     [--scalar f32|f16|bf16|i8]      rebuilds from the stored vectors
  realontext query   --db FILE --branch BRANCH [--k N] [--route lexical|vector|ann|hybrid]
                     [--rerank 1] [--rerank-endpoint URL] [--rerank-model NAME] [--rerank-pool N]
                     [--rerank-max-bytes N] [--rerank-rpm N] [--rerank-tpm N]
                     query on stdin, JSON on stdout; rerank key from RERANK_API_KEY
  realontext mcp     --db FILE --branch BRANCH [--route lexical|ann|hybrid] [--k N] [--full-text N]
                     [--rerank 1] and the --rerank-* options of query
                     MCP over stdio, one JSON object per line
  realontext symbols --db FILE --branch BRANCH           function names, one per line
)";

struct Args {
    std::string cmd;
    std::map<std::string, std::vector<std::string>> opts;

    const std::string* one(const std::string& key) const
    {
        auto it = opts.find(key);
        return it == opts.end() || it->second.empty() ? nullptr : &it->second.back();
    }
    const std::vector<std::string>& many(const std::string& key) const
    {
        static const std::vector<std::string> none;
        auto it = opts.find(key);
        return it == opts.end() ? none : it->second;
    }
};

bool parse_args(int argc, char** argv, Args& a)
{
    if (argc < 2)
        return false;
    a.cmd = argv[1];
    for (int i = 2; i < argc; i += 2) {
        std::string key = argv[i];
        if (key.rfind("--", 0) != 0 || i + 1 >= argc)
            return false;
        a.opts[key.substr(2)].push_back(argv[i + 1]);
    }
    return true;
}

size_t number(const Args& a, const std::string& key, size_t fallback)
{
    const std::string* v = a.one(key);
    return v ? std::stoul(*v) : fallback;
}

std::string key_from_env(const char* name)
{
    const char* k = std::getenv(name);
    return k ? k : "";
}

int fail(const std::string& msg)
{
    std::fprintf(stderr, "realontext: %s\n", msg.c_str());
    return 1;
}

Result<store::Db> open_db(const Args& a)
{
    const std::string* path = a.one("db");
    if (!path)
        return Err{"--db is required"};
    auto db = store::Db::open(*path);
    if (!db)
        return db;
    if (auto s = lexical::attach(*db); !s)
        return Err{s.error()};
    return db;
}

int cmd_index(const Args& a, store::Db& db)
{
    const std::string* git = a.one("git");
    if (!git)
        return fail("--git is required");
    auto repo = ingest::Repo::open(*git);
    if (!repo)
        return fail(repo.error());

    const auto& revs = a.many("rev");
    const auto& names = a.many("as");
    if (revs.size() != names.size())
        return fail("every --rev needs an --as");
    std::vector<ingest::Ref> targets;
    if (revs.empty()) {
        auto refs = repo->branches();
        if (!refs)
            return fail(refs.error());
        targets = std::move(*refs);
    }
    for (size_t i = 0; i < revs.size(); i++) {
        auto id = repo->resolve(revs[i]);
        if (!id)
            return fail(id.error());
        targets.push_back({names[i], *id});
    }

    for (const ingest::Ref& t : targets) {
        auto at = store::branch_commit(db, t.name);
        if (!at)
            return fail(at.error());
        if (*at && **at == t.commit) {
            std::fprintf(stderr, "[index] %s @%s unchanged\n", t.name.c_str(), hex(t.commit).substr(0, 12).c_str());
            continue;
        }
        auto started = std::chrono::steady_clock::now();
        auto files = repo->tree(t.commit);
        if (!files)
            return fail(files.error());
        auto stats = store::index_branch(db, t.name, t.commit, *files,
                                         [&](const Oid& blob) { return repo->text(blob); });
        if (!stats)
            return fail(t.name + ": " + stats.error());
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::fprintf(stderr, "[index] %s @%s files=%zu new_files=%zu parsed=%zu chunks=%zu new_chunks=%zu %.1fs\n",
                     t.name.c_str(), hex(t.commit).substr(0, 12).c_str(), stats->files, stats->new_files,
                     stats->parsed, stats->chunks, stats->new_chunks, secs);
    }
    auto started = std::chrono::steady_clock::now();
    if (auto s = lexical::sync(db); !s)
        return fail(s.error());
    std::fprintf(stderr, "[index] fts sync %.1fs\n",
                 std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
    return 0;
}

int cmd_embed(const Args& a, store::Db& db)
{
    auto str = [&](const char* key, const char* fallback) { return a.one(key) ? *a.one(key) : std::string(fallback); };
    store::Fingerprint fp{{str("endpoint", "https://api.jina.ai/v1/embeddings"), str("model", "jina-embeddings-v4"),
                           str("task", "code"), static_cast<uint32_t>(number(a, "dim", 1024))},
                          number(a, "max-bytes", 16384)};
    if (a.one("dry-run")) {
        auto p = store::pending(db, fp.max_bytes);
        if (!p)
            return fail(p.error());
        std::fprintf(stderr, "[embed] pending chunks=%zu bytes=%zu est_tokens=%.0f\n", p->chunks, p->bytes,
                     static_cast<double>(p->bytes) / model::bytes_per_token);
        return 0;
    }

    model::Limiter limiter(static_cast<double>(number(a, "rpm", 100)), static_cast<double>(number(a, "tpm", 100000)),
                           model::Clock::real());
    model::Embedder embedder(fp.embedding, key_from_env("JINA_API_KEY"), model::http_post(), limiter,
                             model::Clock::real());
    auto started = std::chrono::steady_clock::now();
    auto last = started;
    auto done = store::embed(db, fp, embedder, {number(a, "batch-bytes", 65536), number(a, "jobs", 4)},
                             [&](const store::EmbedProgress& p, size_t total) {
                                 auto now = std::chrono::steady_clock::now();
                                 if (now - last < std::chrono::seconds(10) && p.chunks < total)
                                     return;
                                 last = now;
                                 double secs = std::chrono::duration<double>(now - started).count();
                                 std::fprintf(stderr, "[embed] %zu/%zu chunks tokens=%llu %.0f tok/s %.0fs\n",
                                              p.chunks, total, static_cast<unsigned long long>(p.tokens),
                                              static_cast<double>(p.tokens) / secs, secs);
                             });
    if (!done)
        return fail(done.error());
    return 0;
}

int cmd_build_index(const Args& a, store::Db& db)
{
    auto fp = store::pinned(db);
    if (!fp)
        return fail(fp.error());
    vector::Params params{number(a, "connectivity", 16), number(a, "expansion-add", 128),
                          a.one("scalar") ? *a.one("scalar") : "f32"};

    auto started = std::chrono::steady_clock::now();
    auto last = started;
    auto built = vector::build(db, fp->embedding.dim, params, *a.one("db"), [&](size_t added, size_t total) {
        auto now = std::chrono::steady_clock::now();
        if (now - last < std::chrono::seconds(10) && added < total)
            return;
        last = now;
        std::fprintf(stderr, "[build-index] %zu/%zu vectors %.0fs\n", added, total,
                     std::chrono::duration<double>(now - started).count());
    });
    if (!built)
        return fail(built.error());
    std::fprintf(stderr, "[build-index] %zu vectors M=%zu ef_construction=%zu %s %.1f MB %.1fs\n", built->vectors,
                 params.connectivity, params.expansion_add, params.scalar.c_str(),
                 static_cast<double>(built->bytes) / 1e6,
                 std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
    return 0;
}

// Ranking one query the way --route asks for. Holds the embedder and the ANN
// index, so a server that answers many queries loads them once.
class Route {
public:
    static Result<Route> make(const Args& a, store::Db& db, std::string branch, size_t k)
    {
        Route r(db, std::move(branch), k, a.one("route") ? *a.one("route") : "lexical");
        if (r.route_ == "lexical")
            return r;
        if (r.route_ != "vector" && r.route_ != "ann" && r.route_ != "hybrid")
            return Err{"--route is lexical, vector, ann or hybrid"};
        auto fp = store::pinned(db);
        if (!fp)
            return Err{fp.error()};
        r.limiter_ = std::make_unique<model::Limiter>(1e9, 1e12, model::Clock::real()); // the query, alone
        r.embedder_ = std::make_unique<model::Embedder>(fp->embedding, key_from_env("JINA_API_KEY"),
                                                        model::http_post(), *r.limiter_, model::Clock::real());
        if (r.route_ == "ann" || r.route_ == "hybrid") {
            auto index = vector::Index::open(db, *a.one("db"));
            if (!index)
                return Err{index.error()};
            r.index_ = std::make_unique<vector::Index>(std::move(*index));
        }
        return r;
    }

    // Reranking is orthogonal to the route: any of them can be handed to the
    // model, which is how the rerank delta on each is measured (D27).
    Status with_rerank(const Args& a)
    {
        if (!a.one("rerank"))
            return ok;
        std::string key = key_from_env("RERANK_API_KEY");
        if (key.empty())
            return Err{"--rerank needs RERANK_API_KEY"};
        model::Rerank config{a.one("rerank-endpoint") ? *a.one("rerank-endpoint")
                                                      : "https://openrouter.ai/api/v1/rerank",
                             a.one("rerank-model") ? *a.one("rerank-model") : "voyageai/rerank-2.5-lite"};
        rerank_limiter_ = std::make_unique<model::Limiter>(static_cast<double>(number(a, "rerank-rpm", 100)),
                                                          static_cast<double>(number(a, "rerank-tpm", 2000000)),
                                                          model::Clock::real());
        reranker_ = std::make_unique<model::Reranker>(std::move(config), std::move(key), model::http_post(),
                                                      *rerank_limiter_, model::Clock::real());
        rerank_ = match::Reranking{reranker_.get(), number(a, "rerank-pool", 50),
                                   number(a, "rerank-max-bytes", 16384)};
        return ok;
    }

    Result<match::Ranked> operator()(const std::string& query) const
    {
        auto ranked = retrieve(query);
        if (!ranked || !rerank_)
            return ranked;
        if (auto s = match::rerank(db_, query, *ranked, *rerank_); !s)
            return Err{s.error()};
        return ranked;
    }

private:
    Route(store::Db& db, std::string branch, size_t k, std::string route)
        : db_(db), branch_(std::move(branch)), k_(k), route_(std::move(route)) {}

    Result<match::Ranked> retrieve(const std::string& query) const
    {
        if (route_ == "lexical")
            return match::retrieve(db_, branch_, query, k_);
        auto v = embedder_->embed({query}, model::Role::Query);
        if (!v)
            return Err{v.error()};
        if (route_ == "vector")
            return match::nearest(db_, branch_, v->data, k_);
        if (route_ == "ann")
            return match::nearest_ann(db_, branch_, *index_, v->data, k_);
        return match::hybrid(db_, branch_, query, *index_, v->data, k_);
    }

    store::Db& db_;
    std::string branch_;
    size_t k_;
    std::string route_;
    std::unique_ptr<model::Limiter> limiter_;
    std::unique_ptr<model::Embedder> embedder_;
    std::unique_ptr<vector::Index> index_;
    std::unique_ptr<model::Limiter> rerank_limiter_;
    std::unique_ptr<model::Reranker> reranker_;
    std::optional<match::Reranking> rerank_;
};

int cmd_query(const Args& a, store::Db& db)
{
    const std::string* branch = a.one("branch");
    if (!branch)
        return fail("--branch is required");
    std::string query(std::istreambuf_iterator<char>(std::cin), {});

    auto route = Route::make(a, db, *branch, number(a, "k", 200));
    if (!route)
        return fail(route.error());
    if (auto s = route->with_rerank(a); !s)
        return fail(s.error());
    auto ranked = (*route)(query);
    if (!ranked)
        return fail(ranked.error());

    auto group = [](const match::Group& g) {
        nlohmann::json chunks = nlohmann::json::array();
        for (const match::Candidate& c : g.chunks) {
            nlohmann::json where = nlohmann::json::array();
            for (const store::Location& l : c.info.where)
                where.push_back({{"path", l.path}, {"start_line", l.start_line}, {"end_line", l.end_line}});
            chunks.push_back({{"chunk", c.chunk},
                              {"score", c.score},
                              {"kind", c.info.kind == parse::Kind::Function ? "function" : "container"},
                              {"symbol", c.info.symbol},
                              {"locations", std::move(where)}});
        }
        nlohmann::json files = nlohmann::json::array();
        for (const match::File& f : g.files)
            files.push_back({{"path", f.path}, {"score", f.score}});
        return nlohmann::json{{"chunks", std::move(chunks)}, {"files", std::move(files)}};
    };
    // code: where the change likely goes. tests: what exercises it, and did not catch the fault.
    std::cout << nlohmann::json{{"code", group(ranked->code)}, {"tests", group(ranked->tests)}}.dump() << "\n";
    return 0;
}

int cmd_mcp(const Args& a, store::Db& db)
{
    const std::string* branch = a.one("branch");
    if (!branch)
        return fail("--branch is required");
    auto route = Route::make(a, db, *branch, number(a, "k", 200));
    if (!route)
        return fail(route.error());
    if (auto s = route->with_rerank(a); !s)
        return fail(s.error());
    serve::Options opt{*branch, number(a, "full-text", 20)};
    std::fprintf(stderr, "[mcp] %s branch=%s route=%s rerank=%s\n", a.one("db")->c_str(), branch->c_str(),
                 a.one("route") ? a.one("route")->c_str() : "lexical", a.one("rerank") ? "on" : "off");
    serve::Retrieve retrieve = [&](const std::string& query) { return (*route)(query); };
    if (auto s = serve::mcp(db, opt, retrieve, std::cin, std::cout); !s)
        return fail(s.error());
    return 0;
}

int cmd_symbols(const Args& a, store::Db& db)
{
    const std::string* branch = a.one("branch");
    if (!branch)
        return fail("--branch is required");
    auto names = store::function_names(db, *branch);
    if (!names)
        return fail(names.error());
    for (const std::string& n : *names)
        std::cout << n << "\n";
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    Args a;
    if (!parse_args(argc, argv, a) || (a.cmd != "index" && a.cmd != "embed" && a.cmd != "build-index" &&
                                       a.cmd != "query" && a.cmd != "mcp" && a.cmd != "symbols")) {
        std::fputs(usage, stderr);
        return 2;
    }
    auto db = open_db(a);
    if (!db)
        return fail(db.error());
    if (a.cmd == "index")
        return cmd_index(a, *db);
    if (a.cmd == "embed")
        return cmd_embed(a, *db);
    if (a.cmd == "build-index")
        return cmd_build_index(a, *db);
    if (a.cmd == "query")
        return cmd_query(a, *db);
    if (a.cmd == "mcp")
        return cmd_mcp(a, *db);
    return cmd_symbols(a, *db);
}
