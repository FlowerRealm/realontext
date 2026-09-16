#pragma once
#include <string_view>

namespace ingest {

// Paths that never reach the index: vendored code (cost) and secrets (safety).
// docs/modules/ingest.md -> 过滤. Applied identically during evaluation.
bool indexable(std::string_view path);

// Blobs past this size are data, not source.
inline constexpr size_t max_blob_bytes = 1 << 20;

// Test code: kept in the index, ranked below implementation (docs/modules/match.md).
// Path conventions only: test directories (tests/, __tests__/, src/integTest/)
// and test file names (foo_test.go, test_foo.py, FooTests.java, foo.spec.ts).
bool is_test(std::string_view path);

// git's own heuristic: a NUL byte in the first 8000 bytes means binary.
bool is_text(std::string_view content);

} // namespace ingest
