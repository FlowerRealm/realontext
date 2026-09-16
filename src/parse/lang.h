#pragma once
#include <cstdint>
#include <string_view>

struct TSLanguage;

namespace parse {

// Language is a property of the path, not of the blob: the same bytes under
// .ts and .tsx parse differently. Everything keyed by blob also keys by Lang.
enum class Lang : uint8_t { None = 0, C, Cpp, Go, Rust, TypeScript, Tsx, Python, Java };

Lang lang_of(std::string_view path);
const TSLanguage* grammar(Lang lang);

} // namespace parse
