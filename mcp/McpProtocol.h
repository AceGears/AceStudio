#pragma once

// The MCP protocol layer of acestudio-mcp (Phase 7), independent of the
// transport. Dual-era:
//
//  * Modern, 2026-07-28: stateless. Every request carries
//    `_meta["io.modelcontextprotocol/protocolVersion"]` and
//    `_meta["io.modelcontextprotocol/clientCapabilities"]`; `server/discover`
//    advertises versions, capabilities and identity; results carry
//    `resultType` and `_meta["io.modelcontextprotocol/serverInfo"]`; list
//    results carry `ttlMs` / `cacheScope`. No `ping` or `logging/setLevel`.
//  * Legacy, 2025-11-25 and earlier: the `initialize` handshake, `ping`,
//    `logging/setLevel`.
//
// On stdio the first request decides the era of the connection (as in the
// reference SDKs): an `initialize` opens a legacy connection, an enveloped
// request a modern one. Over HTTP, each modern request is its own
// connection and legacy clients get a session per `initialize`.

#include <nlohmann/json.hpp>

#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace acestudio::mcp {

using nlohmann::json;

inline constexpr const char* kModernVersion = "2026-07-28";
inline const std::vector<std::string>& legacyVersions();   // newest first
inline const std::vector<std::string>& supportedVersions(); // modern, then legacy

namespace meta {
inline constexpr const char* kProtocolVersion = "io.modelcontextprotocol/protocolVersion";
inline constexpr const char* kClientCapabilities = "io.modelcontextprotocol/clientCapabilities";
inline constexpr const char* kClientInfo = "io.modelcontextprotocol/clientInfo";
inline constexpr const char* kServerInfo = "io.modelcontextprotocol/serverInfo";
inline constexpr const char* kLogLevel = "io.modelcontextprotocol/logLevel";
} // namespace meta

namespace code {
inline constexpr int kParseError = -32700;
inline constexpr int kInvalidRequest = -32600;
inline constexpr int kMethodNotFound = -32601;
inline constexpr int kInvalidParams = -32602;
inline constexpr int kInternalError = -32603;
inline constexpr int kHeaderMismatch = -32020;
inline constexpr int kUnsupportedProtocolVersion = -32022;
} // namespace code

// One running tool call.
struct ToolCall {
    const std::atomic<bool>* cancel = nullptr;
    // Sends notifications/progress when the client asked for progress.
    std::function<void(double progress, std::optional<double> total, const std::string& message)> progress;
    // Sends notifications/message when the client asked for this level
    // (legacy: logging/setLevel; modern: _meta logLevel on the request).
    std::function<void(const std::string& level, const std::string& message)> log;
};

// Content blocks for tools.
json textBlock(const std::string& s);
json imageBlock(const std::string& base64, const std::string& mimeType = "image/png");

// What a tool returns: MCP content blocks plus optional structured content.
struct ToolResult {
    json content = json::array();
    std::optional<json> structured;
    bool isError = false;

    static ToolResult text(const std::string& s, bool error = false);
    ToolResult& addText(const std::string& s);
    ToolResult& addImage(const std::string& base64Png);
};

struct Tool {
    std::string name;
    std::string title;
    std::string description;
    json inputSchema;
    json annotations = json::object();   // readOnlyHint, destructiveHint, ...
    std::function<ToolResult(const json& args, const ToolCall& call)> run;
};

struct ServerIdentity {
    std::string name = "acestudio";
    std::string title = "AceStudio";
    std::string version;
    std::string instructions;
};

// State of one connection (a stdio process, a legacy HTTP session, or one
// modern HTTP request).
struct Connection {
    enum class Era { Undecided, Legacy, Modern };
    Era era = Era::Undecided;
    std::string legacyVersion;   // negotiated by initialize
    bool initialized = false;    // notifications/initialized received
    std::string logLevel = "info";
};

// Messages the server sends that are not the response (progress, logs).
using Notify = std::function<void(const json& notification)>;

class McpServer {
public:
    McpServer(ServerIdentity id, std::vector<Tool> tools);

    // Handles one JSON-RPC message. Returns the response for requests, or
    // nullopt for notifications. `cancel` is the flag of this request (set by
    // the transport on notifications/cancelled or a closed HTTP stream).
    std::optional<json> handle(const json& message, Connection& c, const Notify& notify = {},
                               const std::atomic<bool>* cancel = nullptr);

    // For notifications/cancelled: the request id it names (stdio transport).
    static std::optional<json> cancelledRequestId(const json& message);

    // The error response for a message that is not valid JSON.
    static json parseError(const std::string& detail);
    static json errorResponse(const json& id, int code, const std::string& message, const json& data = nullptr);

    const std::vector<Tool>& tools() const { return m_tools; }
    const ServerIdentity& identity() const { return m_id; }

    // Is the request a modern (enveloped) one? (Used by the HTTP transport.)
    static bool hasModernEnvelope(const json& message);

private:
    json capabilities(bool modern) const;
    json serverInfo() const;
    json toolsList(bool modern) const;
    json callTool(const json& params, const json& requestMeta, const Notify& notify, const std::atomic<bool>* cancel,
                  const json& id, std::optional<std::string> logThreshold);
    std::optional<json> handleLegacy(const json& msg, Connection& c, const Notify& notify, const std::atomic<bool>* cancel);
    std::optional<json> handleModern(const json& msg, Connection& c, const Notify& notify, const std::atomic<bool>* cancel);

    ServerIdentity m_id;
    std::vector<Tool> m_tools;   // sorted by name: tools/list is deterministic
};

// ---- inline definitions

inline const std::vector<std::string>& legacyVersions() {
    static const std::vector<std::string> v{"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};
    return v;
}
inline const std::vector<std::string>& supportedVersions() {
    static const std::vector<std::string> v{kModernVersion, "2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};
    return v;
}

} // namespace acestudio::mcp
