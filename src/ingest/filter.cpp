#include "ingest/filter.h"

#include <array>
#include <string>

namespace ingest {

namespace {

bool starts_with(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }
bool ends_with(std::string_view s, std::string_view p)
{
    return s.size() >= p.size() && s.substr(s.size() - p.size()) == p;
}

constexpr std::array skip_dirs = {
    std::string_view("third_party"), std::string_view("vendor"), std::string_view("external"),
    std::string_view("node_modules"), std::string_view("secrets"),
};

bool skip_dir(std::string_view dir)
{
    for (auto d : skip_dirs)
        if (dir == d)
            return true;
    return false;
}

bool skip_file(std::string_view name)
{
    return ends_with(name, ".min.js") || ends_with(name, ".pb.cc") || ends_with(name, ".pb.go") ||
           (starts_with(name, "moc_") && ends_with(name, ".cpp")) ||
           name.find("_generated.") != std::string_view::npos ||
           name == ".env" || starts_with(name, ".env.") ||
           ends_with(name, ".pem") || ends_with(name, ".key") ||
           name == "id_rsa" || name == "credentials";
}

bool lower(char c) { return c >= 'a' && c <= 'z'; }
bool upper(char c) { return c >= 'A' && c <= 'Z'; }
bool digit(char c) { return c >= '0' && c <= '9'; }

std::string ascii_lower(std::string_view s)
{
    std::string out(s);
    for (char& c : out)
        if (upper(c))
            c = static_cast<char>(c | 0x20);
    return out;
}

constexpr std::array test_dirs = {
    std::string_view("test"),     std::string_view("tests"),    std::string_view("testing"),
    std::string_view("__tests__"), std::string_view("testdata"), std::string_view("spec"),
    std::string_view("specs"),    std::string_view("fixtures"), std::string_view("e2e"),
    std::string_view("benchmark"), std::string_view("benchmarks"),
};

// A camel-case word ending the name: fooTest, integTests. Not "latest".
bool camel_suffix(std::string_view s, std::string_view word)
{
    return s.size() > word.size() && ends_with(s, word) &&
           (lower(s[s.size() - word.size() - 1]) || digit(s[s.size() - word.size() - 1]));
}

bool test_dir(std::string_view d)
{
    std::string l = ascii_lower(d);
    for (auto t : test_dirs)
        if (l == t)
            return true;
    // testFixtures, test_utils, test-support
    if (starts_with(d, "test") && d.size() > 4 && (upper(d[4]) || d[4] == '_' || d[4] == '-'))
        return true;
    return camel_suffix(d, "Test") || camel_suffix(d, "Tests");
}

bool test_file(std::string_view name)
{
    size_t dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0)
        return false;
    std::string_view stem = name.substr(0, dot);
    return starts_with(stem, "test_") || starts_with(stem, "gtest_") || stem == "conftest" ||
           ends_with(stem, "_test") || ends_with(stem, "_tests") || ends_with(stem, "_unittest") ||
           ends_with(stem, ".test") || ends_with(stem, ".spec") ||
           camel_suffix(stem, "Test") || camel_suffix(stem, "Tests") || camel_suffix(stem, "IT") ||
           camel_suffix(stem, "TestCase");
}

} // namespace

bool is_test(std::string_view path)
{
    size_t start = 0;
    for (size_t slash; (slash = path.find('/', start)) != std::string_view::npos; start = slash + 1)
        if (test_dir(path.substr(start, slash - start)))
            return true;
    return test_file(path.substr(start));
}

bool indexable(std::string_view path)
{
    size_t start = 0;
    for (size_t slash; (slash = path.find('/', start)) != std::string_view::npos; start = slash + 1)
        if (skip_dir(path.substr(start, slash - start)))
            return false;
    return !skip_file(path.substr(start));
}

bool is_text(std::string_view content)
{
    return content.substr(0, 8000).find('\0') == std::string_view::npos;
}

} // namespace ingest
