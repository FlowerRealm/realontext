#include "parse/lang.h"

#include <array>
#include <utility>

extern "C" {
const TSLanguage* tree_sitter_c();
const TSLanguage* tree_sitter_cpp();
const TSLanguage* tree_sitter_go();
const TSLanguage* tree_sitter_rust();
const TSLanguage* tree_sitter_typescript();
const TSLanguage* tree_sitter_tsx();
const TSLanguage* tree_sitter_python();
const TSLanguage* tree_sitter_java();
}

namespace parse {

namespace {

// .h goes to C++: the C++ grammar reads C headers, the C grammar cannot read
// class bodies, and C++ repositories keep inline methods in headers.
constexpr std::array<std::pair<std::string_view, Lang>, 22> extensions = {{
    {".c", Lang::C},          {".h", Lang::Cpp},        {".cc", Lang::Cpp},
    {".cpp", Lang::Cpp},      {".cxx", Lang::Cpp},      {".hpp", Lang::Cpp},
    {".hh", Lang::Cpp},       {".hxx", Lang::Cpp},      {".ipp", Lang::Cpp},
    {".go", Lang::Go},        {".rs", Lang::Rust},      {".ts", Lang::TypeScript},
    {".mts", Lang::TypeScript}, {".cts", Lang::TypeScript}, {".tsx", Lang::Tsx},
    {".js", Lang::Tsx},       {".jsx", Lang::Tsx},      {".mjs", Lang::Tsx},
    {".cjs", Lang::Tsx},      {".py", Lang::Python},    {".java", Lang::Java},
    {".inl", Lang::Cpp},
}};

} // namespace

Lang lang_of(std::string_view path)
{
    size_t slash = path.rfind('/');
    std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    size_t dot = name.rfind('.');
    if (dot == std::string_view::npos)
        return Lang::None;
    std::string_view ext = name.substr(dot);
    for (auto [e, lang] : extensions)
        if (e.size() == ext.size()) {
            bool same = true;
            for (size_t i = 0; i < e.size(); i++)
                same &= (ext[i] | 0x20) == e[i] || ext[i] == e[i];
            if (same)
                return lang;
        }
    return Lang::None;
}

const TSLanguage* grammar(Lang lang)
{
    switch (lang) {
    case Lang::C: return tree_sitter_c();
    case Lang::Cpp: return tree_sitter_cpp();
    case Lang::Go: return tree_sitter_go();
    case Lang::Rust: return tree_sitter_rust();
    case Lang::TypeScript: return tree_sitter_typescript();
    case Lang::Tsx: return tree_sitter_tsx();
    case Lang::Python: return tree_sitter_python();
    case Lang::Java: return tree_sitter_java();
    case Lang::None: break;
    }
    return nullptr;
}

} // namespace parse
