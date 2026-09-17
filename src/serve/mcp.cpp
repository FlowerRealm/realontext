#include "serve/mcp.h"

#include <istream>
#include <ostream>
#include <string>

#include <nlohmann/json.hpp>

namespace serve {

namespace {

using nlohmann::json;

// The version this server speaks. A client asking for another one gets its own
// back when it looks like a version at all: the handshake is not the place to
// be strict, and the payloads below are the same either way.
constexpr const char* protocol = "2025-06-18";

constexpr int parse_error = -32700;
constexpr int invalid_request = -32600;
constexpr int method_not_found = -32601;

void reply(std::ostream& out, const json& id, json result)
{
    out << json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}}.dump() << "\n" << std::flush;
}

void reply_error(std::ostream& out, const json& id, int code, const std::string& message)
{
    out << json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}}.dump() << "\n"
        << std::flush;
}

json tool_list(const Options& opt)
{
    // One tool, one argument. The budget and session arguments serve.md calls
    // architectural arrive with the layers that read them (D25).
    return json{{"tools",
                 json::array({{{"name", "codebase-retrieval"},
                               {"description", "Finds the code on branch " + opt.branch +
                                                   " that a task description is about. Returns two groups: `code`, "
                                                   "where the change likely goes, and `tests`, what exercises that "
                                                   "behaviour. Ask with a task or bug description, not a symbol name."},
                               {"inputSchema",
                                {{"type", "object"},
                                 {"properties", {{"query", {{"type", "string"}, {"description", "The task, in prose"}}}}},
                                 {"required", json::array({"query"})}}}}})}};
}

json locations(const store::ChunkInfo& info)
{
    json where = json::array();
    for (const store::Location& l : info.where)
        where.push_back({{"path", l.path}, {"start_line", l.start_line}, {"end_line", l.end_line}});
    return where;
}

Result<json> render(store::Db& db, const match::Group& group, size_t full_text)
{
    json chunks = json::array();
    for (const match::Candidate& c : group.chunks) {
        json entry{{"id", c.chunk},
                   {"score", c.score},
                   {"kind", c.info.kind == parse::Kind::Function ? "function" : "container"},
                   {"symbol", c.info.symbol},
                   {"locations", locations(c.info)}};
        // Stage 3 does not downgrade anything, so the ones that fit carry their
        // source and the rest carry a location. The count is a placeholder for
        // the budget curate/ will spend instead (D25).
        if (chunks.size() < full_text) {
            auto text = store::chunk_text(db, c.chunk);
            if (!text)
                return Err{text.error()};
            entry["text"] = *text;
        }
        chunks.push_back(std::move(entry));
    }
    json files = json::array();
    for (const match::File& f : group.files)
        files.push_back({{"path", f.path}, {"score", f.score}});
    return json{{"chunks", std::move(chunks)}, {"files", std::move(files)}};
}

Result<json> call_tool(store::Db& db, const Options& opt, const Retrieve& retrieve, const json& params)
{
    if (!params.is_object() || params.value("name", "") != "codebase-retrieval")
        return Err{"no such tool"};
    const json args = params.contains("arguments") ? params.at("arguments") : json::object();
    if (!args.is_object() || !args.contains("query") || !args.at("query").is_string())
        return Err{"codebase-retrieval needs a string `query`"};

    auto ranked = retrieve(args.at("query").get<std::string>());
    if (!ranked)
        return Err{ranked.error()};
    auto code = render(db, ranked->code, opt.full_text);
    if (!code)
        return Err{code.error()};
    auto tests = render(db, ranked->tests, opt.full_text);
    if (!tests)
        return Err{tests.error()};

    json payload{{"branch", opt.branch}, {"code", std::move(*code)}, {"tests", std::move(*tests)}};
    // MCP carries tool output as content blocks; the structured copy is there
    // for clients that read it, and costs nothing for the ones that do not.
    return json{{"content", json::array({{{"type", "text"}, {"text", payload.dump(2)}}})},
                {"structuredContent", std::move(payload)}};
}

} // namespace

Status mcp(store::Db& db, const Options& opt, const Retrieve& retrieve, std::istream& in, std::ostream& out)
{
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty())
            continue;
        json request = json::parse(line, nullptr, false);
        if (request.is_discarded()) {
            reply_error(out, nullptr, parse_error, "not JSON");
            continue;
        }
        if (!request.is_object() || !request.contains("method") || !request.at("method").is_string()) {
            reply_error(out, nullptr, invalid_request, "not a JSON-RPC request");
            continue;
        }
        const std::string method = request.at("method").get<std::string>();
        // No id means a notification: acknowledge nothing, answer nothing.
        if (!request.contains("id"))
            continue;
        const json& id = request.at("id");

        if (method == "initialize") {
            std::string version = protocol;
            if (request.contains("params") && request.at("params").is_object()) {
                const json& p = request.at("params");
                if (p.contains("protocolVersion") && p.at("protocolVersion").is_string())
                    version = p.at("protocolVersion").get<std::string>();
            }
            reply(out, id,
                  {{"protocolVersion", version},
                   {"capabilities", {{"tools", json::object()}}},
                   {"serverInfo", {{"name", "realontext"}, {"version", "0.0.0"}}}});
        } else if (method == "tools/list") {
            reply(out, id, tool_list(opt));
        } else if (method == "tools/call") {
            auto result = call_tool(db, opt, retrieve,
                                    request.contains("params") ? request.at("params") : json::object());
            if (result)
                reply(out, id, std::move(*result));
            else
                // A failed retrieval is the tool's answer, not a protocol fault:
                // the agent should see why rather than lose the connection.
                reply(out, id,
                      {{"isError", true},
                       {"content", json::array({{{"type", "text"}, {"text", result.error()}}})}});
        } else if (method == "ping") {
            reply(out, id, json::object());
        } else {
            reply_error(out, id, method_not_found, method);
        }
    }
    return ok;
}

} // namespace serve
