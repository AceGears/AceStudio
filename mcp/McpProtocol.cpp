#include "McpProtocol.h"

#include <algorithm>

namespace acestudio::mcp {

namespace {

bool isModernVersion(const std::string& v) { return v == kModernVersion; }

bool isLegacyVersion(const std::string& v) {
    const auto& l = legacyVersions();
    return std::find(l.begin(), l.end(), v) != l.end();
}

const json& paramsOf(const json& msg) {
    static const json empty = json::object();
    auto it = msg.find("params");
    return it != msg.end() && it->is_object() ? *it : empty;
}

const json& metaOf(const json& params) {
    static const json empty = json::object();
    auto it = params.find("_meta");
    return it != params.end() && it->is_object() ? *it : empty;
}

json okResponse(const json& id, json result) { return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}}; }

// Minimal argument check against a tool's inputSchema: required keys and the
// JSON type of each declared property. Problems are tool errors (the model
// can correct them), not protocol errors.
std::optional<std::string> checkArguments(const json& schema, const json& args) {
    if (!args.is_object()) return "arguments must be an object";
    if (auto req = schema.find("required"); req != schema.end() && req->is_array())
        for (const auto& k : *req)
            if (k.is_string() && !args.contains(k.get<std::string>())) return "missing required argument '" + k.get<std::string>() + "'";
    const auto props = schema.find("properties");
    if (props == schema.end() || !props->is_object()) return std::nullopt;
    for (const auto& [key, value] : args.items()) {
        auto p = props->find(key);
        if (p == props->end()) {
            if (schema.value("additionalProperties", true) == false) return "unknown argument '" + key + "'";
            continue;
        }
        const std::string type = p->value("type", std::string());
        if (value.is_null()) continue;
        bool ok = true;
        if (type == "string") ok = value.is_string();
        else if (type == "integer") ok = value.is_number_integer() || (value.is_number_float() && value.get<double>() == static_cast<double>(static_cast<long long>(value.get<double>())));
        else if (type == "number") ok = value.is_number();
        else if (type == "boolean") ok = value.is_boolean();
        else if (type == "array") ok = value.is_array();
        else if (type == "object") ok = value.is_object();
        if (!ok) return "argument '" + key + "' must be " + (type == "integer" ? "an integer" : type == "array" || type == "object" ? "an " + type : "a " + type);
        if (auto e = p->find("enum"); e != p->end() && e->is_array() && std::find(e->begin(), e->end(), value) == e->end())
            return "argument '" + key + "' must be one of " + e->dump();
    }
    return std::nullopt;
}

int levelRank(const std::string& l) {
    static const std::vector<std::string> levels{"debug", "info", "notice", "warning", "error", "critical", "alert", "emergency"};
    const auto it = std::find(levels.begin(), levels.end(), l);
    return it == levels.end() ? -1 : static_cast<int>(it - levels.begin());
}

} // namespace

json textBlock(const std::string& s) { return {{"type", "text"}, {"text", s}}; }
json imageBlock(const std::string& b64, const std::string& mimeType) { return {{"type", "image"}, {"data", b64}, {"mimeType", mimeType}}; }

ToolResult ToolResult::text(const std::string& s, bool error) {
    ToolResult r;
    r.addText(s);
    r.isError = error;
    return r;
}

ToolResult& ToolResult::addText(const std::string& s) {
    content.push_back(textBlock(s));
    return *this;
}

ToolResult& ToolResult::addImage(const std::string& base64Png) {
    content.push_back(imageBlock(base64Png));
    return *this;
}

McpServer::McpServer(ServerIdentity id, std::vector<Tool> tools) : m_id(std::move(id)), m_tools(std::move(tools)) {
    std::sort(m_tools.begin(), m_tools.end(), [](const Tool& a, const Tool& b) { return a.name < b.name; });
}

json McpServer::errorResponse(const json& id, int c, const std::string& message, const json& data) {
    json e{{"code", c}, {"message", message}};
    if (!data.is_null()) e["data"] = data;
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"error", e}};
}

json McpServer::parseError(const std::string& detail) { return errorResponse(nullptr, code::kParseError, "Parse error: " + detail); }

bool McpServer::hasModernEnvelope(const json& message) {
    if (!message.is_object()) return false;
    return metaOf(paramsOf(message)).contains(meta::kProtocolVersion);
}

std::optional<json> McpServer::cancelledRequestId(const json& message) {
    if (!message.is_object() || message.value("method", std::string()) != "notifications/cancelled" || message.contains("id"))
        return std::nullopt;
    const json& p = paramsOf(message);
    if (auto it = p.find("requestId"); it != p.end()) return *it;
    return std::nullopt;
}

json McpServer::serverInfo() const {
    json info{{"name", m_id.name}, {"version", m_id.version}};
    if (!m_id.title.empty()) info["title"] = m_id.title;
    return info;
}

json McpServer::capabilities(bool modern) const {
    json caps{{"tools", {{"listChanged", false}}}};
    if (!modern) caps["logging"] = json::object();   // legacy only: logging/setLevel
    return caps;
}

json McpServer::toolsList(bool modern) const {
    json list = json::array();
    for (const Tool& t : m_tools) {
        json j{{"name", t.name}, {"description", t.description}, {"inputSchema", t.inputSchema}};
        if (!t.title.empty()) j["title"] = t.title;
        if (!t.annotations.empty()) j["annotations"] = t.annotations;
        list.push_back(std::move(j));
    }
    json r{{"tools", std::move(list)}};
    if (modern) {
        // The tool set is fixed for a given build.
        r["ttlMs"] = 3600000;
        r["cacheScope"] = "public";
    }
    return r;
}

json McpServer::callTool(const json& params, const json& requestMeta, const Notify& notify, const std::atomic<bool>* cancel,
                         const json& id, std::optional<std::string> logThreshold) {
    const auto nameIt = params.find("name");
    if (nameIt == params.end() || !nameIt->is_string()) return errorResponse(id, code::kInvalidParams, "tools/call needs a tool name");
    const std::string name = nameIt->get<std::string>();
    const auto tool = std::find_if(m_tools.begin(), m_tools.end(), [&](const Tool& t) { return t.name == name; });
    if (tool == m_tools.end()) return errorResponse(id, code::kInvalidParams, "Unknown tool: " + name);
    json args = params.value("arguments", json::object());
    if (args.is_null()) args = json::object();

    ToolResult result;
    if (auto problem = checkArguments(tool->inputSchema, args)) {
        result = ToolResult::text("Invalid arguments for " + name + ": " + *problem, true);
    } else {
        ToolCall call;
        call.cancel = cancel;
        if (auto token = requestMeta.find("progressToken"); token != requestMeta.end() && notify) {
            const json tok = *token;
            call.progress = [notify, tok](double progress, std::optional<double> total, const std::string& message) {
                json p{{"progressToken", tok}, {"progress", progress}};
                if (total) p["total"] = *total;
                if (!message.empty()) p["message"] = message;
                notify(json{{"jsonrpc", "2.0"}, {"method", "notifications/progress"}, {"params", p}});
            };
        }
        if (logThreshold && notify && levelRank(*logThreshold) >= 0) {
            const int min = levelRank(*logThreshold);
            call.log = [notify, min](const std::string& level, const std::string& message) {
                if (levelRank(level) < min) return;
                notify(json{{"jsonrpc", "2.0"}, {"method", "notifications/message"},
                            {"params", {{"level", level}, {"logger", "acestudio"}, {"data", message}}}});
            };
        }
        try {
            result = tool->run(args, call);
        } catch (const std::exception& e) {
            result = ToolResult::text(std::string("The tool failed: ") + e.what(), true);
        }
    }
    json r{{"content", result.content}, {"isError", result.isError}};
    if (result.structured) r["structuredContent"] = *result.structured;
    return okResponse(id, std::move(r));
}

std::optional<json> McpServer::handle(const json& msg, Connection& c, const Notify& notify, const std::atomic<bool>* cancel) {
    if (!msg.is_object() || msg.value("jsonrpc", std::string()) != "2.0" || !msg.contains("method") || !msg["method"].is_string()) {
        if (msg.is_object() && (msg.contains("result") || msg.contains("error"))) return std::nullopt;   // a response: we send no requests
        const json id = msg.is_object() && msg.contains("id") ? msg["id"] : json(nullptr);
        return errorResponse(id, code::kInvalidRequest, "Not a JSON-RPC 2.0 request");
    }
    const bool isRequest = msg.contains("id");
    if (isRequest && !(msg["id"].is_string() || msg["id"].is_number_integer()))
        return errorResponse(nullptr, code::kInvalidRequest, "The request id must be a string or an integer");
    const std::string method = msg["method"].get<std::string>();

    if (!isRequest) {
        // Notifications: initialized (legacy) and cancelled are the only ones
        // that matter; the transport acts on cancelled.
        if (method == "notifications/initialized" && c.era == Connection::Era::Legacy) c.initialized = true;
        return std::nullopt;
    }

    const bool enveloped = hasModernEnvelope(msg);
    if (c.era == Connection::Era::Undecided) {
        if (method == "initialize") c.era = Connection::Era::Legacy;
        else if (enveloped) c.era = Connection::Era::Modern;
        else if (method == "ping") return okResponse(msg["id"], json::object());
        else
            return errorResponse(msg["id"], code::kInvalidRequest,
                                 "Send initialize first (protocol 2025-11-25 and earlier), or include "
                                 "_meta[\"" + std::string(meta::kProtocolVersion) + "\"] (protocol 2026-07-28)");
    }
    return c.era == Connection::Era::Legacy ? handleLegacy(msg, c, notify, cancel) : handleModern(msg, c, notify, cancel);
}

std::optional<json> McpServer::handleLegacy(const json& msg, Connection& c, const Notify& notify, const std::atomic<bool>* cancel) {
    const json& id = msg["id"];
    const std::string method = msg["method"].get<std::string>();
    const json& params = paramsOf(msg);
    if (method != "initialize" && hasModernEnvelope(msg))
        return errorResponse(id, code::kInvalidRequest,
                             "This connection uses the initialize handshake (protocol " + c.legacyVersion +
                                 "); requests with the 2026-07-28 envelope are not accepted on it");
    if (method == "initialize") {
        if (!c.legacyVersion.empty()) return errorResponse(id, code::kInvalidRequest, "initialize was already received");
        const std::string requested = params.value("protocolVersion", std::string());
        // Echo a version we support, else offer our newest legacy version.
        c.legacyVersion = isLegacyVersion(requested) ? requested : legacyVersions().front();
        json r{{"protocolVersion", c.legacyVersion}, {"capabilities", capabilities(false)}, {"serverInfo", serverInfo()}};
        if (!m_id.instructions.empty()) r["instructions"] = m_id.instructions;
        return okResponse(id, std::move(r));
    }
    if (method == "ping") return okResponse(id, json::object());
    if (c.legacyVersion.empty()) return errorResponse(id, code::kInvalidRequest, "Send initialize first");
    if (method == "logging/setLevel") {
        static const std::vector<std::string> levels{"debug", "info", "notice", "warning", "error", "critical", "alert", "emergency"};
        const std::string level = params.value("level", std::string());
        if (std::find(levels.begin(), levels.end(), level) == levels.end())
            return errorResponse(id, code::kInvalidParams, "Unknown log level '" + level + "'");
        c.logLevel = level;
        return okResponse(id, json::object());
    }
    if (method == "tools/list") return okResponse(id, toolsList(false));
    if (method == "tools/call") return callTool(params, metaOf(params), notify, cancel, id, c.logLevel);
    return errorResponse(id, code::kMethodNotFound, "Method not found: " + method);
}

std::optional<json> McpServer::handleModern(const json& msg, Connection&, const Notify& notify, const std::atomic<bool>* cancel) {
    const json& id = msg["id"];
    const std::string method = msg["method"].get<std::string>();
    const json& params = paramsOf(msg);
    const json& m = metaOf(params);
    if (method == "initialize") {
        json data{{"supported", json::array({kModernVersion})}};
        if (params.contains("protocolVersion") && params["protocolVersion"].is_string()) data["requested"] = params["protocolVersion"];
        return errorResponse(id, code::kUnsupportedProtocolVersion,
                             "This connection uses protocol 2026-07-28; the initialize handshake is not accepted on it", data);
    }
    const auto ver = m.find(meta::kProtocolVersion);
    if (ver == m.end() || !ver->is_string())
        return errorResponse(id, code::kInvalidParams, std::string("Missing _meta[\"") + meta::kProtocolVersion + "\"]");
    const std::string version = ver->get<std::string>();
    if (!isModernVersion(version))
        return errorResponse(id, code::kUnsupportedProtocolVersion, "Unsupported protocol version",
                             json{{"supported", supportedVersions()}, {"requested", version}});
    const auto caps = m.find(meta::kClientCapabilities);
    if (caps == m.end() || !caps->is_object())
        return errorResponse(id, code::kInvalidParams, std::string("Missing _meta[\"") + meta::kClientCapabilities + "\"]");

    std::optional<json> response;
    if (method == "server/discover") {
        json r{{"supportedVersions", supportedVersions()}, {"capabilities", capabilities(true)}, {"ttlMs", 3600000}, {"cacheScope", "public"}};
        if (!m_id.instructions.empty()) r["instructions"] = m_id.instructions;
        response = okResponse(id, std::move(r));
    } else if (method == "tools/list") {
        response = okResponse(id, toolsList(true));
    } else if (method == "tools/call") {
        // Modern: log notifications only when this request asked for them.
        std::optional<std::string> threshold;
        if (auto l = m.find(meta::kLogLevel); l != m.end() && l->is_string()) threshold = l->get<std::string>();
        response = callTool(params, m, notify, cancel, id, threshold);
    } else {
        return errorResponse(id, code::kMethodNotFound, "Method not found: " + method);
    }
    // Modern results: resultType and the serverInfo stamp.
    if (response && response->contains("result")) {
        json& r = (*response)["result"];
        if (!r.contains("resultType")) r["resultType"] = "complete";
        json& rm = r["_meta"];
        if (!rm.is_object()) rm = json::object();
        rm[meta::kServerInfo] = serverInfo();
    }
    return response;
}

} // namespace acestudio::mcp
