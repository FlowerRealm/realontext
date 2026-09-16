#include "ingest/filter.h"

#include <array>

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

} // namespace

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
