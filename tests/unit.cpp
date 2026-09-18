// Chunk names and tokens for each language. Names are what function-level
// scoring compares, so a wrong one silently caps recall. Then the embedding
// path with a fake clock and a fake provider: no network, no sleeping.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "ingest/filter.h"
#include "lexical/lexical.h"
#include "lexical/tokenizer.h"
#include "match/match.h"
#include "model/model.h"
#include "parse/chunk.h"
#include "serve/mcp.h"
#include "store/embed.h"
#include "vector/vector.h"

#include <nlohmann/json.hpp>

namespace {

int failures = 0;

void expect(bool cond, const std::string& what)
{
    if (!cond) {
        std::fprintf(stderr, "FAIL %s\n", what.c_str());
        failures++;
    }
}

std::set<std::string> names(parse::Lang lang, const std::string& src, parse::Kind kind)
{
    std::set<std::string> out;
    for (const parse::Chunk& c : parse::chunk(lang, src))
        if (c.kind == kind)
            out.insert(c.symbol);
    return out;
}

void functions(parse::Lang lang, const std::string& src, std::set<std::string> want, const char* label)
{
    auto got = names(lang, src, parse::Kind::Function);
    std::string list;
    for (const auto& n : got)
        list += " " + n;
    expect(got == want, std::string(label) + " functions:" + list);
}

std::vector<std::string> tokens(std::string_view text, bool document)
{
    std::vector<std::string> out;
    lexical::tokenize(text, document, [&](std::string_view t, bool, size_t, size_t) {
        out.emplace_back(t);
        return true;
    });
    return out;
}

struct FakeClock {
    std::chrono::steady_clock::time_point t{};
    double slept = 0;
    model::Clock clock()
    {
        return {[this] { return t; },
                [this](std::chrono::duration<double> d) {
                    slept += d.count();
                    t += std::chrono::duration_cast<std::chrono::steady_clock::duration>(d);
                }};
    }
};

// A provider whose vector for a text is one-hot on the first of "alpha",
// "beta", "gamma" it contains, over 3 dimensions.
model::Response fake_provider(const model::Request& r)
{
    auto req = nlohmann::json::parse(r.body);
    nlohmann::json data = nlohmann::json::array();
    size_t i = 0;
    for (const auto& text : req["input"]) {
        std::string t = text.get<std::string>();
        std::vector<float> v{0, 0, 0};
        v[t.find("alpha") != std::string::npos ? 0 : t.find("beta") != std::string::npos ? 1 : 2] = 2;
        data.push_back({{"index", i++}, {"embedding", v}});
    }
    return {200, nlohmann::json{{"data", data}, {"usage", {{"total_tokens", 10 * i}}}}.dump()};
}

// Fusing the two routes (D26). What matters is that neither route can veto the
// other's find, and that being found twice beats being found once.
void hybrid(store::Db& db, const vector::Index& index, std::span<const float> beta)
{
    auto agree = match::hybrid(db, "main", "beta", index, beta, 3);
    expect(agree && agree->code.chunks.size() == 3 && agree->code.chunks[0].info.symbol == "beta" &&
               agree->tests.chunks.size() == 1 && agree->tests.chunks[0].info.symbol == "test_beta",
           "a chunk both routes rank first stays first");
    std::set<std::string> found;
    for (const match::Candidate& c : agree->code.chunks)
        found.insert(c.info.symbol);
    expect(found == std::set<std::string>{"alpha", "beta", "gamma"},
           "chunks only the vector route reached are in the fused ranking, not filtered out by the words");

    // "gamma" is the lexical route's only hit and the vector route's last;
    // "beta" is the vector route's first and no hit at all for the words.
    auto disagree = match::hybrid(db, "main", "gamma", index, beta, 3);
    expect(disagree && disagree->code.chunks.size() == 3 && disagree->code.chunks[0].info.symbol == "gamma" &&
               disagree->code.chunks[1].info.symbol == "beta",
           "two routes ranking a chunk beats one route ranking it first");
    expect(disagree && disagree->tests.chunks.size() == 1 && disagree->tests.chunks[0].info.symbol == "test_beta" &&
               disagree->tests.files.size() == 1 && disagree->tests.files[0].path == "tests/t.c",
           "the sides stay apart through fusion");
}

// The ANN index over a database store/ has finished embedding. It is a derived
// cache, so what matters is that it agrees with the exact scan and that a
// database embedded further refuses to be searched through a stale one (D24).
void ann(store::Db& db, const std::string& path, const store::Fingerprint& fp, model::Embedder& embedder)
{
    const std::string index = vector::index_path(path);
    std::filesystem::remove(index);
    auto built = vector::build(db, fp.embedding.dim, {}, path, [](size_t, size_t) {});
    expect(built && built->vectors == 4 && std::filesystem::exists(index), "the index holds every stored vector");

    std::vector<float> query{0, 1, 0};
    auto opened = vector::Index::open(db, path);
    expect(opened && opened->size() == 4 && opened->dim() == fp.embedding.dim, "the index opens against its database");
    auto ranked = match::nearest_ann(db, "main", *opened, query, 2);
    expect(ranked && ranked->code.chunks.size() == 2 && ranked->code.chunks[0].info.symbol == "beta" &&
               ranked->tests.chunks.size() == 1 && ranked->tests.chunks[0].info.symbol == "test_beta",
           "the ANN route ranks like the exact scan");

    hybrid(db, *opened, query);

    std::string more = "int delta() { return 4; }\n";
    auto grown = store::index_branch(db, "main", Oid{8}, {{"y.c", Oid{3}}},
                                     [&](const Oid&) { return Result<std::string>(more); });
    expect(grown && grown->new_chunks == 1, "one more chunk to embed");
    expect(bool(store::embed(db, fp, embedder, {1, 1}, [](const store::EmbedProgress&, size_t) {})), "embed it");
    auto stale = vector::Index::open(db, path);
    expect(!stale && stale.error().find("build-index") != std::string::npos,
           "an index built before the last embedding is refused, not silently short");
    std::filesystem::remove(index);
}

// Reranking (D27). It reorders what a route assembled and nothing else: the
// pool is the ceiling, candidates past it keep the fused order, and a provider
// that fails is an error rather than a quiet fall back to that order.
void reranking()
{
    expect(!model::parse_rerank(R"({"data":[{"index":0,"relevance_score":0.5}]})", 2),
           "a response scoring fewer documents than were sent is an error");
    expect(!model::parse_rerank(R"({"data":[{"index":0,"relevance_score":0.5},{"index":0,"relevance_score":0.1}]})", 2),
           "a document scored twice is an error");
    auto proxied = model::parse_rerank(
        R"({"results":[{"index":0,"relevance_score":0.2,"document":{"text":"x"}}],"usage":{"total_tokens":3}})", 1);
    expect(proxied && proxied->size() == 1 && proxied->at(0).index == 0,
           "a proxy that names the array results is read like the provider's own data");
    auto order = model::parse_rerank(
        R"({"data":[{"index":0,"relevance_score":0.1},{"index":1,"relevance_score":0.9}],"usage":{"total_tokens":8}})", 2);
    expect(order && order->size() == 2 && order->at(0).index == 1 && order->at(1).index == 0,
           "the order comes from the scores, not from the provider's own sorting");

    auto path = std::filesystem::temp_directory_path() / "realontext-rerank.db";
    std::filesystem::remove(path);
    auto db = store::Db::open(path.string());
    expect(bool(db) && bool(lexical::attach(*db)), "open a database for reranking");
    std::string a = "int alpha() { return 1; }\n";
    std::string b = "int beta() { return 2; }\nint beta_two() { return 2; }\n";
    std::string t = "int test_alpha() { return alpha() == 1; }\n";
    auto indexed = store::index_branch(
        *db, "main", Oid{9}, {{"a.c", Oid{1}}, {"b.c", Oid{2}}, {"tests/t.c", Oid{3}}},
        [&](const Oid& id) { return Result<std::string>(id[0] == 1 ? a : id[0] == 2 ? b : t); });
    expect(indexed && indexed->chunks == 4, "four chunks across two code files and a test");
    expect(bool(lexical::sync(*db)), "sync lexical index");

    auto fused = match::retrieve(*db, "main", "alpha beta beta_two", 10);
    expect(fused && fused->code.chunks.size() == 3 && fused->code.files.size() == 2,
           "three code candidates over two files before reranking");
    std::vector<std::string> before;
    for (const match::Candidate& c : fused->code.chunks)
        before.push_back(c.info.symbol);

    // Scores the last document sent best, so a reranked pool comes back exactly
    // reversed and a ranking left alone is visible as one.
    std::vector<nlohmann::json> requests;
    model::Post reversing = [&](const model::Request& r) {
        auto req = nlohmann::json::parse(r.body);
        requests.push_back(req);
        nlohmann::json data = nlohmann::json::array();
        size_t n = req["documents"].size();
        for (size_t i = 0; i < n; i++)
            data.push_back({{"index", i}, {"relevance_score", double(i + 1) / double(n)}});
        return model::Response{200, nlohmann::json{{"data", data}, {"usage", {{"total_tokens", 4 * n}}}}.dump()};
    };
    FakeClock fc;
    model::Limiter open(1e9, 1e12, fc.clock());
    model::Reranker reranker({"http://fake/v1/rerank", "r"}, "", reversing, open, fc.clock());

    match::Ranked ranked = *fused;
    match::Reranking config{&reranker, 2, 8};
    expect(bool(match::rerank(*db, "alpha beta beta_two", ranked, config)), "rerank the ranking");
    expect(requests.size() == 2 && requests[0]["query"] == "alpha beta beta_two" &&
               requests[0]["documents"].size() == 2 && requests[1]["documents"].size() == 1,
           "one call per side, each holding at most the pool");
    expect(requests[0]["documents"][0].get<std::string>().size() <= 8,
           "a document is cut at the reranker's own limit");

    expect(ranked.code.chunks.size() == 3 && ranked.code.chunks[0].info.symbol == before[1] &&
               ranked.code.chunks[1].info.symbol == before[0],
           "the pool comes back in the model's order");
    expect(ranked.code.chunks[2].info.symbol == before[2] && ranked.code.chunks[2].score < 0,
           "a candidate past the pool keeps its place and says it was never read");
    bool falling = true;
    for (size_t i = 1; i < ranked.code.chunks.size(); i++)
        falling = falling && ranked.code.chunks[i].score <= ranked.code.chunks[i - 1].score;
    expect(falling, "scores never rise down the ranking");
    expect(ranked.code.files.size() == 2 && ranked.code.files[0].path == ranked.code.chunks[0].info.where[0].path,
           "files follow the reranked chunks");
    expect(ranked.tests.chunks.size() == 1 && ranked.tests.chunks[0].info.symbol == "test_alpha",
           "a side with one candidate is reranked, not skipped");

    match::Ranked empty;
    requests.clear();
    expect(bool(match::rerank(*db, "alpha", empty, config)) && requests.empty(),
           "nothing to rerank asks the provider nothing");

    model::Post refusing = [](const model::Request&) { return model::Response{400, "bad request"}; };
    model::Reranker failing({"http://fake/v1/rerank", "r"}, "", refusing, open, fc.clock());
    match::Ranked doomed = *fused;
    match::Reranking broken{&failing, 2, 8};
    auto failed = match::rerank(*db, "alpha", doomed, broken);
    expect(!failed && failed.error().find("rerank") != std::string::npos,
           "a provider that refuses is an error, not the fused order handed back as if reranked");
    std::filesystem::remove(path);
}

// The MCP transport with a stub ranking: what is tested here is the protocol
// shape an agent depends on, not the retrieval behind it.
void mcp(store::Db& db)
{
    match::Ranked stub;
    stub.code.chunks.push_back({1, 0.5, {parse::Kind::Function, "beta", {{"x.c", 2, 2}}}});
    stub.code.files.push_back({"x.c", 0.5});
    serve::Retrieve retrieve = [&](const std::string& query) -> Result<match::Ranked> {
        if (query == "boom")
            return Err{"no vector index has been built"};
        return stub;
    };

    std::istringstream in(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26"}})"
        "\n"
        R"({"jsonrpc":"2.0","method":"notifications/initialized"})"
        "\n"
        R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})"
        "\n"
        R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"codebase-retrieval","arguments":{"query":"beta"}}})"
        "\n"
        R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"codebase-retrieval","arguments":{}}})"
        "\n"
        R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"codebase-retrieval","arguments":{"query":"boom"}}})"
        "\n"
        R"({"jsonrpc":"2.0","id":6,"method":"resources/list"})"
        "\n"
        "not json\n");
    std::ostringstream out;
    expect(bool(serve::mcp(db, {"main", 1}, retrieve, in, out)), "the transport runs to end of input");

    std::vector<nlohmann::json> replies;
    std::istringstream lines(out.str());
    for (std::string line; std::getline(lines, line);)
        replies.push_back(nlohmann::json::parse(line));
    expect(replies.size() == 7, "a notification draws no reply, everything else draws one");
    expect(replies[0]["result"]["protocolVersion"] == "2025-03-26" &&
               replies[0]["result"]["capabilities"].contains("tools"),
           "initialize answers in the version the client asked for");
    expect(replies[1]["result"]["tools"].size() == 1 &&
               replies[1]["result"]["tools"][0]["name"] == "codebase-retrieval" &&
               replies[1]["result"]["tools"][0]["inputSchema"]["required"] == nlohmann::json::array({"query"}),
           "one tool, query its only required argument");

    const auto& called = replies[2]["result"];
    const auto& first = called["structuredContent"]["code"]["chunks"][0];
    expect(first["symbol"] == "beta" && first["text"] == *store::chunk_text(db, 1) &&
               called["content"][0]["type"] == "text",
           "a candidate that fits carries its source");
    expect(called["structuredContent"]["tests"]["chunks"].empty(), "the groups stay apart on the wire");
    expect(replies[3]["result"]["isError"] == true, "a call without a query is the tool's error");
    expect(replies[4]["result"]["isError"] == true &&
               replies[4]["result"]["content"][0]["text"] == "no vector index has been built",
           "a failed retrieval reaches the agent as text, not a dropped connection");
    expect(replies[5]["error"]["code"] == -32601, "an unknown method is method-not-found");
    expect(replies[6]["error"]["code"] == -32700 && replies[6]["id"].is_null(),
           "a line that is not JSON is answered, and the loop survives it");
}

void embedding()
{
    FakeClock fc;
    model::Limiter rpm(60, 1e9, fc.clock());
    for (int i = 0; i < 60; i++)
        rpm.acquire(1);
    expect(fc.slept == 0, "a full minute of requests passes without waiting");
    rpm.acquire(1);
    expect(fc.slept == 1, "request 61 waits one second at 60 rpm");

    FakeClock tc;
    model::Limiter tpm(1e9, 6000, tc.clock());
    tpm.acquire(6000);
    tpm.settle(6000, 6100);
    tpm.acquire(100);
    expect(tc.slept == 2, "billed tokens beyond the estimate are paid back in waiting");

    auto parsed = model::parse(
        R"({"data":[{"index":1,"embedding":[0,4]},{"index":0,"embedding":[3,4]}],"usage":{"total_tokens":7}})", 2, 2);
    expect(parsed && parsed->data == std::vector<float>{0.6f, 0.8f, 0, 1} && parsed->tokens == 7,
           "vectors land at their index, normalised");
    expect(!model::parse(R"({"data":[{"index":0,"embedding":[1,0]}]})", 2, 2), "a missing vector is an error");

    model::Embedding config{"http://fake/v1/embeddings", "m", "code", 3};
    std::vector<long> statuses{429, 503, 200};
    std::vector<std::string> bodies;
    model::Post flaky = [&](const model::Request& r) {
        bodies.push_back(r.body);
        long status = statuses[bodies.size() - 1];
        return status == 200 ? fake_provider(r) : model::Response{status, "busy"};
    };
    FakeClock ec;
    model::Limiter open(1e9, 1e12, ec.clock());
    model::Embedder retrying(config, "", flaky, open, ec.clock());
    auto v = retrying.embed({"beta"}, model::Role::Query);
    expect(v && bodies.size() == 3 && v->data == std::vector<float>{0, 1, 0}, "429 and 5xx are retried");
    auto req = nlohmann::json::parse(bodies[0]);
    expect(req["task"] == "code.query" && req["dimensions"] == 3 && req["model"] == "m", "request names task role and dim");

    size_t calls = 0;
    model::Post denied = [&](const model::Request&) {
        calls++;
        return model::Response{401, "bad key"};
    };
    model::Embedder refused(config, "k", denied, open, ec.clock());
    expect(!refused.embed({"x"}, model::Role::Passage) && calls == 1, "401 fails on the first attempt");

    expect(store::embed_text("S", "ab\xc3\xa9", 5) == "S\nab", "truncation never splits a UTF-8 sequence");

    auto path = std::filesystem::temp_directory_path() / "realontext-unit.db";
    std::filesystem::remove(path);
    auto db = store::Db::open(path.string());
    expect(bool(db), "open database");
    expect(bool(lexical::attach(*db)), "attach lexical index");
    std::string src = "int alpha() { return 1; }\nint beta() { return 2; }\nint gamma() { return 3; }\n";
    std::string test_src = "int test_beta() { return beta() == 2; }\n";
    auto indexed = store::index_branch(*db, "main", Oid{9}, {{"x.c", Oid{1}}, {"tests/t.c", Oid{2}}},
                                       [&](const Oid& id) { return Result<std::string>(id[0] == 1 ? src : test_src); });
    expect(indexed && indexed->chunks == 4, "four chunks to embed");
    expect(bool(lexical::sync(*db)), "sync lexical index");
    auto words = match::retrieve(*db, "main", "beta", 10);
    expect(words && words->code.chunks.size() == 1 && words->code.chunks[0].info.symbol == "beta" &&
               words->tests.chunks.size() == 1 && words->tests.chunks[0].info.symbol == "test_beta" &&
               words->tests.files[0].path == "tests/t.c",
           "lexical ranking returns code and tests as separate groups");

    store::Fingerprint fp{config, 1024};
    store::Fingerprint other = fp;
    other.embedding.model = "n";
    expect(store::pin(*db, other) && store::pin(*db, fp) && store::pinned(*db)->embedding.model == "m",
           "the fingerprint follows the request while nothing is embedded");
    std::string first_ok;
    model::Post second_fails = [&](const model::Request& r) {
        if (first_ok.empty()) {
            first_ok = r.body;
            return fake_provider(r);
        }
        return model::Response{400, "bad input"};
    };
    model::Embedder broken(config, "", second_fails, open, ec.clock());
    auto partial = store::embed(*db, fp, broken, {1, 1}, [](const store::EmbedProgress&, size_t) {});
    expect(!partial && *store::unembedded(*db) == 3, "a failed run keeps what it was paid for");

    model::Embedder working(config, "", fake_provider, open, ec.clock());
    auto resumed = store::embed(*db, fp, working, {1, 2}, [](const store::EmbedProgress&, size_t) {});
    expect(resumed && resumed->chunks == 3 && resumed->tokens == 30, "rerun embeds only what is pending");

    auto mixed = store::embed(*db, other, working, {1, 1}, [](const store::EmbedProgress&, size_t) {});
    expect(!mixed && mixed.error().find("embed.model") == 0, "a different model is refused by name");

    std::vector<float> query{0, 1, 0};
    auto ranked = match::nearest(*db, "main", query, 2);
    expect(ranked && ranked->code.chunks.size() == 2 && ranked->code.chunks[0].info.symbol == "beta" &&
               ranked->code.files.size() == 1 && ranked->code.files[0].path == "x.c" &&
               ranked->tests.chunks.size() == 1 && ranked->tests.chunks[0].info.symbol == "test_beta",
           "nearest chunks per side, files by best chunk");

    ann(*db, path.string(), fp, working);
    mcp(*db);
    reranking();

    expect(bool(store::set_meta_int(*db, "embed.input_version", store::input_version + 1)), "bump stored version");
    db = Err{"closed"};
    auto reopened = store::Db::open(path.string());
    expect(!reopened && reopened.error().find("input version") != std::string::npos,
           "vectors of another input version refuse to open");
    std::filesystem::remove(path);
}

} // namespace

int main()
{
    using parse::Lang;

    expect(parse::normalize("Foo<T>::bar(int) -> R") == "Foo::bar", "normalize generics");
    expect(parse::normalize("a.b#c") == "a::b::c", "normalize separators");
    expect(parse::normalize("*ptr") == "ptr", "normalize pointer");

    functions(Lang::Cpp, R"(
namespace DB {
class Foo {
    // doc
    void a() {}
    int x;
};
template <typename T>
void Foo::b(T t) {}
}
int main() { return 0; }
)", {"DB::Foo::a", "DB::Foo::b", "main"}, "cpp");
    expect(names(Lang::Cpp, "namespace DB { class Foo { void a() {} int x; }; }", parse::Kind::Container) ==
               std::set<std::string>{"DB", "DB::Foo"},
           "cpp containers");

    functions(Lang::C, "static int add(int a, int b) { return a + b; }\nint *ptr(void) { return 0; }\n",
              {"add", "ptr"}, "c");

    functions(Lang::Go, R"(package labels
func F() {}
func (r *Matcher[K]) Match(s string) bool { return true }
)", {"labels::F", "labels::Matcher::Match"}, "go");

    functions(Lang::Rust, R"(
impl<T> fmt::Display for x::Foo<T> { fn fmt(&self) {} }
mod m { fn g() {} }
fn top() {}
)", {"Foo::fmt", "m::g", "top"}, "rust");

    functions(Lang::Python, R"(
class A:
    @dec
    def m(self):
        def inner():
            pass
def f():
    pass
)", {"A::m", "f"}, "python");

    functions(Lang::Java, R"(
class A {
    A() {}
    /** doc */
    void m() {}
    class B { void n() {} }
}
)", {"A::A", "A::m", "A::B::n"}, "java");

    functions(Lang::TypeScript, R"(
export class C { m(): void {} }
const f = () => 1;
function g() {}
namespace N { export function h() {} }
)", {"C::m", "f", "g", "N::h"}, "typescript");

    expect(parse::chunk(Lang::None, "name: value\n").size() == 0, "files without a grammar are not indexed");

    // Coverage: every non-blank line belongs to exactly one chunk's content.
    std::string src = "#include <x>\nint a() { return 1; }\nint b;\nint c() { return 2; }\n";
    size_t bytes = 0;
    for (const auto& c : parse::chunk(Lang::C, src))
        bytes += std::count_if(c.content.begin(), c.content.end(), [](char ch) { return ch != '\n'; });
    expect(bytes == static_cast<size_t>(std::count_if(src.begin(), src.end(), [](char ch) { return ch != '\n'; })),
           "chunks cover the file exactly once");

    expect(tokens("ConfigParser::parse", true) ==
               std::vector<std::string>{"configparser", "config", "parser", "parse"},
           "document tokens split camel case");
    expect(tokens("max_retry_interval_ms HTTPServer", true) ==
               std::vector<std::string>{"max_retry_interval_ms", "max", "retry", "interval", "ms", "httpserver",
                                        "http", "server"},
           "document tokens split snake and acronym");
    expect(tokens("ConfigParser", false) == std::vector<std::string>{"configparser"}, "query tokens stay whole");
    expect(lexical::terms("Why does the ConfigParser crash") ==
               std::vector<std::string>{"configparser", "config", "parser", "crash"},
           "terms drop stopwords");

    expect(ingest::is_test("server/src/internalClusterTest/java/org/Foo.java"), "gradle test source set");
    expect(ingest::is_test("pkg/labels/regexp_test.go"), "go test file");
    expect(ingest::is_test("pandas/tests/frame/test_api.py"), "python tests dir");
    expect(ingest::is_test("src/FooTests.java") && ingest::is_test("a/b.spec.ts"), "java and ts test names");
    expect(!ingest::is_test("src/latest/contest.go") && !ingest::is_test("src/Attestation.java"), "latest is not a test");

    embedding();

    if (failures)
        std::fprintf(stderr, "%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
