#pragma once
#include <functional>
#include <string_view>

#include "result.h"

struct sqlite3;

namespace lexical {

// Called per token with its byte span in the input. Return false to stop.
using Emit = std::function<bool(std::string_view token, bool colocated, size_t start, size_t end)>;

// Words are runs of [A-Za-z0-9_] and non-ASCII bytes, lowercased. In document
// mode a compound identifier also yields its sub-words at the same position:
//   ConfigParser -> configparser  config  parser
// Query mode yields whole words only; query-side splitting is terms()'s job.
void tokenize(std::string_view text, bool document, const Emit& emit);

// Registers the tokenizer with FTS5 under the name "code". Per connection.
Status register_tokenizer(sqlite3* db);

} // namespace lexical
