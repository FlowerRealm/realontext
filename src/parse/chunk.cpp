#include "parse/chunk.h"

#include <optional>

#include <tree_sitter/api.h>

namespace parse {

namespace {

// Definitions nested deeper than this inside non-definition syntax (expression
// trees, initialiser lists) stay in their container's text. It also bounds the
// recursion on pathological generated files.
constexpr int max_depth = 64;

std::string_view type(TSNode n) { return ts_node_type(n); }

TSNode field(TSNode n, std::string_view name)
{
    return ts_node_child_by_field_name(n, name.data(), static_cast<uint32_t>(name.size()));
}

bool has(TSNode n, std::string_view name) { return !ts_node_is_null(field(n, name)); }

uint32_t first_line(TSNode n) { return ts_node_start_point(n).row + 1; }

uint32_t last_line(TSNode n)
{
    TSPoint s = ts_node_start_point(n), e = ts_node_end_point(n);
    return e.column == 0 && e.row > s.row ? e.row : e.row + 1;
}

bool is_comment(TSNode n)
{
    std::string_view t = type(n);
    return t.find("comment") != std::string_view::npos || t == "attribute_item";
}

bool blank(std::string_view s)
{
    for (char c : s)
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f' && c != '\v')
            return false;
    return true;
}

std::string join(const std::string& scope, const std::string& name)
{
    if (scope.empty())
        return name;
    if (name.empty())
        return scope;
    return scope + "::" + name;
}

struct Def {
    Kind kind;
    TSNode node; // for a container: the node whose children hold its members
    std::string name;
};

class Chunker {
public:
    Chunker(Lang lang, std::string_view src) : lang_(lang), src_(src) {}

    std::vector<Chunk> run(TSNode root)
    {
        std::string scope;
        if (lang_ == Lang::Go)
            scope = go_package(root);
        container(root, scope, 0, static_cast<uint32_t>(src_.size()), 1, last_line(root));
        return std::move(out_);
    }

private:
    Lang lang_;
    std::string_view src_;
    std::vector<Chunk> out_;

    std::string_view text(TSNode n) const
    {
        uint32_t s = ts_node_start_byte(n), e = ts_node_end_byte(n);
        return src_.substr(s, e - s);
    }

    std::string name_of(TSNode n) const { return ts_node_is_null(n) ? std::string() : normalize(text(n)); }

    void container(TSNode node, const std::string& scope, uint32_t begin, uint32_t end, uint32_t l1, uint32_t l2)
    {
        std::string rest;
        uint32_t cursor = begin;
        walk(node, scope, cursor, rest, 0);
        rest.append(src_.substr(cursor, end - cursor));
        if (!blank(rest))
            out_.push_back({l1, l2, Kind::Container, scope, std::move(rest)});
    }

    void walk(TSNode node, const std::string& scope, uint32_t& cursor, std::string& rest, int depth)
    {
        if (depth > max_depth)
            return;
        TSNode lead{};
        bool leading = false;
        uint32_t count = ts_node_named_child_count(node);
        for (uint32_t i = 0; i < count; i++) {
            TSNode c = ts_node_named_child(node, i);
            if (is_comment(c)) {
                if (!leading)
                    lead = c;
                leading = true;
                continue;
            }
            std::optional<Def> d = definition(c);
            TSNode first = leading ? lead : c;
            leading = false;
            if (!d) {
                walk(c, scope, cursor, rest, depth + 1);
                continue;
            }
            uint32_t s = std::max(ts_node_start_byte(first), cursor);
            uint32_t e = ts_node_end_byte(c);
            rest.append(src_.substr(cursor, s - cursor));
            rest.push_back('\n');
            std::string name = join(scope, d->name);
            if (d->kind == Kind::Function)
                out_.push_back({first_line(first), last_line(c), Kind::Function, std::move(name),
                                std::string(src_.substr(s, e - s))});
            else
                container(d->node, name, s, e, first_line(first), last_line(c));
            cursor = e;
        }
    }

    // ---------------------------------------------------------------- per language

    std::optional<Def> definition(TSNode n) const
    {
        std::string_view t = type(n);
        // Wrappers keep attributes, templates and export keywords inside the chunk.
        if (t == "template_declaration" || t == "decorated_definition" || t == "export_statement") {
            uint32_t count = ts_node_named_child_count(n);
            for (uint32_t i = 0; i < count; i++)
                if (auto d = definition(ts_node_named_child(n, i)))
                    return d;
            return std::nullopt;
        }
        switch (lang_) {
        case Lang::C:
        case Lang::Cpp: return cpp(n, t);
        case Lang::Go: return go(n, t);
        case Lang::Rust: return rust(n, t);
        case Lang::TypeScript:
        case Lang::Tsx: return typescript(n, t);
        case Lang::Python: return python(n, t);
        case Lang::Java: return java(n, t);
        case Lang::None: break;
        }
        return std::nullopt;
    }

    static std::optional<Def> function(TSNode n, std::string name)
    {
        if (name.empty())
            return std::nullopt;
        return Def{Kind::Function, n, std::move(name)};
    }

    std::optional<Def> named_container(TSNode n) const
    {
        if (!has(n, "body"))
            return std::nullopt;
        return Def{Kind::Container, n, name_of(field(n, "name"))};
    }

    std::optional<Def> cpp(TSNode n, std::string_view t) const
    {
        if (t == "function_definition") {
            TSNode d = field(n, "declarator");
            while (!ts_node_is_null(d) && has(d, "declarator"))
                d = field(d, "declarator");
            return function(n, name_of(d));
        }
        if (lang_ == Lang::Cpp && (t == "namespace_definition" || t == "class_specifier" ||
                                   t == "struct_specifier" || t == "union_specifier"))
            return named_container(n);
        return std::nullopt;
    }

    std::string go_package(TSNode root) const
    {
        uint32_t count = ts_node_named_child_count(root);
        for (uint32_t i = 0; i < count; i++) {
            TSNode c = ts_node_named_child(root, i);
            if (type(c) == "package_clause" && ts_node_named_child_count(c) > 0)
                return name_of(ts_node_named_child(c, 0));
        }
        return {};
    }

    std::optional<Def> go(TSNode n, std::string_view t) const
    {
        if (t == "function_declaration")
            return function(n, name_of(field(n, "name")));
        if (t != "method_declaration")
            return std::nullopt;
        // Receiver (*T[K]) names the type the method belongs to: T::M.
        std::string receiver;
        TSNode params = field(n, "receiver");
        if (!ts_node_is_null(params) && ts_node_named_child_count(params) > 0) {
            TSNode ty = field(ts_node_named_child(params, 0), "type");
            if (!ts_node_is_null(ty)) {
                std::string_view s = text(ty);
                receiver = normalize(s.substr(0, s.find('[')));
            }
        }
        std::string name = name_of(field(n, "name"));
        return function(n, name.empty() ? name : join(receiver, name));
    }

    std::optional<Def> rust(TSNode n, std::string_view t) const
    {
        if (t == "function_item")
            return function(n, name_of(field(n, "name")));
        if (t == "impl_item") {
            // impl<T> Trait for path::Type<T> -> Type
            std::string ty = name_of(field(n, "type"));
            size_t sep = ty.rfind("::");
            if (sep != std::string::npos)
                ty = ty.substr(sep + 2);
            if (!has(n, "body"))
                return std::nullopt;
            return Def{Kind::Container, n, ty};
        }
        if (t == "trait_item" || t == "mod_item")
            return named_container(n);
        return std::nullopt;
    }

    std::optional<Def> python(TSNode n, std::string_view t) const
    {
        if (t == "function_definition")
            return function(n, name_of(field(n, "name")));
        if (t == "class_definition")
            return named_container(n);
        return std::nullopt;
    }

    std::optional<Def> java(TSNode n, std::string_view t) const
    {
        if (t == "method_declaration" || t == "constructor_declaration" || t == "compact_constructor_declaration")
            return function(n, name_of(field(n, "name")));
        if (t == "class_declaration" || t == "interface_declaration" || t == "enum_declaration" ||
            t == "record_declaration" || t == "annotation_type_declaration")
            return named_container(n);
        return std::nullopt;
    }

    static bool function_value(TSNode v)
    {
        if (ts_node_is_null(v))
            return false;
        std::string_view t = type(v);
        return t == "arrow_function" || t == "function_expression" || t == "function" ||
               t == "generator_function";
    }

    std::optional<Def> typescript(TSNode n, std::string_view t) const
    {
        if (t == "function_declaration" || t == "generator_function_declaration" || t == "method_definition")
            return function(n, name_of(field(n, "name")));
        if (t == "public_field_definition" && function_value(field(n, "value")))
            return function(n, name_of(field(n, "name")));
        if (t == "lexical_declaration" || t == "variable_declaration") {
            // const f = () => {} is a function named f; several declarators are not.
            if (ts_node_named_child_count(n) != 1)
                return std::nullopt;
            TSNode d = ts_node_named_child(n, 0);
            if (type(d) != "variable_declarator" || !function_value(field(d, "value")))
                return std::nullopt;
            return function(n, name_of(field(d, "name")));
        }
        if (t == "class_declaration" || t == "abstract_class_declaration" || t == "class" ||
            t == "internal_module")
            return named_container(n);
        return std::nullopt;
    }
};

TSParser* parser_for(Lang lang)
{
    thread_local TSParser* parser = ts_parser_new();
    ts_parser_set_language(parser, grammar(lang));
    return parser;
}

} // namespace

std::vector<Chunk> chunk(Lang lang, std::string_view src)
{
    if (blank(src))
        return {};
    if (lang == Lang::None) {
        uint32_t lines = 1;
        for (size_t i = 0; i + 1 < src.size(); i++)
            lines += src[i] == '\n';
        return {Chunk{1, lines, Kind::Container, {}, std::string(src)}};
    }
    TSTree* tree = ts_parser_parse_string(parser_for(lang), nullptr, src.data(), static_cast<uint32_t>(src.size()));
    if (!tree)
        return chunk(Lang::None, src);
    std::vector<Chunk> out = Chunker(lang, src).run(ts_tree_root_node(tree));
    ts_tree_delete(tree);
    return out;
}

std::string normalize(std::string_view name)
{
    size_t arrow = name.find("->");
    if (arrow != std::string_view::npos)
        name = name.substr(0, arrow);
    std::string out;
    int angle = 0, paren = 0;
    for (char c : name) {
        if (c == '<') { angle++; continue; }
        if (c == '>' && angle) { angle--; continue; }
        if (c == '(') { paren++; continue; }
        if (c == ')' && paren) { paren--; continue; }
        if (angle || paren || c == '*' || c == '&' || c == ' ' || c == '\t' || c == '\n' || c == '\r')
            continue;
        if (c == '.' || c == '#') {
            out += "::";
            continue;
        }
        out += c;
    }
    std::string collapsed;
    for (size_t i = 0; i < out.size(); i++) {
        if (out[i] == ':') {
            size_t j = i;
            while (j < out.size() && out[j] == ':')
                j++;
            collapsed += j - i == 1 ? ":" : "::";
            i = j - 1;
            continue;
        }
        collapsed += out[i];
    }
    size_t b = collapsed.find_first_not_of(':');
    if (b == std::string::npos)
        return {};
    size_t e = collapsed.find_last_not_of(':');
    return collapsed.substr(b, e - b + 1);
}

} // namespace parse
