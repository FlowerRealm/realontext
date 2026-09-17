#pragma once
// The MCP endpoint an agent talks to. Stage 3 is the thinnest path through
// docs/modules/serve.md: stdio JSON-RPC in one process, one tool, `query` its
// only argument (D25). token_budget and session_id wait for curate/ and
// playbook/, because a parameter with no reader gets redesigned anyway.
#include <cstddef>
#include <functional>
#include <iosfwd>
#include <string>

#include "match/match.h"
#include "result.h"
#include "store/store.h"

namespace serve {

// Ranking one query. Supplied by the caller: which route to take and how to
// embed a query are decisions of the command line, not of the transport.
using Retrieve = std::function<Result<match::Ranked>(const std::string& query)>;

struct Options {
    std::string branch;    // stage 3 serves one branch, named at startup
    size_t full_text = 20; // per group, how many candidates carry their source
};

// Serves requests read from `in`, one JSON object per line, until end of input.
// Returns an error only when the transport itself fails; a bad request gets a
// JSON-RPC error back and the loop goes on.
Status mcp(store::Db& db, const Options& opt, const Retrieve& retrieve, std::istream& in, std::ostream& out);

} // namespace serve
