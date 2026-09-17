// realontext CLI. Stages 1–2: index branches, embed their chunks, query one of them (docs/roadmap.md).
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ingest/git.h"
#include "lexical/lexical.h"
#include "match/match.h"
#include "model/model.h"
#include "store/embed.h"
#include "store/store.h"

namespace {

constexpr const char* usage = R"(usage:
  realontext index   --db FILE --git MIRROR [--rev REV --as BRANCH]...
                     without --rev: every branch of the mirror
  realontext embed   --db FILE [--endpoint URL] [--model NAME] [--task TASK] [--dim N]
                     [--max-bytes N] [--batch-bytes N] [--jobs N] [--rpm N] [--tpm N] [--dry-run 1]
                     key from JINA_API_KEY; interrupt and rerun to resume
  realontext query   --db FILE --branch BRANCH [--k N] [--route lexical|vector]
                     query on stdin, JSON on stdout
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

std::string key_from_env()
{
    const char* k = std::getenv("JINA_API_KEY");
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
    model::Embedder embedder(fp.embedding, key_from_env(), model::http_post(), limiter, model::Clock::real());
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

Result<match::Ranked> rank(const Args& a, store::Db& db, const std::string& branch, const std::string& query,
                           size_t k)
{
    std::string route = a.one("route") ? *a.one("route") : "lexical";
    if (route == "lexical")
        return match::retrieve(db, branch, query, k);
    if (route != "vector")
        return Err{"--route is lexical or vector"};
    auto fp = store::pinned(db);
    if (!fp)
        return Err{fp.error()};
    model::Limiter limiter(1e9, 1e12, model::Clock::real()); // one request
    model::Embedder embedder(fp->embedding, key_from_env(), model::http_post(), limiter, model::Clock::real());
    auto v = embedder.embed({query}, model::Role::Query);
    if (!v)
        return Err{v.error()};
    return match::nearest(db, branch, v->data, k);
}

int cmd_query(const Args& a, store::Db& db)
{
    const std::string* branch = a.one("branch");
    if (!branch)
        return fail("--branch is required");
    size_t k = number(a, "k", 200);
    std::string query(std::istreambuf_iterator<char>(std::cin), {});

    auto ranked = rank(a, db, *branch, query, k);
    if (!ranked)
        return fail(ranked.error());

    nlohmann::json chunks = nlohmann::json::array();
    for (const match::Candidate& c : ranked->chunks) {
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
    for (const match::File& f : ranked->files)
        files.push_back({{"path", f.path}, {"score", f.score}});
    std::cout << nlohmann::json{{"chunks", std::move(chunks)}, {"files", std::move(files)}}.dump() << "\n";
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
    if (!parse_args(argc, argv, a) || (a.cmd != "index" && a.cmd != "embed" && a.cmd != "query" && a.cmd != "symbols")) {
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
    if (a.cmd == "query")
        return cmd_query(a, *db);
    return cmd_symbols(a, *db);
}
