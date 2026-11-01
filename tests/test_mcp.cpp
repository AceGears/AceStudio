#include "TestSupport.h"

#include "AceTools.h"
#include "McpProtocol.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

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
    (void)s.handle(request(5, "tools/call", p), c, Peer{n, {}, nullptr});
    CHECK(sent.empty());
    p["_meta"]["progressToken"] = "tok";
    p["_meta"][meta::kLogLevel] = "info";
    (void)s.handle(request(6, "tools/call", p), c, Peer{n, {}, nullptr});
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

// ---------------------------------------------------------------- Phase 7 pass: input, prompts, resources

namespace {

McpServer inputServer() {
    std::vector<Tool> tools;
    tools.push_back({"ask", "", "asks the user", {{"type", "object"}}, {}, [](const json&, const ToolCall& c) {
                         auto r = c.input->ask("name", "elicitation/create", {{"message", "Name?"}});
                         if (!r) {
                             c.input->setState("round-1");
                             return c.input->modern() && c.input->error().empty() ? ToolResult{} : ToolResult::text(c.input->error(), true);
                         }
                         return ToolResult::text("Hello " + r->value("content", json::object()).value("name", std::string()) + " " +
                                                 c.input->state().dump());
                     }});
    std::vector<Prompt> prompts;
    prompts.push_back({"greet", "", "greets", {{"who", "who", true}},
                       [](const json& a, InputContext&) -> Result<json> {
                           return json{{"messages", {{{"role", "user"}, {"content", textBlock("Hi " + a["who"].get<std::string>())}}}}};
                       },
                       [](const std::string& n, const std::string& v, const json&) {
                           std::vector<std::string> out;
                           for (const char* x : {"world", "wanda", "bob"})
                               if (n == "who" && std::string(x).rfind(v, 0) == 0) out.push_back(x);
                           return out;
                       }});
    std::vector<Resource> resources;
    resources.push_back({"test://a", "a", "", "A", "text/plain",
                         [](const std::string& uri, InputContext&) -> Result<json> { return json::array({{{"uri", uri}, {"text", "A!"}}}); },
                         60000, true});
    std::vector<ResourceTemplate> templates;
    templates.push_back({"test://item/{+path}", "item", "", "items", "text/plain",
                         [](const std::string& uri, const std::map<std::string, std::string>& v, InputContext&) -> Result<json> {
                             return json::array({{{"uri", uri}, {"text", "item " + v.at("path")}}});
                         }, {}, 0, false});
    templates.push_back({"test://item/{+path}/size", "size", "", "item sizes", "text/plain",
                         [](const std::string& uri, const std::map<std::string, std::string>& v, InputContext&) -> Result<json> {
                             return json::array({{{"uri", uri}, {"text", "size of " + v.at("path")}}});
                         }, {}, 0, false});
    return McpServer({}, std::move(tools), std::move(prompts), std::move(resources), std::move(templates));
}

json modernWith(int id, const std::string& method, json params, json caps) {
    params["_meta"] = envelope();
    params["_meta"][meta::kClientCapabilities] = std::move(caps);
    return request(id, method, std::move(params));
}

} // namespace

TEST_CASE("MCP HMAC-SHA-256 matches RFC 4231", "[mcp]") {
    CHECK(hmacSha256Hex("Jefe", "what do ya want for nothing?") == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    CHECK(hmacSha256Hex(std::string(20, '\x0b'), "Hi There") == "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
}

TEST_CASE("MCP multi round-trip input (2026-07-28)", "[mcp]") {
    McpServer s = inputServer();
    Connection c;
    const json caps{{"elicitation", json::object()}};
    auto r1 = s.handle(modernWith(1, "tools/call", {{"name", "ask"}}, caps), c);
    REQUIRE(r1);
    const json& ir = (*r1)["result"];
    CHECK(ir["resultType"] == "input_required");
    CHECK(ir["inputRequests"]["name"]["method"] == "elicitation/create");
    REQUIRE(ir["requestState"].is_string());
    const std::string state = ir["requestState"];

    // The retry with the answer and the state completes.
    json retry{{"name", "ask"}, {"inputResponses", {{"name", {{"action", "accept"}, {"content", {{"name", "Ada"}}}}}}}, {"requestState", state}};
    auto r2 = s.handle(modernWith(2, "tools/call", retry, caps), c);
    REQUIRE(r2);
    CHECK((*r2)["result"]["resultType"] == "complete");
    CHECK((*r2)["result"]["content"][0]["text"] == "Hello Ada \"round-1\"");
    // Extra responses are ignored; a missing one asks again.
    retry["inputResponses"]["other"] = json::object();
    CHECK((*s.handle(modernWith(3, "tools/call", retry, caps), c))["result"]["resultType"] == "complete");
    json missing{{"name", "ask"}, {"inputResponses", {{"wrong", json::object()}}}};
    CHECK((*s.handle(modernWith(4, "tools/call", missing, caps), c))["result"]["resultType"] == "input_required");
    // Tampered, malformed or misplaced state is rejected; bad responses too.
    json tampered = retry;
    tampered["requestState"] = state.substr(0, state.size() - 2) + (state.back() == 'a' ? "bb" : "aa");
    CHECK(errorCode(s.handle(modernWith(5, "tools/call", tampered, caps), c)) == code::kInvalidParams);
    json other = retry;
    other["arguments"] = {{"x", 1}};   // a different request
    CHECK(errorCode(s.handle(modernWith(6, "tools/call", other, caps), c)) == code::kInvalidParams);
    json bad{{"name", "ask"}, {"inputResponses", "nope"}};
    CHECK(errorCode(s.handle(modernWith(7, "tools/call", bad, caps), c)) == code::kInvalidParams);
    json badValue{{"name", "ask"}, {"inputResponses", {{"name", 5}}}};
    CHECK(errorCode(s.handle(modernWith(8, "tools/call", badValue, caps), c)) == code::kInvalidParams);
    // Without the capability nothing is requested.
    auto r9 = s.handle(modernWith(9, "tools/call", {{"name", "ask"}}, json::object()), c);
    CHECK((*r9)["result"]["isError"] == true);
    CHECK_FALSE((*r9)["result"].contains("inputRequests"));
}

TEST_CASE("MCP legacy server-to-client requests", "[mcp]") {
    McpServer s = inputServer();
    Connection c;
    (void)s.handle(request(1, "initialize", {{"protocolVersion", "2025-11-25"}, {"capabilities", {{"elicitation", json::object()}}}}), c);
    json sent;
    Peer peer;
    peer.request = [&](const std::string& method, const json& params, const std::atomic<bool>*) -> Result<json> {
        sent = {{"method", method}, {"params", params}};
        return json{{"action", "accept"}, {"content", {{"name", "Lin"}}}};
    };
    auto r = s.handle(request(2, "tools/call", {{"name", "ask"}}), c, peer);
    REQUIRE(r);
    CHECK(sent["method"] == "elicitation/create");
    CHECK((*r)["result"]["content"][0]["text"].get<std::string>().rfind("Hello Lin", 0) == 0);
    peer.request = [](const std::string&, const json&, const std::atomic<bool>*) -> Result<json> {
        return Error{ErrorCode::Cancelled, "the client did not answer"};
    };
    auto r2 = s.handle(request(3, "tools/call", {{"name", "ask"}}), c, peer);
    CHECK((*r2)["result"]["isError"] == true);
}

TEST_CASE("MCP prompts, resources, templates, completions", "[mcp]") {
    McpServer s = inputServer();
    Connection c;
    auto caps = s.handle(modern(1, "server/discover"), c);
    CHECK((*caps)["result"]["capabilities"].contains("prompts"));
    CHECK((*caps)["result"]["capabilities"]["resources"]["subscribe"] == true);
    CHECK((*caps)["result"]["capabilities"].contains("completions"));
    auto pl = s.handle(modern(2, "prompts/list"), c);
    CHECK((*pl)["result"]["prompts"][0]["arguments"][0]["required"] == true);
    CHECK((*pl)["result"].contains("ttlMs"));
    auto pg = s.handle(modern(3, "prompts/get", {{"name", "greet"}, {"arguments", {{"who", "Ann"}}}}), c);
    CHECK((*pg)["result"]["messages"][0]["content"]["text"] == "Hi Ann");
    CHECK(errorCode(s.handle(modern(4, "prompts/get", {{"name", "greet"}}), c)) == code::kInvalidParams);   // missing argument
    CHECK(errorCode(s.handle(modern(5, "prompts/get", {{"name", "nope"}}), c)) == code::kInvalidParams);

    auto rl = s.handle(modern(6, "resources/list"), c);
    CHECK((*rl)["result"]["resources"][0]["uri"] == "test://a");
    auto rr = s.handle(modern(7, "resources/read", {{"uri", "test://a"}}), c);
    CHECK((*rr)["result"]["contents"][0]["text"] == "A!");
    CHECK((*rr)["result"]["ttlMs"] == 60000);
    CHECK((*rr)["result"]["cacheScope"] == "public");
    // The more specific template wins.
    auto t1 = s.handle(modern(8, "resources/read", {{"uri", "test://item/a/b.png"}}), c);
    CHECK((*t1)["result"]["contents"][0]["text"] == "item a/b.png");
    auto t2 = s.handle(modern(9, "resources/read", {{"uri", "test://item/a/b.png/size"}}), c);
    CHECK((*t2)["result"]["contents"][0]["text"] == "size of a/b.png");
    // Not found: -32602 with the uri (2026), -32002 (legacy).
    auto nf = s.handle(modern(10, "resources/read", {{"uri", "test://none"}}), c);
    CHECK(errorCode(nf) == code::kInvalidParams);
    CHECK((*nf)["error"]["data"]["uri"] == "test://none");
    Connection legacy;
    (void)s.handle(request(1, "initialize", {{"protocolVersion", "2025-11-25"}}), legacy);
    CHECK(errorCode(s.handle(request(2, "resources/read", {{"uri", "test://none"}}), legacy)) == code::kLegacyResourceNotFound);

    auto cp = s.handle(modern(11, "completion/complete", {{"ref", {{"type", "ref/prompt"}, {"name", "greet"}}}, {"argument", {{"name", "who"}, {"value", "w"}}}}), c);
    CHECK((*cp)["result"]["completion"]["values"] == json({"world", "wanda"}));
    CHECK(errorCode(s.handle(modern(12, "completion/complete", {{"ref", {{"type", "ref/prompt"}, {"name", "x"}}}}), c)) == code::kInvalidParams);

    // Legacy subscriptions and their notifications.
    CHECK(s.handle(request(3, "resources/subscribe", {{"uri", "test://a"}}), legacy)->contains("result"));
    CHECK(McpServer::legacyUpdateFor(legacy, "test://a"));
    CHECK_FALSE(McpServer::legacyUpdateFor(legacy, "test://b"));
    (void)s.handle(request(4, "resources/unsubscribe", {{"uri", "test://a"}}), legacy);
    CHECK_FALSE(McpServer::legacyUpdateFor(legacy, "test://a"));
}

TEST_CASE("MCP subscriptions/listen and URI templates", "[mcp]") {
    McpServer s = inputServer();
    json listen = modern(5, "subscriptions/listen", {{"notifications", {{"toolsListChanged", true}, {"resourceSubscriptions", {"test://a"}}}}});
    auto r = s.listenRequest(listen);
    REQUIRE(std::holds_alternative<McpServer::Listen>(r));
    auto& l = std::get<McpServer::Listen>(r);
    CHECK(l.acknowledgement["method"] == "notifications/subscriptions/acknowledged");
    CHECK(l.acknowledgement["params"]["_meta"][meta::kSubscriptionId] == 5);
    CHECK(l.acknowledgement["params"]["notifications"]["resourceSubscriptions"] == json({"test://a"}));
    auto n = McpServer::updateFor(l.sub, "test://a");
    REQUIRE(n);
    CHECK((*n)["params"]["_meta"][meta::kSubscriptionId] == 5);
    CHECK_FALSE(McpServer::updateFor(l.sub, "test://b"));
    CHECK(s.listenClosed(l.sub)["result"]["_meta"][meta::kSubscriptionId] == 5);
    CHECK(std::holds_alternative<json>(s.listenRequest(request(6, "subscriptions/listen"))));   // no envelope

    CHECK(matchUriTemplate("x://a/{id}/data", "x://a/12/data") == std::map<std::string, std::string>{{"id", "12"}});
    CHECK_FALSE(matchUriTemplate("x://a/{id}/data", "x://a/1/2/data"));   // {var} stops at '/'
    CHECK(matchUriTemplate("x://p/{+path}", "x://p/a/b/c.png") == std::map<std::string, std::string>{{"path", "a/b/c.png"}});
    CHECK(matchUriTemplate("x://p/{+path}/frame/{n}.png", "x://p/a/b.ace/frame/3.png") ==
          std::map<std::string, std::string>{{"path", "a/b.ace"}, {"n", "3"}});
    CHECK_FALSE(matchUriTemplate("x://p/{id}", "x://p/"));
}

TEST_CASE("MCP modern-era removed methods and envelope errors", "[mcp]") {
    McpServer s = inputServer();
    Connection c;
    // initialize carrying the 2026 envelope: a removed method.
    CHECK(errorCode(s.handle(modern(1, "initialize", {{"protocolVersion", "2025-11-25"}}), c)) == code::kMethodNotFound);
    for (const char* m : {"ping", "logging/setLevel", "resources/subscribe", "resources/unsubscribe"})
        CHECK(errorCode(s.handle(modern(2, m), c)) == code::kMethodNotFound);
    CHECK(McpServer::envelopeProblem(request(3, "tools/list", {{"_meta", {{meta::kProtocolVersion, kModernVersion}}}})));
    CHECK_FALSE(McpServer::envelopeProblem(modern(4, "tools/list")));
    // A tool can require a capability.
    std::vector<Tool> tools;
    tools.push_back({"needs", "", "", {{"type", "object"}}, {}, [](const json&, const ToolCall& call) {
                         return call.input->supports("sampling/createMessage") ? ToolResult::text("ok") : ToolResult::missingCapability("sampling");
                     }});
    McpServer s2({}, std::move(tools));
    auto r = s2.handle(modern(5, "tools/call", {{"name", "needs"}}), c);
    REQUIRE(errorCode(r) == code::kMissingRequiredClientCapability);
    CHECK((*r)["error"]["data"]["requiredCapabilities"].contains("sampling"));
}

namespace {

McpServer taskServer() {
    std::vector<Tool> tools;
    tools.push_back({"sync", "", "sync only", {{"type", "object"}}, {}, [](const json&, const ToolCall&) { return ToolResult::text("now"); }});
    tools.push_back({"job", "", "waits for a question, then finishes", {{"type", "object"}}, {},
                     [](const json&, const ToolCall& c) {
                         if (c.progress) c.progress(1, 2.0, "half way");
                         auto a = c.input->askAll({{"a", "elicitation/create", {{"message", "A?"}}}, {"b", "elicitation/create", {{"message", "B?"}}}});
                         if (!a[0] || !a[1]) return ToolResult::text("no answer", true);
                         return ToolResult::text("got " + (*a[0])["content"]["v"].get<std::string>() + (*a[1])["content"]["v"].get<std::string>());
                     },
                     "optional"});
    tools.push_back({"spin", "", "runs until cancelled", {{"type", "object"}}, {},
                     [](const json&, const ToolCall& c) {
                         while (!*c.cancel) std::this_thread::sleep_for(std::chrono::milliseconds(5));
                         return ToolResult::text("stopped");
                     },
                     "required"});
    tools.push_back({"bad", "", "protocol error", {{"type", "object"}}, {},
                     [](const json&, const ToolCall&) {
                         ToolResult r;
                         r.protocolError = json{{"code", -32603}, {"message", "broken"}};
                         return r;
                     },
                     "optional"});
    return McpServer({}, std::move(tools));
}

json waitFor(McpServer& s, Connection& c, const json& caps, const std::string& taskId, const std::string& status) {
    json r;
    for (int i = 0; i < 400; ++i) {
        r = (*s.handle(modernWith(900 + i, "tasks/get", {{"taskId", taskId}}, caps), c))["result"];
        if (r["status"] == status) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return r;
}

} // namespace

TEST_CASE("MCP tasks extension (io.modelcontextprotocol/tasks)", "[mcp]") {
    McpServer s = taskServer();
    Connection c;
    const json tasks{{"elicitation", json::object()}, {"extensions", {{kTasksExtension, json::object()}}}};
    const json plain{{"elicitation", json::object()}};

    auto disc = s.handle(modern(1, "server/discover"), c);
    CHECK((*disc)["result"]["capabilities"]["extensions"].contains(kTasksExtension));
    auto list = (*s.handle(modern(2, "tools/list"), c))["result"]["tools"];
    for (const auto& t : list) {
        if (t["name"] == "sync") CHECK_FALSE(t.contains("execution"));
        if (t["name"] == "spin") CHECK(t["execution"]["taskSupport"] == "required");
    }

    // Without the extension: sync results, -32021 for required tools and tasks/*.
    CHECK((*s.handle(modernWith(3, "tools/call", {{"name", "sync"}}, tasks), c))["result"]["resultType"] == "complete");
    CHECK(errorCode(s.handle(modernWith(4, "tools/call", {{"name", "spin"}}, plain), c)) == code::kMissingRequiredClientCapability);
    CHECK(errorCode(s.handle(modernWith(5, "tasks/get", {{"taskId", "x"}}, plain), c)) == code::kMissingRequiredClientCapability);
    CHECK(errorCode(s.handle(modernWith(6, "tasks/get", {{"taskId", "x"}}, tasks), c)) == code::kInvalidParams);
    CHECK(errorCode(s.handle(modernWith(7, "tasks/list", json::object(), tasks), c)) == code::kMethodNotFound);

    // A task that asks two questions; partial answers keep it waiting.
    auto created = (*s.handle(modernWith(10, "tools/call", {{"name", "job"}}, tasks), c))["result"];
    REQUIRE(created["resultType"] == "task");
    CHECK_FALSE(created.contains("requestState"));
    CHECK(created["ttlMs"].is_number_integer());
    const std::string id = created["taskId"];
    auto waiting = waitFor(s, c, tasks, id, "input_required");
    REQUIRE(waiting["status"] == "input_required");
    CHECK(waiting["inputRequests"].size() == 2);
    auto ack = (*s.handle(modernWith(11, "tasks/update", {{"taskId", id}, {"inputResponses", {{"a", {{"action", "accept"}, {"content", {{"v", "1"}}}}}}}}, tasks), c))["result"];
    CHECK(ack["resultType"] == "complete");
    CHECK_FALSE(ack.contains("taskId"));
    auto partial = waitFor(s, c, tasks, id, "input_required");
    CHECK(partial["inputRequests"].size() == 1);
    CHECK(partial["inputRequests"].contains("b"));
    s.handle(modernWith(12, "tasks/update", {{"taskId", id}, {"inputResponses", {{"b", {{"action", "accept"}, {"content", {{"v", "2"}}}}}}}}, tasks), c);
    auto done = waitFor(s, c, tasks, id, "completed");
    REQUIRE(done["status"] == "completed");
    CHECK(done["resultType"] == "complete");
    CHECK(done["result"]["content"][0]["text"] == "got 12");
    // Cancelling a finished task is a no-op.
    CHECK((*s.handle(modernWith(13, "tasks/cancel", {{"taskId", id}}, tasks), c))["result"]["resultType"] == "complete");
    CHECK(waitFor(s, c, tasks, id, "completed")["status"] == "completed");

    // Cancellation while running; protocol errors fail the task.
    const std::string spin = (*s.handle(modernWith(20, "tools/call", {{"name", "spin"}}, tasks), c))["result"]["taskId"];
    s.handle(modernWith(21, "tasks/cancel", {{"taskId", spin}}, tasks), c);
    CHECK(waitFor(s, c, tasks, spin, "cancelled")["status"] == "cancelled");
    const std::string bad = (*s.handle(modernWith(22, "tools/call", {{"name", "bad"}}, tasks), c))["result"]["taskId"];
    auto failed = waitFor(s, c, tasks, bad, "failed");
    CHECK(failed["error"]["code"] == -32603);
    CHECK_FALSE(failed.contains("result"));
}
