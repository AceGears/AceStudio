#include "TestSupport.h"

#include "AceTools.h"
#include "McpProtocol.h"

#include <catch2/catch_test_macros.hpp>

using namespace acestudio;
using namespace acestudio::mcp;

namespace {

json envelope(const std::string& version = kModernVersion) {
    return {{meta::kProtocolVersion, version}, {meta::kClientCapabilities, json::object()}, {meta::kClientInfo, {{"name", "t"}, {"version", "1"}}}};
}

json request(int id, const std::string& method, json params = json::object()) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}};
}

json modern(int id, const std::string& method, json params = json::object()) {
    params["_meta"] = envelope();
    return request(id, method, std::move(params));
}

McpServer makeServer() {
    std::vector<Tool> tools;
    tools.push_back({"zeta", "", "last", {{"type", "object"}, {"properties", json::object()}}, {}, [](const json&, const ToolCall&) {
                         return ToolResult::text("z");
                     }});
    tools.push_back({"echo", "Echo", "echoes",
                     {{"type", "object"},
                      {"properties", {{"text", {{"type", "string"}}}, {"n", {{"type", "integer"}}}, {"mode", {{"type", "string"}, {"enum", {"a", "b"}}}}}},
                      {"required", {"text"}},
                      {"additionalProperties", false}},
                     {},
                     [](const json& a, const ToolCall& c) {
                         if (c.progress) c.progress(1, 2.0, "half");
                         if (c.log) c.log("info", "logged");
                         if (c.log) c.log("debug", "noisy");
                         ToolResult r = ToolResult::text(a["text"].get<std::string>());
                         r.structured = json{{"echo", a["text"]}};
                         return r;
                     }});
    tools.push_back({"boom", "", "throws", {{"type", "object"}}, {}, [](const json&, const ToolCall&) -> ToolResult {
                         throw std::runtime_error("kaput");
                     }});
    ServerIdentity id;
    id.version = "9.9.9";
    id.instructions = "be nice";
    return McpServer(id, std::move(tools));
}

int errorCode(const std::optional<json>& r) { return r && r->contains("error") ? (*r)["error"]["code"].get<int>() : 0; }

} // namespace

TEST_CASE("MCP modern era: discover, envelope, results", "[mcp]") {
    McpServer s = makeServer();
    Connection c;
    auto d = s.handle(modern(1, "server/discover"), c);
    REQUIRE(d);
    const json& r = (*d)["result"];
    CHECK(r["resultType"] == "complete");
    CHECK(r["supportedVersions"][0] == kModernVersion);
    CHECK(r["supportedVersions"].size() == 5);
    CHECK(r["capabilities"].contains("tools"));
    CHECK_FALSE(r["capabilities"].contains("logging"));   // logging/setLevel is gone in 2026-07-28
    CHECK(r["_meta"][meta::kServerInfo]["name"] == "acestudio");
    CHECK(r["_meta"][meta::kServerInfo]["version"] == "9.9.9");
    CHECK(r["instructions"] == "be nice");
    CHECK(r["ttlMs"].is_number());
    CHECK(r["cacheScope"] == "public");
    CHECK(c.era == Connection::Era::Modern);

    auto list = s.handle(modern(2, "tools/list"), c);
    REQUIRE(list);
    const json& tools = (*list)["result"]["tools"];
    REQUIRE(tools.size() == 3);
    CHECK(tools[0]["name"] == "boom");   // deterministic: sorted by name
    CHECK(tools[1]["name"] == "echo");
    CHECK(tools[2]["name"] == "zeta");
    CHECK(tools[1]["title"] == "Echo");
    CHECK((*list)["result"]["ttlMs"].is_number());
    CHECK((*list)["result"]["cacheScope"] == "public");
    CHECK((*list)["result"]["resultType"] == "complete");

    auto call = s.handle(modern(3, "tools/call", {{"name", "echo"}, {"arguments", {{"text", "hi"}}}}), c);
    REQUIRE(call);
    CHECK((*call)["result"]["content"][0]["text"] == "hi");
    CHECK((*call)["result"]["structuredContent"]["echo"] == "hi");
    CHECK((*call)["result"]["isError"] == false);
    CHECK((*call)["result"]["_meta"][meta::kServerInfo]["name"] == "acestudio");

    // Removed in 2026-07-28.
    CHECK(errorCode(s.handle(modern(4, "ping"), c)) == code::kMethodNotFound);
    CHECK(errorCode(s.handle(modern(5, "logging/setLevel", {{"level", "info"}}), c)) == code::kMethodNotFound);
    // initialize on a modern connection names the versions.
    auto init = s.handle(request(6, "initialize", {{"protocolVersion", "2025-11-25"}}), c);
    REQUIRE(errorCode(init) == code::kUnsupportedProtocolVersion);
    CHECK((*init)["error"]["data"]["supported"][0] == kModernVersion);
}

TEST_CASE("MCP modern era: version and envelope errors", "[mcp]") {
    McpServer s = makeServer();
    Connection c;
    json bad = request(1, "tools/list", {{"_meta", envelope("1900-01-01")}});
    auto r = s.handle(bad, c);
    REQUIRE(errorCode(r) == code::kUnsupportedProtocolVersion);
    CHECK((*r)["error"]["data"]["requested"] == "1900-01-01");
    CHECK((*r)["error"]["data"]["supported"][0] == kModernVersion);
    // A legacy version in a modern envelope is not a modern request either.
    CHECK(errorCode(s.handle(request(2, "tools/list", {{"_meta", envelope("2025-11-25")}}), c)) == code::kUnsupportedProtocolVersion);
    // Client capabilities are required.
    json noCaps = request(3, "tools/list", {{"_meta", {{meta::kProtocolVersion, kModernVersion}}}});
    CHECK(errorCode(s.handle(noCaps, c)) == code::kInvalidParams);
    CHECK(errorCode(s.handle(modern(4, "resources/list"), c)) == code::kMethodNotFound);
}

TEST_CASE("MCP legacy era: initialize handshake", "[mcp]") {
    McpServer s = makeServer();
    Connection c;
    // Before any decision only ping and initialize work.
    CHECK(errorCode(s.handle(request(1, "tools/list"), c)) == code::kInvalidRequest);
    CHECK(s.handle(request(2, "ping"), c)->contains("result"));
    CHECK(c.era == Connection::Era::Undecided);

    auto init = s.handle(request(3, "initialize", {{"protocolVersion", "2025-06-18"}, {"capabilities", json::object()},
                                                   {"clientInfo", {{"name", "x"}, {"version", "1"}}}}), c);
    REQUIRE(init);
    CHECK((*init)["result"]["protocolVersion"] == "2025-06-18");   // a supported version is echoed
    CHECK((*init)["result"]["serverInfo"]["name"] == "acestudio");
    CHECK((*init)["result"]["capabilities"].contains("logging"));
    CHECK_FALSE((*init)["result"].contains("resultType"));          // 2026 vocabulary stays off the legacy wire
    CHECK(c.era == Connection::Era::Legacy);
    CHECK_FALSE(s.handle({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}, c));
    CHECK(c.initialized);

    auto list = s.handle(request(4, "tools/list"), c);
    REQUIRE(list);
    CHECK_FALSE((*list)["result"].contains("ttlMs"));
    CHECK_FALSE((*list)["result"].contains("_meta"));
    CHECK(s.handle(request(5, "ping"), c)->contains("result"));
    CHECK(s.handle(request(6, "logging/setLevel", {{"level", "warning"}}), c)->contains("result"));
    CHECK(c.logLevel == "warning");
    CHECK(errorCode(s.handle(request(7, "logging/setLevel", {{"level", "loud"}}), c)) == code::kInvalidParams);
    // The 2026 envelope is refused on a legacy connection, and so is a second initialize.
    CHECK(errorCode(s.handle(modern(8, "tools/list"), c)) == code::kInvalidRequest);
    CHECK(errorCode(s.handle(request(9, "initialize", {{"protocolVersion", "2025-11-25"}}), c)) == code::kInvalidRequest);

    // An unknown client version gets our newest legacy version.
    Connection c2;
    auto init2 = s.handle(request(1, "initialize", {{"protocolVersion", "2099-01-01"}}), c2);
    CHECK((*init2)["result"]["protocolVersion"] == "2025-11-25");
}

TEST_CASE("MCP tool calls: errors, progress, logs", "[mcp]") {
    McpServer s = makeServer();
    Connection c;
    // Unknown tool: protocol error. Bad arguments and exceptions: tool errors.
    CHECK(errorCode(s.handle(modern(1, "tools/call", {{"name", "nope"}}), c)) == code::kInvalidParams);
    for (const json& args : {json::object(), json{{"text", 5}}, json{{"text", "x"}, {"extra", 1}}, json{{"text", "x"}, {"n", 1.5}},
                             json{{"text", "x"}, {"mode", "c"}}}) {
        INFO(args.dump());
        auto r = s.handle(modern(2, "tools/call", {{"name", "echo"}, {"arguments", args}}), c);
        REQUIRE(r);
        CHECK((*r)["result"]["isError"] == true);
    }
    CHECK((*s.handle(modern(3, "tools/call", {{"name", "echo"}, {"arguments", {{"text", "x"}, {"n", 2.0}, {"mode", "b"}}}}), c))["result"]["isError"] == false);
    auto boom = s.handle(modern(4, "tools/call", {{"name", "boom"}}), c);
    CHECK((*boom)["result"]["isError"] == true);
    CHECK((*boom)["result"]["content"][0]["text"].get<std::string>().find("kaput") != std::string::npos);

    // Progress needs a progressToken; logs need _meta logLevel (modern).
    std::vector<json> sent;
    Notify n = [&](const json& j) { sent.push_back(j); };
    json p{{"name", "echo"}, {"arguments", {{"text", "x"}}}};
    p["_meta"] = envelope();
    (void)s.handle(request(5, "tools/call", p), c, n);
    CHECK(sent.empty());
    p["_meta"]["progressToken"] = "tok";
    p["_meta"][meta::kLogLevel] = "info";
    (void)s.handle(request(6, "tools/call", p), c, n);
    REQUIRE(sent.size() == 2);
    CHECK(sent[0]["method"] == "notifications/progress");
    CHECK(sent[0]["params"]["progressToken"] == "tok");
    CHECK(sent[0]["params"]["total"] == 2.0);
    CHECK(sent[1]["method"] == "notifications/message");
    CHECK(sent[1]["params"]["data"] == "logged");   // debug is below the requested level
}

TEST_CASE("MCP message validation", "[mcp]") {
    McpServer s = makeServer();
    Connection c;
    CHECK(errorCode(s.handle(json{{"id", 1}, {"method", "x"}}, c)) == code::kInvalidRequest);   // no jsonrpc
    CHECK(errorCode(s.handle(json::array(), c)) == code::kInvalidRequest);
    CHECK(errorCode(s.handle(json{{"jsonrpc", "2.0"}, {"id", 1.5}, {"method", "ping"}}, c)) == code::kInvalidRequest);
    CHECK_FALSE(s.handle(json{{"jsonrpc", "2.0"}, {"id", 1}, {"result", json::object()}}, c));   // responses are ignored
    CHECK(McpServer::parseError("x")["error"]["code"] == code::kParseError);
    json cancel{{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"}, {"params", {{"requestId", 7}}}};
    CHECK(McpServer::cancelledRequestId(cancel) == json(7));
    CHECK_FALSE(s.handle(cancel, c));
}

TEST_CASE("MCP tool helpers: base64 and root checks", "[mcp]") {
    CHECK(base64({}) == "");
    CHECK(base64({'f'}) == "Zg==");
    CHECK(base64({'f', 'o'}) == "Zm8=");
    CHECK(base64({'f', 'o', 'o', 'b', 'a', 'r'}) == "Zm9vYmFy");

    TempDir dir;
    std::filesystem::create_directories(dir / "proj" / "sub");
    const auto root = std::filesystem::weakly_canonical(dir / "proj");
    auto ok = resolveInRoots({root}, "sub/a.png");
    REQUIRE(ok);
    CHECK(ok.value() == root / "sub" / "a.png");
    CHECK(resolveInRoots({root}, (root / "b.png").string()));
    CHECK_FALSE(resolveInRoots({root}, "../x.png"));
    CHECK_FALSE(resolveInRoots({root}, "sub/../../x.png"));
    CHECK_FALSE(resolveInRoots({root}, (dir / "projx" / "a.png").string()));   // a sibling with a common prefix
    CHECK_FALSE(resolveInRoots({}, "a.png"));
}
