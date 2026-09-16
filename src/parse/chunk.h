#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "parse/lang.h"

namespace parse {

// Function: one callable. Container: whatever a class, namespace, impl or the
// file itself holds besides its functions. The file is a container without a
// name, so top-level code and grammar-less files need no rule of their own.
enum class Kind : uint8_t { Function = 0, Container = 1 };

struct Chunk {
    uint32_t start_line; // 1-based, inclusive
    uint32_t end_line;
    Kind kind;
    std::string symbol; // Outer::inner, normalised; empty for an unnamed container
    std::string content;
};

// Chunks never overlap and together cover every non-blank line of `src`.
// Pure: no I/O, no model, no storage (docs/modules/parse.md).
std::vector<Chunk> chunk(Lang lang, std::string_view src);

// The frozen qualified-name rules of docs/benchmark.md: no generics, argument
// lists, return types or pointers; every nesting separator becomes "::".
std::string normalize(std::string_view name);

} // namespace parse
