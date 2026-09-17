// Chunk names and tokens for each language. Names are what function-level
// scoring compares, so a wrong one silently caps recall. Then the embedding
// path with a fake clock and a fake provider: no network, no sleeping.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "ingest/filter.h"
#include "lexical/lexical.h"
#include "lexical/tokenizer.h"
#include "match/match.h"
#include "model/model.h"
#include "parse/chunk.h"
#include "store/embed.h"

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
    Oid blob{1};
    std::string src = "int alpha() { return 1; }\nint beta() { return 2; }\nint gamma() { return 3; }\n";
    auto indexed = store::index_branch(*db, "main", Oid{9}, {{"x.c", blob}}, [&](const Oid&) { return Result<std::string>(src); });
    expect(indexed && indexed->chunks == 3, "three chunks to embed");

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
    expect(!partial && *store::unembedded(*db) == 2, "a failed run keeps what it was paid for");

    model::Embedder working(config, "", fake_provider, open, ec.clock());
    auto resumed = store::embed(*db, fp, working, {1, 2}, [](const store::EmbedProgress&, size_t) {});
    expect(resumed && resumed->chunks == 2 && resumed->tokens == 20, "rerun embeds only what is pending");

    auto mixed = store::embed(*db, other, working, {1, 1}, [](const store::EmbedProgress&, size_t) {});
    expect(!mixed && mixed.error().find("embed.model") == 0, "a different model is refused by name");

    std::vector<float> query{0, 1, 0};
    auto ranked = match::nearest(*db, "main", query, 2);
    expect(ranked && ranked->chunks.size() == 2 && ranked->chunks[0].info.symbol == "beta" &&
               ranked->files.size() == 1 && ranked->files[0].path == "x.c",
           "nearest chunk first, files by best chunk");

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
