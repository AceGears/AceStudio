#include "McpProtocol.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <random>

namespace acestudio::mcp {

const std::vector<std::string>& legacyVersions() {
    static const std::vector<std::string> v{"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};
    return v;
}
const std::vector<std::string>& supportedVersions() {
    static const std::vector<std::string> v{kModernVersion, "2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};
    return v;
}

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

int levelRank(const std::string& l) {
    static const std::vector<std::string> levels{"debug", "info", "notice", "warning", "error", "critical", "alert", "emergency"};
    const auto it = std::find(levels.begin(), levels.end(), l);
    return it == levels.end() ? -1 : static_cast<int>(it - levels.begin());
}

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
        else if (type == "integer")
            ok = value.is_number_integer() ||
                 (value.is_number_float() && value.get<double>() == static_cast<double>(static_cast<long long>(value.get<double>())));
        else if (type == "number") ok = value.is_number();
        else if (type == "boolean") ok = value.is_boolean();
        else if (type == "array") ok = value.is_array();
        else if (type == "object") ok = value.is_object();
        if (!ok)
            return "argument '" + key + "' must be " +
                   (type == "integer" ? "an integer" : type == "array" || type == "object" ? "an " + type : "a " + type);
        if (auto e = p->find("enum"); e != p->end() && e->is_array() && std::find(e->begin(), e->end(), value) == e->end())
            return "argument '" + key + "' must be one of " + e->dump();
        if (type == "array" && p->contains("items") && (*p)["items"].value("type", std::string()) == "string")
            for (const auto& item : value)
                if (!item.is_string()) return "argument '" + key + "' must be a list of strings";
    }
    return std::nullopt;
}

// ---- base64 (requestState)

const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string b64url(const std::string& in) {
    std::string out;
    std::size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const unsigned v = (unsigned(std::uint8_t(in[i])) << 16) | (unsigned(std::uint8_t(in[i + 1])) << 8) | std::uint8_t(in[i + 2]);
        out += kB64[v >> 18];
        out += kB64[(v >> 12) & 63];
        out += kB64[(v >> 6) & 63];
        out += kB64[v & 63];
    }
    if (i + 1 == in.size()) {
        const unsigned v = unsigned(std::uint8_t(in[i])) << 16;
        out += kB64[v >> 18];
        out += kB64[(v >> 12) & 63];
    } else if (i + 2 == in.size()) {
        const unsigned v = (unsigned(std::uint8_t(in[i])) << 16) | (unsigned(std::uint8_t(in[i + 1])) << 8);
        out += kB64[v >> 18];
        out += kB64[(v >> 12) & 63];
        out += kB64[(v >> 6) & 63];
    }
    return out;
}

std::optional<std::string> unb64url(const std::string& in) {
    std::string out;
    unsigned buf = 0;
    int bits = 0;
    for (char c : in) {
        const char* p = std::strchr(kB64, c);
        if (!p || c == '\0') return std::nullopt;
        buf = (buf << 6) | static_cast<unsigned>(p - kB64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((buf >> bits) & 0xff);
        }
    }
    return out;
}

bool constantTimeEqual(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char d = 0;
    for (std::size_t i = 0; i < a.size(); ++i) d |= static_cast<unsigned char>(a[i] ^ b[i]);
    return d == 0;
}

long long nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// The request parameters a requestState is bound to.
json salientParams(const json& params) {
    json p = params;
    if (p.is_object()) {
        p.erase("_meta");
        p.erase("inputResponses");
        p.erase("requestState");
    }
    return p;
}

std::string requestName(const std::string& method, const json& params) {
    if (method == "resources/read") return params.value("uri", std::string());
    return params.value("name", std::string());
}

// Envelope checks for modern requests; an error response or nullopt.
std::optional<json> envelopeError(const json& id, const json& m) {
    const auto ver = m.find(meta::kProtocolVersion);
    if (ver == m.end() || !ver->is_string())
        return McpServer::errorResponse(id, code::kInvalidParams, std::string("Missing _meta[\"") + meta::kProtocolVersion + "\"]");
    const std::string version = ver->get<std::string>();
    if (!isModernVersion(version))
        return McpServer::errorResponse(id, code::kUnsupportedProtocolVersion, "Unsupported protocol version",
                                        json{{"supported", supportedVersions()}, {"requested", version}});
    const auto caps = m.find(meta::kClientCapabilities);
    if (caps == m.end() || !caps->is_object())
        return McpServer::errorResponse(id, code::kInvalidParams, std::string("Missing _meta[\"") + meta::kClientCapabilities + "\"]");
    return std::nullopt;
}

int errorCodeFor(const Error& e) {
    switch (e.code) {
    case ErrorCode::InvalidArgument:
    case ErrorCode::NotFound: return code::kInvalidParams;
    default: return code::kInternalError;
    }
}

} // namespace

// ------------------------------------------------------------------ InputContext

InputContext::InputContext(json clientCapabilities, bool modern, json inputResponses, json state, Requester requester,
                           const std::atomic<bool>* cancel)
    : m_caps(clientCapabilities.is_object() ? std::move(clientCapabilities) : json::object()), m_modern(modern),
      m_responses(inputResponses.is_object() ? std::move(inputResponses) : json::object()), m_stateIn(std::move(state)),
      m_requester(std::move(requester)), m_cancel(cancel) {}

bool InputContext::supports(const std::string& method) const {
    if (method == "elicitation/create") return m_caps.contains("elicitation");
    if (method == "sampling/createMessage") return m_caps.contains("sampling");
    if (method == "roots/list") return m_caps.contains("roots");
    return false;
}

std::vector<std::optional<json>> InputContext::askAll(const std::vector<InputRequest>& requests) {
    if (m_taskWaiter) {
        std::vector<InputRequest> ok;
        for (const auto& r : requests) {
            if (supports(r.method)) ok.push_back(r);
            else m_error = "the client does not support " + r.method;
        }
        auto answers = m_taskWaiter(ok);
        std::vector<std::optional<json>> out;
        std::size_t j = 0;
        for (const auto& r : requests) out.push_back(supports(r.method) && j < answers.size() ? answers[j++] : std::nullopt);
        return out;
    }
    std::vector<std::optional<json>> out;
    for (const auto& r : requests) out.push_back(ask(r.key, r.method, r.params));
    return out;
}

std::optional<json> InputContext::ask(const std::string& key, const std::string& method, const json& params) {
    if (!supports(method)) {
        m_error = "the client does not support " + method;
        return std::nullopt;
    }
    if (m_taskWaiter) {
        auto r = m_taskWaiter({{key, method, params}});
        if (r.empty() || !r[0]) m_error = "the task was cancelled while waiting for the client";
        return r.empty() ? std::nullopt : r[0];
    }
    if (m_modern) {
        if (auto it = m_responses.find(key); it != m_responses.end() && it->is_object()) return *it;
        m_pending[key] = {{"method", method}, {"params", params}};
        return std::nullopt;
    }
    if (!m_requester) {
        m_error = "this connection cannot send requests to the client";
        return std::nullopt;
    }
    auto r = m_requester(method, params, m_cancel);
    if (!r) {
        m_error = r.error().message;
        return std::nullopt;
    }
    return r.value();
}

// ------------------------------------------------------------------ content

json textBlock(const std::string& s) { return {{"type", "text"}, {"text", s}}; }
json imageBlock(const std::string& b64, const std::string& mimeType) { return {{"type", "image"}, {"data", b64}, {"mimeType", mimeType}}; }

ToolResult ToolResult::text(const std::string& s, bool error) {
    ToolResult r;
    r.addText(s);
    r.isError = error;
    return r;
}

ToolResult ToolResult::missingCapability(const std::string& capability) {
    ToolResult r;
    r.protocolError = json{{"code", code::kMissingRequiredClientCapability},
                           {"message", "This request needs the client capability '" + capability + "'"},
                           {"data", {{"requiredCapabilities", {{capability, json::object()}}}}}};
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

// ------------------------------------------------------------------ URI templates

std::optional<std::map<std::string, std::string>> matchUriTemplate(const std::string& t, const std::string& uri) {
    std::map<std::string, std::string> vars;
    std::size_t ti = 0, ui = 0;
    while (ti < t.size()) {
        if (t[ti] != '{') {
            if (ui >= uri.size() || uri[ui] != t[ti]) return std::nullopt;
            ++ti;
            ++ui;
            continue;
        }
        const std::size_t close = t.find('}', ti);
        if (close == std::string::npos) return std::nullopt;
        std::string name = t.substr(ti + 1, close - ti - 1);
        const bool reserved = !name.empty() && name[0] == '+';
        if (reserved) name.erase(0, 1);
        ti = close + 1;
        // The literal after the variable ends it (or the end of the URI).
        const std::size_t litEnd = t.find('{', ti);
        const std::string lit = t.substr(ti, litEnd == std::string::npos ? std::string::npos : litEnd - ti);
        std::size_t end;
        if (lit.empty()) end = uri.size();
        else {
            end = reserved ? uri.rfind(lit) : uri.find(lit, ui);
            if (end == std::string::npos || end < ui) return std::nullopt;
        }
        std::string value = uri.substr(ui, end - ui);
        if (value.empty() || (!reserved && value.find('/') != std::string::npos)) return std::nullopt;
        vars[name] = value;
        ui = end;
    }
    if (ui != uri.size()) return std::nullopt;
    return vars;
}

// ------------------------------------------------------------------ tasks
//
// Tasks of this process live in memory and, when a task folder is set, in
// <dir>/<taskId>.json too, rewritten on every change. That lets a restarted
// server (or a second acestudio-mcp sharing the folder) answer tasks/get. The
// owner process keeps <dir>/<instance>.alive fresh; a task whose owner stopped
// before finishing reads as failed. Others ask the owner to cancel a task, or
// pass it answers, through <taskId>.cancel and <taskId>.update-*.json files,
// which the owner picks up within a second.

namespace fs = std::filesystem;

struct Task {
    std::string id;
    std::string status = "working";   // working | input_required | completed | failed | cancelled
    std::string statusMessage;
    long long createdMs = 0, updatedMs = 0;
    int ttlMs = 3600000;
    int pollIntervalMs = 500;
    json result;     // completed
    json error;      // failed
    json pending = json::object();     // input_required: key -> request
    json responses = json::object();   // answers waiting to be taken
    std::atomic<bool> cancel{false};
    std::mutex m;
    std::condition_variable cv;
};

struct TaskStore {
    std::mutex m;
    std::map<std::string, std::shared_ptr<Task>> tasks;
    fs::path dir;                      // empty: memory only
    std::string instance;              // this process, for the .alive file
    McpServer::TaskObserver observer;
    int running = 0;                   // task threads still working
    std::condition_variable idle;
    std::thread watcher;
    std::atomic<bool> stop{false};
};

namespace {

constexpr long long kAliveMs = 10000;   // an owner that has not shown signs of life for this long has stopped

long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string isoTime(long long ms) {
    const std::time_t t = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[96];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
                  tm.tm_sec, static_cast<int>(ms % 1000));
    return buf;
}

std::string randomHex(int words) {
    std::random_device rd;
    std::string out;
    char buf[16];
    for (int i = 0; i < words; ++i) {
        std::snprintf(buf, sizeof buf, "%08x", rd());
        out += buf;
    }
    return out;
}

bool terminal(const std::string& status) { return status == "completed" || status == "failed" || status == "cancelled"; }

// Task ids are "task-" + hex; anything else is never a file name.
bool validTaskId(const std::string& id) {
    if (id.size() < 6 || id.size() > 64 || id.rfind("task-", 0) != 0) return false;
    return std::all_of(id.begin() + 5, id.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool hasTasksExtension(const json& caps) {
    return caps.is_object() && caps.contains("extensions") && caps["extensions"].is_object() && caps["extensions"].contains(kTasksExtension);
}

json taskRequiredError(const json& id) {
    return McpServer::errorResponse(id, code::kMissingRequiredClientCapability,
                                    std::string("This request needs the client extension '") + kTasksExtension + "'",
                                    {{"requiredCapabilities", {{"extensions", {{kTasksExtension, json::object()}}}}}});
}

// The task's shape on the wire (DetailedTask without the result fields).
json taskEnvelope(const Task& t) {
    json j{{"taskId", t.id},
           {"status", t.status},
           {"createdAt", isoTime(t.createdMs)},
           {"lastUpdatedAt", isoTime(t.updatedMs)},
           {"ttlMs", t.ttlMs},
           {"pollIntervalMs", t.pollIntervalMs}};
    if (!t.statusMessage.empty()) j["statusMessage"] = t.statusMessage;
    return j;
}

json detailed(const Task& t) {
    json r = taskEnvelope(t);
    if (t.status == "completed") r["result"] = t.result;
    if (t.status == "failed") r["error"] = t.error;
    if (t.status == "input_required" && !t.pending.empty()) r["inputRequests"] = t.pending;
    return r;
}

json toolResultJson(const ToolResult& r) {
    json j{{"content", r.content}, {"isError", r.isError}};
    if (r.structured) j["structuredContent"] = *r.structured;
    return j;
}

// ---- files

bool writeAtomically(const fs::path& file, const std::string& text) {
    const fs::path tmp = file.string() + "." + randomHex(1) + ".tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) return false;
        o << text;
        if (!o.flush()) return false;
    }
    std::error_code ec;
    fs::rename(tmp, file, ec);   // replaces atomically on POSIX and Windows (MoveFileEx)
    if (ec) fs::remove(tmp, ec);
    return !ec;
}

std::optional<json> readJsonFile(const fs::path& file) {
    std::ifstream f(file, std::ios::binary);
    if (!f) return std::nullopt;
    std::string text((std::istreambuf_iterator<char>(f)), {});
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return std::nullopt;
    return j;
}

json taskFile(const Task& t, const std::string& owner) {
    json j{{"taskId", t.id},   {"owner", owner},     {"status", t.status},       {"statusMessage", t.statusMessage},
           {"createdMs", t.createdMs}, {"updatedMs", t.updatedMs}, {"ttlMs", t.ttlMs}, {"pollIntervalMs", t.pollIntervalMs},
           {"pending", t.pending}};
    if (!t.result.is_null()) j["result"] = t.result;
    if (!t.error.is_null()) j["error"] = t.error;
    return j;
}

std::shared_ptr<Task> taskFromFile(const json& j) {
    auto t = std::make_shared<Task>();
    t->id = j.value("taskId", std::string());
    t->status = j.value("status", std::string("failed"));
    t->statusMessage = j.value("statusMessage", std::string());
    t->createdMs = j.value("createdMs", 0LL);
    t->updatedMs = j.value("updatedMs", t->createdMs);
    t->ttlMs = j.value("ttlMs", 3600000);
    t->pollIntervalMs = j.value("pollIntervalMs", 500);
    t->pending = j.value("pending", json::object());
    t->result = j.value("result", json());
    t->error = j.value("error", json());
    return t;
}

bool ownerAlive(const fs::path& dir, const std::string& owner) {
    if (owner.empty() || owner.find_first_not_of("0123456789abcdef") != std::string::npos) return false;
    const auto j = readJsonFile(dir / (owner + ".alive"));
    return j && nowMs() - j->value("at", 0LL) < kAliveMs;
}

// Call with task->m held: write the file and tell the observer.
void changed(TaskStore& store, const Task& t) {
    if (!store.dir.empty()) (void)writeAtomically(store.dir / (t.id + ".json"), taskFile(t, store.instance).dump());
    if (store.observer) store.observer(detailed(t));
}

// tasks/update: answers to pending questions (others are ignored).
void deliver(Task& t, const json& responses) {
    for (const auto& [k, v] : responses.items())
        if (v.is_object() && t.pending.contains(k)) {
            t.responses[k] = v;
            t.pending.erase(k);
        }
    t.updatedMs = nowMs();
    t.cv.notify_all();
}

void cancelTask(TaskStore& store, Task& t) {
    if (terminal(t.status)) return;
    t.cancel = true;
    t.status = "cancelled";
    t.statusMessage.clear();
    t.pending = json::object();
    t.updatedMs = nowMs();
    t.cv.notify_all();
    changed(store, t);
}

// The owner's housekeeping, about once a second: the .alive heartbeat,
// cancel and update requests from other processes, expired task files.
void watch(TaskStore* store) {
    long long lastSweep = 0;
    while (!store->stop) {
        const long long now = nowMs();
        (void)writeAtomically(store->dir / (store->instance + ".alive"), json{{"at", now}}.dump());
        std::vector<std::shared_ptr<Task>> mine;
        {
            std::lock_guard lk(store->m);
            for (auto& [id, t] : store->tasks) mine.push_back(t);
        }
        std::error_code ec;
        for (auto& t : mine) {
            const fs::path cancel = store->dir / (t->id + ".cancel");
            if (fs::exists(cancel, ec)) {
                fs::remove(cancel, ec);
                std::lock_guard tl(t->m);
                cancelTask(*store, *t);
            }
        }
        for (fs::directory_iterator it(store->dir, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = it->path().filename().string();
            const auto dash = name.find(".update-");
            if (dash == std::string::npos || it->path().extension() != ".json") continue;
            const std::string id = name.substr(0, dash);
            std::shared_ptr<Task> t;
            {
                std::lock_guard lk(store->m);
                if (auto f = store->tasks.find(id); f != store->tasks.end()) t = f->second;
            }
            if (!t) continue;
            const auto update = readJsonFile(it->path());
            std::error_code rec;
            fs::remove(it->path(), rec);
            if (!update) continue;
            std::lock_guard tl(t->m);
            deliver(*t, update->value("inputResponses", json::object()));
            changed(*store, *t);
        }
        if (now - lastSweep > 60000) {   // expired files of any process
            lastSweep = now;
            for (fs::directory_iterator it(store->dir, ec), end; !ec && it != end; it.increment(ec)) {
                const fs::path& f = it->path();
                if (f.extension() != ".json" && f.extension() != ".alive" && f.extension() != ".cancel") continue;
                std::error_code fec;
                const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(fs::file_time_type::clock::now() -
                                                                                       fs::last_write_time(f, fec)).count();
                if (fec) continue;
                if (f.extension() == ".json" && validTaskId(f.stem().string())) {
                    const auto j = readJsonFile(f);
                    if (j && now > j->value("updatedMs", 0LL) + j->value("ttlMs", 3600000)) fs::remove(f, fec);
                } else if (age > 24LL * 3600 * 1000) {
                    fs::remove(f, fec);   // stale heartbeats and requests
                }
            }
        }
        for (int i = 0; i < 10 && !store->stop; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

} // namespace

void McpServer::setTaskDirectory(const std::filesystem::path& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    m_tasks->dir = dir;
    m_tasks->instance = randomHex(4);
    m_tasks->watcher = std::thread([store = m_tasks.get()] { watch(store); });   // joined by ~McpServer
}

McpServer::~McpServer() {
    m_tasks->stop = true;
    if (m_tasks->watcher.joinable()) m_tasks->watcher.join();
    std::error_code ec;
    if (!m_tasks->dir.empty()) fs::remove(m_tasks->dir / (m_tasks->instance + ".alive"), ec);
}

void McpServer::setTaskObserver(TaskObserver o) { m_tasks->observer = std::move(o); }

int McpServer::activeTasks() const {
    std::lock_guard lk(m_tasks->m);
    return m_tasks->running;
}

bool McpServer::waitForTasks(std::chrono::milliseconds timeout) const {
    std::unique_lock lk(m_tasks->m);
    return m_tasks->idle.wait_for(lk, timeout, [this] { return m_tasks->running == 0; });
}

std::optional<json> McpServer::taskUpdateFor(const Subscription& s, const json& task) {
    if (!s.tasks) return std::nullopt;
    json params = task;
    params["_meta"] = {{meta::kSubscriptionId, s.id}};
    return json{{"jsonrpc", "2.0"}, {"method", "notifications/tasks"}, {"params", std::move(params)}};
}

json McpServer::createTask(const json& id, const Tool& tool, const json& /*args*/, const json& caps,
                           std::function<ToolResult(const ToolCall&)> work) {
    auto task = std::make_shared<Task>();
    task->id = "task-" + randomHex(4);
    task->createdMs = task->updatedMs = nowMs();
    task->statusMessage = "Running " + tool.name;
    auto store = m_tasks;
    {
        std::lock_guard lk(store->m);
        // Finished tasks past their TTL go.
        for (auto it = store->tasks.begin(); it != store->tasks.end();) {
            std::lock_guard tl(it->second->m);
            if (terminal(it->second->status) && nowMs() > it->second->updatedMs + it->second->ttlMs) it = store->tasks.erase(it);
            else ++it;
        }
        store->tasks[task->id] = task;
        ++store->running;
    }
    {
        std::lock_guard tl(task->m);
        changed(*store, *task);   // durable before the result is returned
    }
    std::thread([task, store, caps, work = std::move(work)] {
        InputContext in(caps, true, json(), json(), nullptr, &task->cancel);
        in.setTaskWaiter([task, store](const std::vector<InputRequest>& reqs) {
            std::unique_lock lk(task->m);
            for (const auto& r : reqs)
                if (!task->responses.contains(r.key)) task->pending[r.key] = {{"method", r.method}, {"params", r.params}};
            if (!task->pending.empty() && !task->cancel) {
                task->status = "input_required";
                task->updatedMs = nowMs();
                changed(*store, *task);
            }
            task->cv.wait(lk, [&] {
                if (task->cancel) return true;
                for (const auto& r : reqs)
                    if (!task->responses.contains(r.key)) return false;
                return true;
            });
            std::vector<std::optional<json>> out;
            for (const auto& r : reqs) {
                if (task->responses.contains(r.key)) {
                    out.push_back(task->responses[r.key]);
                    task->responses.erase(r.key);
                } else {
                    out.push_back(std::nullopt);
                }
            }
            if (!task->cancel) {
                task->status = "working";
                task->updatedMs = nowMs();
                changed(*store, *task);
            }
            return out;
        });
        ToolCall call;
        call.cancel = &task->cancel;
        call.input = &in;
        call.progress = [task, store](double progress, std::optional<double> total, const std::string& message) {
            std::lock_guard lk(task->m);
            if (terminal(task->status)) return;
            task->statusMessage = (total ? std::to_string(static_cast<int>(100.0 * progress / *total)) + "% " : std::string()) + message;
            task->updatedMs = nowMs();
            changed(*store, *task);
        };
        ToolResult r;
        json error;
        try {
            r = work(call);
        } catch (const std::exception& e) {
            error = {{"code", code::kInternalError}, {"message", std::string("The task failed: ") + e.what()}};
        }
        if (error.is_null() && r.protocolError) error = *r.protocolError;
        {
            std::lock_guard lk(task->m);
            if (!terminal(task->status)) {   // not cancelled meanwhile
                if (!error.is_null()) {
                    task->status = "failed";
                    task->error = error;
                } else {
                    task->status = "completed";
                    task->result = toolResultJson(r);
                }
                task->statusMessage.clear();
                task->pending = json::object();
                task->updatedMs = nowMs();
                changed(*store, *task);
            }
        }
        std::lock_guard lk(store->m);
        --store->running;
        store->idle.notify_all();
    }).detach();

    std::lock_guard lk(task->m);
    json result = taskEnvelope(*task);
    result["resultType"] = "task";
    return okResponse(id, std::move(result));
}

json McpServer::tasksMethod(const json& id, const std::string& method, const json& params, const json& caps) {
    if (!hasTasksExtension(caps)) return taskRequiredError(id);
    if (!params.contains("taskId") || !params["taskId"].is_string()) return errorResponse(id, code::kInvalidParams, method + " needs a taskId");
    const std::string taskId = params["taskId"].get<std::string>();
    json responses = json::object();
    if (method == "tasks/update") {
        responses = params.value("inputResponses", json::object());
        if (!responses.is_object()) return errorResponse(id, code::kInvalidParams, "inputResponses must be an object");
        for (const auto& [k, v] : responses.items())
            if (!v.is_object()) return errorResponse(id, code::kInvalidParams, "inputResponses[\"" + k + "\"] must be an object");
    }
    auto store = m_tasks;
    std::shared_ptr<Task> task;
    {
        std::lock_guard lk(store->m);
        if (auto it = store->tasks.find(taskId); it != store->tasks.end()) task = it->second;
    }
    if (task) {   // one of ours
        std::lock_guard lk(task->m);
        if (method == "tasks/get") return okResponse(id, detailed(*task));
        if (method == "tasks/update") {
            deliver(*task, responses);
            changed(*store, *task);
        } else {
            cancelTask(*store, *task);   // idempotent: a finished task stays as it is
        }
        return okResponse(id, json::object());
    }
    // Another process's task, or one from before a restart.
    std::optional<json> file;
    if (!store->dir.empty() && validTaskId(taskId)) file = readJsonFile(store->dir / (taskId + ".json"));
    if (!file || nowMs() > file->value("updatedMs", 0LL) + file->value("ttlMs", 3600000))
        return errorResponse(id, code::kInvalidParams, "Unknown task", {{"taskId", taskId}});
    auto t = taskFromFile(*file);
    const std::string owner = file->value("owner", std::string());
    if (!terminal(t->status) && !ownerAlive(store->dir, owner)) {
        t->status = "failed";
        t->error = {{"code", code::kInternalError},
                    {"message", "acestudio-mcp stopped before the task finished; run the tool again"}};
        t->statusMessage.clear();
        t->pending = json::object();
        t->updatedMs = nowMs();
        (void)writeAtomically(store->dir / (taskId + ".json"), taskFile(*t, owner).dump());
    }
    if (method == "tasks/get") return okResponse(id, detailed(*t));
    if (!terminal(t->status)) {   // the owner applies these within a second
        if (method == "tasks/update")
            (void)writeAtomically(store->dir / (taskId + ".update-" + randomHex(2) + ".json"), json{{"inputResponses", responses}}.dump());
        else
            (void)writeAtomically(store->dir / (taskId + ".cancel"), "{}");
    }
    return okResponse(id, json::object());
}

// ------------------------------------------------------------------ McpServer

McpServer::McpServer(ServerIdentity id, std::vector<Tool> tools, std::vector<Prompt> prompts, std::vector<Resource> resources,
                     std::vector<ResourceTemplate> templates)
    : m_id(std::move(id)), m_tasks(std::make_shared<TaskStore>()), m_tools(std::move(tools)), m_prompts(std::move(prompts)),
      m_resources(std::move(resources)), m_templates(std::move(templates)) {
    std::sort(m_tools.begin(), m_tools.end(), [](const Tool& a, const Tool& b) { return a.name < b.name; });
    std::sort(m_prompts.begin(), m_prompts.end(), [](const Prompt& a, const Prompt& b) { return a.name < b.name; });
    std::sort(m_resources.begin(), m_resources.end(), [](const Resource& a, const Resource& b) { return a.uri < b.uri; });
    std::random_device rd;
    for (int i = 0; i < 8; ++i) m_stateKey += std::to_string(rd()) + ":";
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

bool McpServer::changesConnection(const std::string& method) {
    return method == "initialize" || method == "notifications/initialized" || method == "logging/setLevel" ||
           method == "resources/subscribe" || method == "resources/unsubscribe";
}

std::optional<json> McpServer::envelopeProblem(const json& message) {
    const json id = message.is_object() && message.contains("id") ? message["id"] : json(nullptr);
    return envelopeError(id, metaOf(paramsOf(message)));
}

std::optional<json> McpServer::cancelledRequestId(const json& message) {
    if (!message.is_object() || message.value("method", std::string()) != "notifications/cancelled" || message.contains("id"))
        return std::nullopt;
    const json& p = paramsOf(message);
    if (auto it = p.find("requestId"); it != p.end()) return *it;
    return std::nullopt;
}

std::optional<json> McpServer::responseId(const json& message) {
    if (!message.is_object() || message.contains("method") || !message.contains("id")) return std::nullopt;
    if (!message.contains("result") && !message.contains("error")) return std::nullopt;
    return message["id"];
}

json McpServer::serverInfo() const {
    json info{{"name", m_id.name}, {"version", m_id.version}};
    if (!m_id.title.empty()) info["title"] = m_id.title;
    return info;
}

bool McpServer::offers(const std::string& method) const {
    if (method.rfind("prompts/", 0) == 0) return !m_prompts.empty();
    if (method.rfind("resources/", 0) == 0) return !m_resources.empty() || !m_templates.empty();
    return true;
}

json McpServer::capabilities(bool modern) const {
    json caps{{"tools", {{"listChanged", false}}}};
    if (!m_prompts.empty()) caps["prompts"] = {{"listChanged", false}};
    if (!m_resources.empty() || !m_templates.empty()) caps["resources"] = {{"subscribe", true}, {"listChanged", false}};
    caps["completions"] = json::object();
    if (!modern) caps["logging"] = json::object();   // legacy only: logging/setLevel
    if (modern && std::any_of(m_tools.begin(), m_tools.end(), [](const Tool& t) { return !t.taskSupport.empty(); }))
        caps["extensions"] = {{kTasksExtension, json::object()}};
    return caps;
}

json McpServer::toolsList(bool modern) const {
    json list = json::array();
    for (const Tool& t : m_tools) {
        json j{{"name", t.name}, {"description", t.description}, {"inputSchema", t.inputSchema}};
        if (!t.title.empty()) j["title"] = t.title;
        if (!t.annotations.empty()) j["annotations"] = t.annotations;
        if (modern && !t.taskSupport.empty()) j["execution"] = {{"taskSupport", t.taskSupport}};
        list.push_back(std::move(j));
    }
    json r{{"tools", std::move(list)}};
    if (modern) {
        r["ttlMs"] = 3600000;   // the tool set is fixed for a given build
        r["cacheScope"] = "public";
    }
    return r;
}

json McpServer::promptsList(bool modern) const {
    json list = json::array();
    for (const Prompt& p : m_prompts) {
        json j{{"name", p.name}, {"description", p.description}};
        if (!p.title.empty()) j["title"] = p.title;
        json args = json::array();
        for (const auto& a : p.arguments) args.push_back({{"name", a.name}, {"description", a.description}, {"required", a.required}});
        j["arguments"] = args;
        list.push_back(std::move(j));
    }
    json r{{"prompts", std::move(list)}};
    if (modern) {
        r["ttlMs"] = 3600000;
        r["cacheScope"] = "public";
    }
    return r;
}

json McpServer::resourcesList(bool modern) const {
    json list = json::array();
    for (const Resource& res : m_resources) {
        json j{{"uri", res.uri}, {"name", res.name}, {"description", res.description}};
        if (!res.title.empty()) j["title"] = res.title;
        if (!res.mimeType.empty()) j["mimeType"] = res.mimeType;
        list.push_back(std::move(j));
    }
    json r{{"resources", std::move(list)}};
    if (modern) {
        r["ttlMs"] = 3600000;
        r["cacheScope"] = "public";
    }
    return r;
}

json McpServer::templatesList(bool modern) const {
    json list = json::array();
    for (const ResourceTemplate& t : m_templates) {
        json j{{"uriTemplate", t.uriTemplate}, {"name", t.name}, {"description", t.description}};
        if (!t.title.empty()) j["title"] = t.title;
        if (!t.mimeType.empty()) j["mimeType"] = t.mimeType;
        list.push_back(std::move(j));
    }
    json r{{"resourceTemplates", std::move(list)}};
    if (modern) {
        r["ttlMs"] = 3600000;
        r["cacheScope"] = "public";
    }
    return r;
}

// ---- requestState: base64url(payload) "." HMAC(payload). The payload binds
// the state to the request (method, name, a digest of the parameters) and
// expires after 10 minutes.

json McpServer::inputRequired(const json& id, const std::string& method, const json& params, const InputContext& in) const {
    json payload{{"m", method},
                 {"n", requestName(method, params)},
                 {"d", hmacSha256Hex(m_stateKey, salientParams(params).dump()).substr(0, 32)},
                 {"exp", nowSeconds() + 600},
                 {"s", in.nextState()}};
    const std::string body = b64url(payload.dump());
    json r{{"resultType", "input_required"}, {"requestState", body + "." + hmacSha256Hex(m_stateKey, body)}};
    if (in.needsInput()) r["inputRequests"] = in.pending();
    return okResponse(id, std::move(r));
}

std::variant<json, std::string> McpServer::verifyState(const std::string& state, const std::string& method, const json& params) const {
    const auto dot = state.rfind('.');
    if (dot == std::string::npos) return std::string("requestState is malformed");
    const std::string body = state.substr(0, dot), mac = state.substr(dot + 1);
    if (!constantTimeEqual(mac, hmacSha256Hex(m_stateKey, body))) return std::string("requestState failed its integrity check");
    const auto raw = unb64url(body);
    const json payload = raw ? json::parse(*raw, nullptr, false) : json();
    if (!payload.is_object()) return std::string("requestState is malformed");
    if (payload.value("m", std::string()) != method || payload.value("n", std::string()) != requestName(method, params) ||
        payload.value("d", std::string()) != hmacSha256Hex(m_stateKey, salientParams(params).dump()).substr(0, 32))
        return std::string("requestState belongs to a different request");
    if (payload.value("exp", 0LL) < nowSeconds()) return std::string("requestState has expired; send the request again without it");
    return payload.value("s", json());
}

// ---- handlers

json McpServer::callTool(const json& id, const json& params, bool modern, InputContext& in, const Peer& peer,
                         std::optional<std::string> logThreshold) {
    const auto nameIt = params.find("name");
    if (nameIt == params.end() || !nameIt->is_string()) return errorResponse(id, code::kInvalidParams, "tools/call needs a tool name");
    const std::string name = nameIt->get<std::string>();
    const auto tool = std::find_if(m_tools.begin(), m_tools.end(), [&](const Tool& t) { return t.name == name; });
    if (tool == m_tools.end()) return errorResponse(id, code::kInvalidParams, "Unknown tool: " + name);
    json args = params.value("arguments", json::object());
    if (args.is_null()) args = json::object();
    const json& m = metaOf(params);

    const bool tasks = modern && hasTasksExtension(in.capabilities());
    if (modern && tool->taskSupport == "required" && !tasks) return taskRequiredError(id);
    ToolResult result;
    if (auto problem = checkArguments(tool->inputSchema, args)) {
        result = ToolResult::text("Invalid arguments for " + name + ": " + *problem, true);
    } else if (tasks && !tool->taskSupport.empty() && !tool->inputBeforeTask) {
        // The whole call runs as a task (the server decides; no client hint).
        const auto run = tool->run;
        return createTask(id, *tool, args, in.capabilities(), [run, args](const ToolCall& c) { return run(args, c); });
    } else {
        ToolCall call;
        call.cancel = peer.cancel;
        call.input = &in;
        const Notify& notify = peer.notify;
        if (auto token = m.find("progressToken"); token != m.end() && notify) {
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
    if (result.protocolError)
        return errorResponse(id, result.protocolError->value("code", code::kInternalError), result.protocolError->value("message", std::string()),
                             result.protocolError->value("data", json()));
    if (modern && in.needsInput()) return inputRequired(id, "tools/call", params, in);
    if (result.asyncWork) {
        if (tasks) return createTask(id, *tool, args, in.capabilities(), result.asyncWork);
        ToolCall now;
        now.cancel = peer.cancel;
        now.input = &in;
        result = result.asyncWork(now);
    }
    json r{{"content", result.content}, {"isError", result.isError}};
    if (result.structured) r["structuredContent"] = *result.structured;
    return okResponse(id, std::move(r));
}

json McpServer::getPrompt(const json& id, const json& params, bool modern, InputContext& in) {
    const std::string name = params.value("name", std::string());
    const auto p = std::find_if(m_prompts.begin(), m_prompts.end(), [&](const Prompt& x) { return x.name == name; });
    if (p == m_prompts.end()) return errorResponse(id, code::kInvalidParams, "Unknown prompt: " + name);
    json args = params.value("arguments", json::object());
    if (!args.is_object()) return errorResponse(id, code::kInvalidParams, "prompt arguments must be an object");
    for (const auto& a : p->arguments)
        if (a.required && (!args.contains(a.name) || !args[a.name].is_string()))
            return errorResponse(id, code::kInvalidParams, "Missing required argument '" + a.name + "' for prompt " + name);
    Result<json> r = Error{ErrorCode::Internal, "no handler"};
    try {
        r = p->get(args, in);
    } catch (const std::exception& e) {
        r = Error{ErrorCode::Internal, e.what()};
    }
    if (modern && in.needsInput()) return inputRequired(id, "prompts/get", params, in);
    if (!r) return errorResponse(id, errorCodeFor(r.error()), r.error().message);
    return okResponse(id, r.value());
}

json McpServer::readResource(const json& id, const json& params, bool modern, InputContext& in) {
    const std::string uri = params.value("uri", std::string());
    auto notFound = [&] {
        return errorResponse(id, modern ? code::kInvalidParams : code::kLegacyResourceNotFound, "Resource not found", json{{"uri", uri}});
    };
    if (uri.empty()) return errorResponse(id, code::kInvalidParams, "resources/read needs a uri");
    Result<json> contents = Error{ErrorCode::NotFound, "not found"};
    int ttl = 0;
    bool pub = false;
    bool found = false;
    try {
        if (auto res = std::find_if(m_resources.begin(), m_resources.end(), [&](const Resource& r) { return r.uri == uri; });
            res != m_resources.end()) {
            found = true;
            ttl = res->ttlMs;
            pub = res->publicCache;
            contents = res->read(uri, in);
        } else {
            // The most specific template first (most literal text).
            std::vector<const ResourceTemplate*> order;
            for (const auto& t : m_templates) order.push_back(&t);
            auto literal = [](const std::string& tpl) {
                std::size_t n = 0;
                bool inVar = false;
                for (char ch : tpl) {
                    if (ch == '{') inVar = true;
                    else if (ch == '}') inVar = false;
                    else if (!inVar) ++n;
                }
                return n;
            };
            std::stable_sort(order.begin(), order.end(),
                             [&](const ResourceTemplate* a, const ResourceTemplate* b) { return literal(a->uriTemplate) > literal(b->uriTemplate); });
            for (const ResourceTemplate* tp : order)
                if (const auto& t = *tp; auto vars = matchUriTemplate(t.uriTemplate, uri)) {
                    found = true;
                    ttl = t.ttlMs;
                    pub = t.publicCache;
                    contents = t.read(uri, *vars, in);
                    break;
                }
        }
    } catch (const std::exception& e) {
        contents = Error{ErrorCode::Internal, e.what()};
    }
    if (modern && in.needsInput()) return inputRequired(id, "resources/read", params, in);
    if (!found || (!contents && contents.error().code == ErrorCode::NotFound)) {
        if (found && contents.error().message != "not found") {
            json e = notFound();
            e["error"]["message"] = "Resource not found: " + contents.error().message;
            return e;
        }
        return notFound();
    }
    if (!contents) return errorResponse(id, errorCodeFor(contents.error()), contents.error().message);
    json result{{"contents", contents.value()}};
    if (modern) {
        result["ttlMs"] = ttl;
        result["cacheScope"] = pub ? "public" : "private";
    }
    return okResponse(id, std::move(result));
}

json McpServer::complete(const json& id, const json& params) const {
    const json ref = params.value("ref", json::object());
    const json arg = params.value("argument", json::object());
    const std::string argName = arg.value("name", std::string()), value = arg.value("value", std::string());
    const json context = params.contains("context") ? params["context"].value("arguments", json::object()) : json::object();
    const Completer* completer = nullptr;
    const std::string type = ref.value("type", std::string());
    if (type == "ref/prompt") {
        const std::string name = ref.value("name", std::string());
        auto p = std::find_if(m_prompts.begin(), m_prompts.end(), [&](const Prompt& x) { return x.name == name; });
        if (p == m_prompts.end()) return errorResponse(id, code::kInvalidParams, "Unknown prompt: " + name);
        completer = &p->complete;
    } else if (type == "ref/resource") {
        const std::string uri = ref.value("uri", std::string());
        auto t = std::find_if(m_templates.begin(), m_templates.end(), [&](const ResourceTemplate& x) { return x.uriTemplate == uri; });
        if (t == m_templates.end()) return errorResponse(id, code::kInvalidParams, "Unknown resource template: " + uri);
        completer = &t->complete;
    } else {
        return errorResponse(id, code::kInvalidParams, "completion ref.type must be ref/prompt or ref/resource");
    }
    std::vector<std::string> values;
    if (completer && *completer) values = (*completer)(argName, value, context);
    const std::size_t total = values.size();
    if (values.size() > 100) values.resize(100);
    return okResponse(id, {{"completion", {{"values", values}, {"total", total}, {"hasMore", total > values.size()}}}});
}

// Never throws: malformed parameters (a string where an object belongs, ...)
// become JSON-RPC errors instead of ending the server (found by fuzzing).
std::optional<json> McpServer::handle(const json& msg, Connection& c, const Peer& peer) {
    const json id = msg.is_object() && msg.contains("id") && (msg["id"].is_string() || msg["id"].is_number_integer()) ? msg["id"] : json();
    try {
        return handleChecked(msg, c, peer);
    } catch (const json::exception& e) {
        if (!msg.is_object() || !msg.contains("id")) return std::nullopt;
        return errorResponse(id, code::kInvalidParams, std::string("Invalid parameters: ") + e.what());
    } catch (const std::exception& e) {
        if (!msg.is_object() || !msg.contains("id")) return std::nullopt;
        return errorResponse(id, code::kInternalError, std::string("Internal error: ") + e.what());
    }
}

std::optional<json> McpServer::handleChecked(const json& msg, Connection& c, const Peer& peer) {
    if (!msg.is_object() || msg.value("jsonrpc", std::string()) != "2.0" || !msg.contains("method") || !msg["method"].is_string()) {
        if (responseId(msg)) return std::nullopt;   // responses are routed by the transport
        const json id = msg.is_object() && msg.contains("id") ? msg["id"] : json(nullptr);
        return errorResponse(id, code::kInvalidRequest, "Not a JSON-RPC 2.0 request");
    }
    const bool isRequest = msg.contains("id");
    if (isRequest && !(msg["id"].is_string() || msg["id"].is_number_integer()))
        return errorResponse(nullptr, code::kInvalidRequest, "The request id must be a string or an integer");
    const std::string method = msg["method"].get<std::string>();

    if (!isRequest) {
        if (method == "notifications/initialized" && c.era == Connection::Era::Legacy) c.initialized = true;
        return std::nullopt;
    }

    const bool enveloped = hasModernEnvelope(msg);
    if (c.era == Connection::Era::Undecided) {
        if (method == "initialize" && !enveloped) c.era = Connection::Era::Legacy;
        else if (enveloped) c.era = Connection::Era::Modern;
        else if (method == "ping") return okResponse(msg["id"], json::object());
        else
            return errorResponse(msg["id"], code::kInvalidRequest,
                                 "Send initialize first (protocol 2025-11-25 and earlier), or include "
                                 "_meta[\"" + std::string(meta::kProtocolVersion) + "\"] (protocol 2026-07-28)");
    }
    return c.era == Connection::Era::Legacy ? handleLegacy(msg, c, peer) : handleModern(msg, c, peer);
}

std::optional<json> McpServer::handleLegacy(const json& msg, Connection& c, const Peer& peer) {
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
        c.legacyVersion = isLegacyVersion(requested) ? requested : legacyVersions().front();
        c.clientCapabilities = params.value("capabilities", json::object());
        json r{{"protocolVersion", c.legacyVersion}, {"capabilities", capabilities(false)}, {"serverInfo", serverInfo()}};
        if (!m_id.instructions.empty()) r["instructions"] = m_id.instructions;
        return okResponse(id, std::move(r));
    }
    if (method == "ping") return okResponse(id, json::object());
    if (c.legacyVersion.empty()) return errorResponse(id, code::kInvalidRequest, "Send initialize first");
    if (method == "logging/setLevel") {
        const std::string level = params.value("level", std::string());
        if (levelRank(level) < 0) return errorResponse(id, code::kInvalidParams, "Unknown log level '" + level + "'");
        c.logLevel = level;
        return okResponse(id, json::object());
    }
    if (method == "resources/subscribe" || method == "resources/unsubscribe") {
        const std::string uri = params.value("uri", std::string());
        if (uri.empty()) return errorResponse(id, code::kInvalidParams, method + " needs a uri");
        if (method == "resources/subscribe") c.subscribed.insert(uri);
        else c.subscribed.erase(uri);
        return okResponse(id, json::object());
    }
    if (!offers(method)) return errorResponse(id, code::kMethodNotFound, "Method not found: " + method + " (not offered by this server)");
    if (method == "tools/list") return okResponse(id, toolsList(false));
    if (method == "prompts/list") return okResponse(id, promptsList(false));
    if (method == "resources/list") return okResponse(id, resourcesList(false));
    if (method == "resources/templates/list") return okResponse(id, templatesList(false));
    if (method == "completion/complete") return complete(id, params);
    InputContext in(c.clientCapabilities, false, json(), json(), peer.request, peer.cancel);
    if (method == "tools/call") return callTool(id, params, false, in, peer, c.logLevel);
    if (method == "prompts/get") return getPrompt(id, params, false, in);
    if (method == "resources/read") return readResource(id, params, false, in);
    return errorResponse(id, code::kMethodNotFound, "Method not found: " + method);
}

std::optional<json> McpServer::handleModern(const json& msg, Connection&, const Peer& peer) {
    const json& id = msg["id"];
    const std::string method = msg["method"].get<std::string>();
    const json& params = paramsOf(msg);
    const json& m = metaOf(params);
    if (method == "initialize" && hasModernEnvelope(msg))   // removed in 2026-07-28
        return errorResponse(id, code::kMethodNotFound, "Method not found: initialize (removed in protocol 2026-07-28; use server/discover)");
    if (method == "initialize") {
        json data{{"supported", json::array({kModernVersion})}};
        if (params.contains("protocolVersion") && params["protocolVersion"].is_string()) data["requested"] = params["protocolVersion"];
        return errorResponse(id, code::kUnsupportedProtocolVersion,
                             "This connection uses protocol 2026-07-28; the initialize handshake is not accepted on it", data);
    }
    if (auto e = envelopeError(id, m)) return e;
    if (!offers(method)) return errorResponse(id, code::kMethodNotFound, "Method not found: " + method + " (not offered by this server)");

    std::optional<json> response;
    if (method == "server/discover") {
        json r{{"supportedVersions", supportedVersions()}, {"capabilities", capabilities(true)}, {"ttlMs", 3600000}, {"cacheScope", "public"}};
        if (!m_id.instructions.empty()) r["instructions"] = m_id.instructions;
        response = okResponse(id, std::move(r));
    } else if (method == "tools/list") {
        response = okResponse(id, toolsList(true));
    } else if (method == "prompts/list") {
        response = okResponse(id, promptsList(true));
    } else if (method == "resources/list") {
        response = okResponse(id, resourcesList(true));
    } else if (method == "resources/templates/list") {
        response = okResponse(id, templatesList(true));
    } else if (method == "completion/complete") {
        response = complete(id, params);
    } else if (method == "tools/call" || method == "prompts/get" || method == "resources/read") {
        // Multi round-trip input: validate inputResponses and requestState.
        json responses = json::object();
        if (params.contains("inputResponses")) {
            if (!params["inputResponses"].is_object())
                return errorResponse(id, code::kInvalidParams, "inputResponses must be an object keyed like inputRequests");
            for (const auto& [k, v] : params["inputResponses"].items())
                if (!v.is_object()) return errorResponse(id, code::kInvalidParams, "inputResponses[\"" + k + "\"] must be an object (the client's result)");
            responses = params["inputResponses"];
        }
        json state;
        if (params.contains("requestState")) {
            if (!params["requestState"].is_string()) return errorResponse(id, code::kInvalidParams, "requestState must be a string");
            auto v = verifyState(params["requestState"].get<std::string>(), method, params);
            if (auto* err = std::get_if<std::string>(&v)) return errorResponse(id, code::kInvalidParams, *err);
            state = std::get<json>(v);
        }
        InputContext in(m.value(meta::kClientCapabilities, json::object()), true, responses, state, nullptr, peer.cancel);
        if (method == "tools/call") {
            std::optional<std::string> threshold;   // logs only when this request asked for them
            if (auto l = m.find(meta::kLogLevel); l != m.end() && l->is_string()) threshold = l->get<std::string>();
            response = callTool(id, params, true, in, peer, threshold);
        } else if (method == "prompts/get") {
            response = getPrompt(id, params, true, in);
        } else {
            response = readResource(id, params, true, in);
        }
    } else if (method == "tasks/get" || method == "tasks/update" || method == "tasks/cancel") {
        response = tasksMethod(id, method, params, m.value(meta::kClientCapabilities, json::object()));
    } else if (method == "subscriptions/listen") {
        return errorResponse(id, code::kInternalError, "subscriptions/listen must be handled by the transport");
    } else {
        return errorResponse(id, code::kMethodNotFound, "Method not found: " + method);
    }
    if (response && response->contains("result")) {
        json& r = (*response)["result"];
        if (!r.contains("resultType")) r["resultType"] = "complete";
        json& rm = r["_meta"];
        if (!rm.is_object()) rm = json::object();
        rm[meta::kServerInfo] = serverInfo();
    }
    return response;
}

// ---- subscriptions

std::variant<McpServer::Listen, json> McpServer::listenRequest(const json& msg) {
    try {
        return listenChecked(msg);
    } catch (const std::exception& e) {
        const json id = msg.is_object() && msg.contains("id") ? msg["id"] : json();
        return errorResponse(id, code::kInvalidParams, std::string("Invalid parameters: ") + e.what());
    }
}

std::variant<McpServer::Listen, json> McpServer::listenChecked(const json& msg) {
    if (!msg.is_object()) return errorResponse(nullptr, code::kInvalidRequest, "Not a JSON-RPC 2.0 request");
    const json id = msg.value("id", json());
    const json& params = paramsOf(msg);
    if (auto e = envelopeError(id, metaOf(params))) return *e;
    const json filter = params.value("notifications", json::object());
    if (!filter.is_object()) return errorResponse(id, code::kInvalidParams, "notifications must be an object");
    Listen l;
    l.sub.id = id;
    json honored = json::object();
    for (const char* k : {"toolsListChanged", "promptsListChanged", "resourcesListChanged"})
        if (filter.value(k, false)) honored[k] = true;
    l.sub.toolsListChanged = honored.contains("toolsListChanged");
    l.sub.promptsListChanged = honored.contains("promptsListChanged");
    l.sub.resourcesListChanged = honored.contains("resourcesListChanged");
    if (filter.value("tasks", false)) {   // task status changes (tasks extension)
        honored["tasks"] = true;
        l.sub.tasks = true;
    }
    if (filter.contains("resourceSubscriptions")) {
        if (!filter["resourceSubscriptions"].is_array()) return errorResponse(id, code::kInvalidParams, "resourceSubscriptions must be a list of URIs");
        json uris = json::array();
        for (const auto& u : filter["resourceSubscriptions"])
            if (u.is_string()) {
                l.sub.resources.insert(u.get<std::string>());
                uris.push_back(u);
            }
        honored["resourceSubscriptions"] = uris;
    }
    l.acknowledgement = {{"jsonrpc", "2.0"},
                         {"method", "notifications/subscriptions/acknowledged"},
                         {"params", {{"_meta", {{meta::kSubscriptionId, id}}}, {"notifications", honored}}}};
    return l;
}

json McpServer::listenClosed(const Subscription& s) const {
    return okResponse(s.id, {{"resultType", "complete"}, {"_meta", {{meta::kSubscriptionId, s.id}, {meta::kServerInfo, serverInfo()}}}});
}

std::optional<json> McpServer::updateFor(const Subscription& s, const std::string& uri) {
    if (!s.resources.count(uri)) return std::nullopt;
    return json{{"jsonrpc", "2.0"},
                {"method", "notifications/resources/updated"},
                {"params", {{"_meta", {{meta::kSubscriptionId, s.id}}}, {"uri", uri}}}};
}

std::optional<json> McpServer::legacyUpdateFor(const Connection& c, const std::string& uri) {
    if (!c.subscribed.count(uri)) return std::nullopt;
    return json{{"jsonrpc", "2.0"}, {"method", "notifications/resources/updated"}, {"params", {{"uri", uri}}}};
}

} // namespace acestudio::mcp
