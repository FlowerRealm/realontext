// Chunk names and tokens for each language. Names are what function-level
// scoring compares, so a wrong one silently caps recall.
#include <algorithm>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "lexical/lexical.h"
#include "lexical/tokenizer.h"
#include "parse/chunk.h"

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

    if (failures)
        std::fprintf(stderr, "%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
