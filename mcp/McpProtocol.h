#pragma once

// The MCP protocol layer of acestudio-mcp (Phase 7), independent of the
// transport. Dual-era:
//
//  * Modern, 2026-07-28: stateless. Every request carries
//    `_meta["io.modelcontextprotocol/protocolVersion"]` and
//    `_meta["io.modelcontextprotocol/clientCapabilities"]`; `server/discover`
//    advertises versions, capabilities and identity; results carry
//    `resultType` and `_meta["io.modelcontextprotocol/serverInfo"]`;
//    cacheable results carry `ttlMs` / `cacheScope`. Input from the client
//    (elicitation, sampling, roots) uses multi round-trip requests: an
//    `input_required` result, then a retry with `inputResponses` and the
//    integrity-protected `requestState`. Change notifications flow on
//    `subscriptions/listen` streams.
//  * Legacy, 2025-11-25 and earlier: the `initialize` handshake, `ping`,
//    `logging/setLevel`, `resources/subscribe`, and server-to-client
//    requests sent while a request is being handled.
//
// On stdio the first request decides the era of the connection (as in the
// reference SDKs): an `initialize` opens a legacy connection, an enveloped
// request a modern one. Over HTTP, each modern request is its own
// connection and legacy clients get a session per `initialize`.

#include "acestudio/core/Result.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace acestudio::mcp {

using nlohmann::json;

inline constexpr const char* kModernVersion = "2026-07-28";
const std::vector<std::string>& legacyVersions();     // newest first
const std::vector<std::string>& supportedVersions();  // modern, then legacy

namespace meta {
inline constexpr const char* kProtocolVersion = "io.modelcontextprotocol/protocolVersion";
inline constexpr const char* kClientCapabilities = "io.modelcontextprotocol/clientCapabilities";
inline constexpr const char* kClientInfo = "io.modelcontextprotocol/clientInfo";
inline constexpr const char* kServerInfo = "io.modelcontextprotocol/serverInfo";
inline constexpr const char* kLogLevel = "io.modelcontextprotocol/logLevel";
inline constexpr const char* kSubscriptionId = "io.modelcontextprotocol/subscriptionId";
} // namespace meta

namespace code {
inline constexpr int kParseError = -32700;
inline constexpr int kInvalidRequest = -32600;
inline constexpr int kMethodNotFound = -32601;
inline constexpr int kInvalidParams = -32602;
inline constexpr int kInternalError = -32603;
inline constexpr int kLegacyResourceNotFound = -32002;   // 2025-11-25 and earlier
inline constexpr int kHeaderMismatch = -32020;
inline constexpr int kMissingRequiredClientCapability = -32021;
inline constexpr int kUnsupportedProtocolVersion = -32022;
} // namespace code

// Sends one server-to-client request and waits for the client's result
// (legacy era only). Errors: the client returned an error, the transport
// cannot carry requests, it was cancelled or timed out.
using Requester = std::function<Result<json>(const std::string& method, const json& params, const std::atomic<bool>* cancel)>;

// Input from the client while a request is handled: elicitation, sampling or
// roots. One API for both eras:
//  * legacy: ask() sends the request now and returns the client's result;
//  * modern: ask() returns the result from `inputResponses` if the client
//    sent it, else records the request and returns nullopt. A handler that
//    got nullopt returns early; the server then answers `input_required`
//    with everything recorded, and the client retries.
//  * task (tasks extension): ask() parks the task in input_required until
//    the client answers with tasks/update.
struct InputRequest {
    std::string key, method;
    json params;
};
// Waits for answers to several requests at once (a task); nullopt entries
// were not answered (cancelled).
using TaskWaiter = std::function<std::vector<std::optional<json>>(const std::vector<InputRequest>&)>;

class InputContext {
public:
    InputContext(json clientCapabilities, bool modern, json inputResponses, json state, Requester requester,
                 const std::atomic<bool>* cancel);

    // method: "elicitation/create", "sampling/createMessage" or "roots/list".
    std::optional<json> ask(const std::string& key, const std::string& method, const json& params);
    // Several requests at once (a task shows them all as pending together).
    std::vector<std::optional<json>> askAll(const std::vector<InputRequest>& requests);
    void setTaskWaiter(TaskWaiter w) { m_taskWaiter = std::move(w); }
    bool inTask() const { return static_cast<bool>(m_taskWaiter); }
    bool supports(const std::string& method) const;
    bool modern() const { return m_modern; }
    const json& capabilities() const { return m_caps; }

    // State carried to the next round (modern), verified when it comes back.
    const json& state() const { return m_stateIn; }
    void setState(json s) { m_stateOut = std::move(s); }

    bool needsInput() const { return !m_pending.empty(); }
    const json& pending() const { return m_pending; }
    const json& nextState() const { return m_stateOut; }
    const std::string& error() const { return m_error; }   // a legacy request failed

private:
    json m_caps;
    bool m_modern;
    json m_responses;
    json m_stateIn;
    json m_stateOut;
    Requester m_requester;
    const std::atomic<bool>* m_cancel;
    json m_pending = json::object();
    std::string m_error;
    TaskWaiter m_taskWaiter;
};

// One running tool call.
struct ToolCall {
    const std::atomic<bool>* cancel = nullptr;
    // Sends notifications/progress when the client asked for progress.
    std::function<void(double progress, std::optional<double> total, const std::string& message)> progress;
    // Sends notifications/message when the client asked for this level
    // (legacy: logging/setLevel; modern: _meta logLevel on the request).
    std::function<void(const std::string& level, const std::string& message)> log;
    InputContext* input = nullptr;
};

// Content blocks.
json textBlock(const std::string& s);
json imageBlock(const std::string& base64, const std::string& mimeType = "image/png");

// What a tool returns: MCP content blocks plus optional structured content.
struct ToolResult {
    json content = json::array();
    std::optional<json> structured;
    bool isError = false;
    // A protocol error instead of a result: {code, message, data?}
    // (e.g. MissingRequiredClientCapability).
    std::optional<json> protocolError;
    // Work to continue as a task (tasks extension): when set, the server
    // creates a task that runs it; without the extension it runs now.
    std::function<ToolResult(const struct ToolCall&)> asyncWork;

    static ToolResult missingCapability(const std::string& capability);

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
    // Tasks extension: "" (never a task), "optional" (a task when the client
    // supports tasks) or "required" (-32021 for clients that do not).
    std::string taskSupport = {};
    // Run the handler first (it may collect MRTR input) and create a task
    // only when it returns asyncWork; otherwise the whole call is the task.
    bool inputBeforeTask = false;
};

inline constexpr const char* kTasksExtension = "io.modelcontextprotocol/tasks";

// Suggestions for an argument: (argument name, typed prefix, other arguments).
using Completer = std::function<std::vector<std::string>(const std::string& name, const std::string& value, const json& context)>;

struct PromptArgument {
    std::string name;
    std::string description;
    bool required = false;
};

struct Prompt {
    std::string name;
    std::string title;
    std::string description;
    std::vector<PromptArgument> arguments;
    // Returns {"messages": [...], "description"?: ...}. InvalidArgument errors
    // become -32602.
    std::function<Result<json>(const json& args, InputContext& input)> get;
    Completer complete;
};

struct Resource {
    std::string uri;
    std::string name;
    std::string title;
    std::string description;
    std::string mimeType;
    // Returns the "contents" array. NotFound errors become "resource not found".
    std::function<Result<json>(const std::string& uri, InputContext& input)> read;
    int ttlMs = 0;                 // freshness hint (modern)
    bool publicCache = false;      // cacheScope "public" or "private"
};

struct ResourceTemplate {
    std::string uriTemplate;       // RFC 6570 level 1-2: {var} and {+var}
    std::string name;
    std::string title;
    std::string description;
    std::string mimeType;
    std::function<Result<json>(const std::string& uri, const std::map<std::string, std::string>& vars, InputContext& input)> read;
    Completer complete;
    int ttlMs = 0;
    bool publicCache = false;
};

// Matches `uri` against a template; returns the variables or nullopt.
std::optional<std::map<std::string, std::string>> matchUriTemplate(const std::string& uriTemplate, const std::string& uri);

struct ServerIdentity {
    std::string name = "acestudio";
    std::string title = "AceStudio";
    std::string version;
    std::string instructions;
};

// A subscriptions/listen stream (modern).
struct Subscription {
    json id;                       // the listen request's id
    bool toolsListChanged = false;
    bool promptsListChanged = false;
    bool resourcesListChanged = false;
    bool tasks = false;            // notifications/tasks (tasks extension)
    std::set<std::string> resources;
};

// State of one connection (a stdio process, a legacy HTTP session, or one
// modern HTTP request).
struct Connection {
    enum class Era { Undecided, Legacy, Modern };
    Era era = Era::Undecided;
    std::string legacyVersion;     // negotiated by initialize
    bool initialized = false;      // notifications/initialized received
    std::string logLevel = "info";
    json clientCapabilities = json::object();   // legacy: from initialize
    std::set<std::string> subscribed;           // legacy resources/subscribe
};

// Messages the server sends that are not the response (progress, logs).
using Notify = std::function<void(const json& notification)>;

// What the transport gives the server for one request.
struct Peer {
    Notify notify;                 // request-scoped notifications
    Requester request;             // legacy server-to-client requests (may be empty)
    const std::atomic<bool>* cancel = nullptr;
};

class McpServer {
public:
    McpServer(ServerIdentity id, std::vector<Tool> tools, std::vector<Prompt> prompts = {}, std::vector<Resource> resources = {},
              std::vector<ResourceTemplate> templates = {});
    ~McpServer();
    McpServer(const McpServer&) = delete;
    McpServer& operator=(const McpServer&) = delete;

    // Handles one JSON-RPC message. Returns the response for requests, or
    // nullopt for notifications. subscriptions/listen is the transport's
    // job (see listenRequest()); everything else is handled here.
    std::optional<json> handle(const json& message, Connection& c, const Peer& peer = {});

    // ---- subscriptions/listen (modern): the transport keeps the stream.
    // Returns the subscription and the acknowledgement notification, or an
    // error response.
    struct Listen {
        Subscription sub;
        json acknowledgement;
    };
    std::variant<Listen, json> listenRequest(const json& message);
    // The graceful end of a listen stream.
    json listenClosed(const Subscription& s) const;
    // A resources/updated notification for a subscriber, or nullopt.
    static std::optional<json> updateFor(const Subscription& s, const std::string& uri);
    static std::optional<json> legacyUpdateFor(const Connection& c, const std::string& uri);

    // ---- tasks extension
    // Keeps every task in <dir>/<taskId>.json as well, so tasks/get still
    // answers after a restart and from another acestudio-mcp sharing the
    // folder (see McpProtocol.cpp). Without it tasks live in memory only.
    void setTaskDirectory(const std::filesystem::path& dir);
    // Called with the DetailedTask after every status change (any thread).
    using TaskObserver = std::function<void(const json& task)>;
    void setTaskObserver(TaskObserver o);
    // Tasks of this process still running or waiting for input.
    int activeTasks() const;
    // Waits until none is; false on timeout.
    bool waitForTasks(std::chrono::milliseconds timeout) const;
    // A notifications/tasks for a listen stream that asked for them, or nullopt.
    static std::optional<json> taskUpdateFor(const Subscription& s, const json& task);

    // For notifications/cancelled: the request id it names.
    static std::optional<json> cancelledRequestId(const json& message);
    // Is this a response to a server-to-client request? Returns its id.
    static std::optional<json> responseId(const json& message);

    static json parseError(const std::string& detail);
    static json errorResponse(const json& id, int code, const std::string& message, const json& data = nullptr);
    static bool hasModernEnvelope(const json& message);
    // For a modern request: the envelope error (missing _meta fields,
    // unsupported version) or nullopt. HTTP answers these with 400.
    static std::optional<json> envelopeProblem(const json& message);
    // Methods that change the connection's state (legacy) and must be handled
    // in order, not on a worker thread.
    static bool changesConnection(const std::string& method);

    const std::vector<Tool>& tools() const { return m_tools; }
    const ServerIdentity& identity() const { return m_id; }

private:
    json capabilities(bool modern) const;
    bool offers(const std::string& method) const;   // prompts/* and resources/* only when advertised
    json serverInfo() const;
    json toolsList(bool modern) const;
    json promptsList(bool modern) const;
    json resourcesList(bool modern) const;
    json templatesList(bool modern) const;

    // Handlers shared by both eras. `modern` adds the 2026 fields. They
    // return the response (result or error).
    json callTool(const json& id, const json& params, bool modern, InputContext& in, const Peer& peer, std::optional<std::string> logThreshold);
    json getPrompt(const json& id, const json& params, bool modern, InputContext& in);
    json readResource(const json& id, const json& params, bool modern, InputContext& in);
    json complete(const json& id, const json& params) const;
    // An InputRequiredResult for `in`'s pending requests, with signed state.
    json inputRequired(const json& id, const std::string& method, const json& params, const InputContext& in) const;
    // Verifies a requestState; returns its payload, or an error message.
    std::variant<json, std::string> verifyState(const std::string& state, const std::string& method, const json& params) const;

    std::optional<json> handleChecked(const json& msg, Connection& c, const Peer& peer);
    std::variant<Listen, json> listenChecked(const json& msg);
    std::optional<json> handleLegacy(const json& msg, Connection& c, const Peer& peer);
    std::optional<json> handleModern(const json& msg, Connection& c, const Peer& peer);

    // Tasks extension.
    json createTask(const json& id, const Tool& tool, const json& args, const json& caps,
                    std::function<ToolResult(const ToolCall&)> work);
    json tasksMethod(const json& id, const std::string& method, const json& params, const json& caps);

    ServerIdentity m_id;
    std::shared_ptr<struct TaskStore> m_tasks;
    std::vector<Tool> m_tools;           // sorted by name: list results are deterministic
    std::vector<Prompt> m_prompts;
    std::vector<Resource> m_resources;
    std::vector<ResourceTemplate> m_templates;
    std::string m_stateKey;              // HMAC key for requestState (per process)
};

// HMAC-SHA-256 (hex), for requestState integrity. Exposed for tests.
std::string hmacSha256Hex(const std::string& key, const std::string& message);

} // namespace acestudio::mcp
