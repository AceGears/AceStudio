// acestudio-mcp: the AceStudio MCP server (Phase 7).
//
//   acestudio-mcp [--root DIR]... [--cli PATH] [--http PORT [--host 127.0.0.1] [--token TOKEN]]
//
// stdio (default): newline-delimited JSON-RPC on stdin/stdout, logs on stderr.
// --http PORT: Streamable HTTP on http://127.0.0.1:PORT/mcp (loopback only).
// The tools may read and write files only inside the --root folders (default:
// the current folder).

#include "AceTools.h"
#include "ClientConfigs.h"
#include "ConformanceFixtures.h"
#include "McpProtocol.h"

#include "acestudio/core/AppBridge.h"
#include "acestudio/core/FileUtil.h"
#include "acestudio/core/Version.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QHostAddress>
#include <QLocalSocket>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <thread>

using namespace acestudio;
using namespace acestudio::mcp;
namespace fs = std::filesystem;

// HTTP limits: one request's headers and body.
constexpr qsizetype kMaxHeaderBytes = 64 * 1024;
constexpr qsizetype kMaxBodyBytes = 64 * 1024 * 1024;

// Compares secrets without leaking how many leading bytes matched.
bool constantTimeEquals(const std::string& a, const std::string& b) {
    unsigned char diff = static_cast<unsigned char>(a.size() != b.size());
    const std::size_t n = std::max(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i)
        diff |= static_cast<unsigned char>((i < a.size() ? a[i] : 0) ^ (i < b.size() ? b[i] : 0));
    return diff == 0;
}
using namespace std::chrono_literals;

namespace {

const char* kInstructions =
    "AceStudio is a pixel-art and game-asset editor. Use these tools to create and inspect .acestudio projects, "
    "render frames to look at them, export sprite sheets with Godot/Unity files, generate seamless autotile "
    "tilesets, pack atlases, slice sheets, generate UI kits, run local AI image tools, and run sandboxed Lua "
    "scripts (the `ace` API, resource acestudio://docs/lua-api) for anything else. app_* tools work on the "
    "documents open in the running AceStudio app. Files are limited to the server's root folders; existing "
    "files are kept unless overwrite is true (or the user agrees when asked).";

// ---------------------------------------------------------------- change hub

// Tools and the bridge watcher report resource changes; transports deliver
// them to subscribers.
class Hub {
public:
    // A changed resource (uri), or a task status change (task, uri empty).
    using Listener = std::function<void(const std::string& uri, const json& task)>;
    int add(Listener l) {
        std::lock_guard lk(m_mutex);
        m_listeners[++m_next] = std::move(l);
        return m_next;
    }
    void remove(int id) {
        std::lock_guard lk(m_mutex);
        m_listeners.erase(id);
    }
    void changed(const std::string& uri) {
        std::vector<Listener> ls;
        {
            std::lock_guard lk(m_mutex);
            for (auto& [k, l] : m_listeners) ls.push_back(l);
        }
        for (auto& l : ls) l(uri, json());
    }
    void taskChanged(const json& task) {
        std::vector<Listener> ls;
        {
            std::lock_guard lk(m_mutex);
            for (auto& [k, l] : m_listeners) ls.push_back(l);
        }
        for (auto& l : ls) l(std::string(), task);
    }

private:
    std::mutex m_mutex;
    std::map<int, Listener> m_listeners;
    int m_next = 0;
};

// ---------------------------------------------------------------- bridge

Result<json> bridgeCall(const std::string& method, const json& params, int timeoutMs) {
    QLocalSocket sock;
    sock.connectToServer(QString::fromStdString(appBridgeName()));
    if (!sock.waitForConnected(1500))
        return Error{ErrorCode::NotFound, "AceStudio is not running (or is an older version without the MCP bridge). "
                                          "Start the AceStudio app and try again."};
    static std::atomic<int> nextId{1};
    const int id = nextId++;
    const std::string line = json{{"id", id}, {"method", method}, {"params", params}}.dump() + "\n";
    sock.write(line.data(), static_cast<qint64>(line.size()));
    if (!sock.waitForBytesWritten(5000)) return Error{ErrorCode::IoError, "Could not send to AceStudio"};
    QByteArray buf;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!buf.contains('\n')) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) return Error{ErrorCode::Cancelled, "AceStudio did not answer in time"};
        if (!sock.waitForReadyRead(static_cast<int>(std::min<long long>(left, 1000)))) {
            if (sock.state() != QLocalSocket::ConnectedState) return Error{ErrorCode::IoError, "AceStudio closed the connection"};
            continue;
        }
        buf += sock.readAll();
        if (buf.size() > 64 * 1024 * 1024) return Error{ErrorCode::IoError, "AceStudio's answer is too large"};
    }
    auto j = json::parse(buf.left(buf.indexOf('\n')).toStdString(), nullptr, false);
    if (!j.is_object()) return Error{ErrorCode::CorruptData, "AceStudio sent an invalid answer"};
    if (j.contains("error")) {
        // run_lua: the error comes with the script's output.
        if (j.contains("output")) return json{{"error", j["error"]}, {"output", j["output"]}};
        return Error{ErrorCode::InvalidArgument, j["error"].is_string() ? j["error"].get<std::string>() : j["error"].dump()};
    }
    return j.value("result", json::object());
}

// While anyone is subscribed to the app's document, a watch connection to the
// app reports its changes (edits, tab switches) to the hub.
class BridgeWatcher {
public:
    explicit BridgeWatcher(Hub& hub) : m_hub(hub) {}
    ~BridgeWatcher() {
        m_stop = true;
        if (m_thread.joinable()) m_thread.join();
    }
    void want() {
        std::lock_guard lk(m_mutex);
        if (m_thread.joinable()) return;
        m_thread = std::thread([this] { loop(); });
    }

private:
    void loop() {
        while (!m_stop) {
            QLocalSocket sock;
            sock.connectToServer(QString::fromStdString(appBridgeName()));
            if (!sock.waitForConnected(1000)) {
                for (int i = 0; i < 30 && !m_stop; ++i) std::this_thread::sleep_for(100ms);
                continue;
            }
            const std::string req = json{{"id", 1}, {"method", "watch"}, {"params", json::object()}}.dump() + "\n";
            sock.write(req.data(), static_cast<qint64>(req.size()));
            sock.waitForBytesWritten(2000);
            QByteArray buf;
            while (!m_stop && sock.state() == QLocalSocket::ConnectedState) {
                if (!sock.waitForReadyRead(200)) continue;
                buf += sock.readAll();
                for (qsizetype nl; (nl = buf.indexOf('\n')) >= 0;) {
                    const json ev = json::parse(buf.left(nl).toStdString(), nullptr, false);
                    buf.remove(0, nl + 1);
                    if (ev.is_object() && ev.contains("event")) {
                        m_hub.changed(kAppDocumentUri);
                        m_hub.changed(kAppFrameUri);
                    }
                }
            }
        }
    }
    Hub& m_hub;
    std::mutex m_mutex;
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
};

bool isAppUri(const std::string& uri) { return uri.rfind("acestudio://app/", 0) == 0; }

// ---------------------------------------------------------------- outgoing requests

// Server-to-client requests (legacy) waiting for the client's response.
class Pending {
public:
    std::string add() {
        std::lock_guard lk(m_mutex);
        const std::string id = "acestudio-" + std::to_string(++m_next);
        m_slots[id];
        return id;
    }
    void deliver(const json& id, const json& message) {
        if (!id.is_string()) return;
        std::lock_guard lk(m_mutex);
        auto it = m_slots.find(id.get<std::string>());
        if (it == m_slots.end()) return;
        it->second = message;
        m_cv.notify_all();
    }
    // The response, or nullopt on cancel / timeout.
    std::optional<json> wait(const std::string& id, const std::atomic<bool>* cancel, std::chrono::seconds timeout) {
        std::unique_lock lk(m_mutex);
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!m_slots[id]) {
            if ((cancel && cancel->load()) || std::chrono::steady_clock::now() > deadline) break;
            m_cv.wait_for(lk, 200ms);
        }
        auto r = m_slots[id];
        m_slots.erase(id);
        return r;
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::map<std::string, std::optional<json>> m_slots;
    int m_next = 0;
};

Result<json> toResult(const std::optional<json>& response) {
    if (!response) return Error{ErrorCode::Cancelled, "the client did not answer"};
    if (response->contains("error")) {
        const json& e = (*response)["error"];
        return Error{ErrorCode::InvalidArgument, "the client refused: " + e.value("message", e.dump())};
    }
    return response->value("result", json::object());
}

// Methods that may wait (tools, client input, the app): run on a worker.
bool isSlow(const std::string& method) { return method == "tools/call" || method == "prompts/get" || method == "resources/read"; }

// ---------------------------------------------------------------- stdio

class StdioTransport {
public:
    StdioTransport(McpServer& s, Hub& hub, BridgeWatcher& watcher) : m_server(s), m_hub(hub), m_watcher(watcher) {
        m_hubId = m_hub.add([this](const std::string& uri, const json& task) { onChanged(uri, task); });
    }
    ~StdioTransport() { m_hub.remove(m_hubId); }

    int run() {
        std::string line;
        while (std::getline(std::cin, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            json msg = json::parse(line, nullptr, false);
            if (msg.is_discarded()) {
                send(McpServer::parseError("the line is not JSON"));
                continue;
            }
            if (auto rid = McpServer::responseId(msg)) {   // the client answering our request
                m_pending.deliver(*rid, msg);
                continue;
            }
            if (auto cancelled = McpServer::cancelledRequestId(msg)) {
                std::lock_guard lk(m_mutex);
                if (auto it = m_running.find(cancelled->dump()); it != m_running.end()) *it->second = true;
                m_subs.erase(cancelled->dump());   // a subscription ends without a response
                continue;
            }
            const std::string method = msg.is_object() ? msg.value("method", std::string()) : std::string();
            if (method == "subscriptions/listen" && msg.contains("id") && McpServer::hasModernEnvelope(msg)) {
                listen(msg);
                continue;
            }
            const bool slow = isSlow(method) && msg.contains("id");
            if (!slow) {
                std::optional<json> r;
                {
                    std::lock_guard lk(m_connMutex);
                    r = m_server.handle(msg, m_conn, peerFor(nullptr));
                    if (m_conn.era == Connection::Era::Legacy) wantWatcher(m_conn.subscribed);
                }
                if (r) send(*r);
                continue;
            }
            // Slow requests run in parallel so cancellation, client answers
            // and other requests are handled while they work.
            auto flag = std::make_shared<std::atomic<bool>>(false);
            const std::string key = msg["id"].dump();
            Connection snapshot;
            {
                std::lock_guard lk(m_connMutex);
                snapshot = m_conn;
            }
            {
                std::lock_guard lk(m_mutex);
                m_running[key] = flag;
                ++m_active;
            }
            std::thread([this, msg = std::move(msg), flag, key, snapshot]() mutable {
                auto r = m_server.handle(msg, snapshot, peerFor(flag.get()));
                bool cancelled;
                {
                    std::lock_guard lk(m_mutex);
                    cancelled = flag->load();
                    m_running.erase(key);
                }
                if (r && !cancelled) send(*r);   // a cancelled request gets no response
                std::lock_guard lk(m_mutex);
                --m_active;
                m_done.notify_all();
            }).detach();
        }
        // stdin closed: let running calls finish (the client may still read
        // their results), end subscriptions gracefully.
        {
            std::unique_lock lk(m_mutex);
            m_done.wait(lk, [this] { return m_active == 0; });
            for (auto& [k, sub] : m_subs) send(m_server.listenClosed(sub));
        }
        // Tasks keep running (up to their TTL) so that a client that comes
        // back, through a new acestudio-mcp, still gets their results.
        if (m_server.activeTasks() > 0) {
            std::fprintf(stderr, "acestudio-mcp: waiting for %d task(s) to finish\n", m_server.activeTasks());
            (void)m_server.waitForTasks(std::chrono::hours(1));
        }
        return 0;
    }

private:
    Peer peerFor(const std::atomic<bool>* cancel) {
        Peer p;
        p.notify = [this](const json& n) { send(n); };
        p.cancel = cancel;
        p.request = [this](const std::string& method, const json& params, const std::atomic<bool>* c) -> Result<json> {
            const std::string id = m_pending.add();
            send(json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}});
            return toResult(m_pending.wait(id, c, 600s));
        };
        return p;
    }
    void listen(const json& msg) {
        auto r = m_server.listenRequest(msg);
        if (auto* err = std::get_if<json>(&r)) {
            send(*err);
            return;
        }
        auto& l = std::get<McpServer::Listen>(r);
        {
            std::lock_guard lk(m_mutex);
            m_subs[msg["id"].dump()] = l.sub;
        }
        send(l.acknowledgement);
        wantWatcher(l.sub.resources);
    }
    void wantWatcher(const std::set<std::string>& uris) {
        for (const auto& u : uris)
            if (isAppUri(u)) m_watcher.want();
    }
    void onChanged(const std::string& uri, const json& task) {
        std::vector<json> out;
        {
            std::lock_guard lk(m_mutex);
            for (auto& [k, sub] : m_subs)
                if (auto n = task.is_null() ? McpServer::updateFor(sub, uri) : McpServer::taskUpdateFor(sub, task)) out.push_back(*n);
        }
        if (!task.is_null()) {
            for (auto& n : out) send(n);
            return;
        }
        {
            std::lock_guard lk(m_connMutex);
            if (auto n = McpServer::legacyUpdateFor(m_conn, uri)) out.push_back(*n);
        }
        for (auto& n : out) send(n);
    }
    void send(const json& j) {
        const std::string s = j.dump() + "\n";
        std::lock_guard lk(m_outMutex);
        std::fwrite(s.data(), 1, s.size(), stdout);
        std::fflush(stdout);
    }

    McpServer& m_server;
    Hub& m_hub;
    BridgeWatcher& m_watcher;
    int m_hubId = 0;
    Connection m_conn;
    std::mutex m_connMutex, m_mutex, m_outMutex;
    std::map<std::string, std::shared_ptr<std::atomic<bool>>> m_running;
    std::map<std::string, Subscription> m_subs;
    Pending m_pending;
    int m_active = 0;
    std::condition_variable m_done;
};

// ---------------------------------------------------------------- HTTP

struct HttpRequest {
    QByteArray method, path;
    std::map<std::string, std::string> headers;   // lower-case names
    QByteArray body;
    std::optional<std::string> header(const std::string& k) const {
        auto it = headers.find(k);
        if (it == headers.end()) return std::nullopt;
        return it->second;
    }
};

bool isLocalHost(std::string host) {
    if (!host.empty() && host.front() == '[') {
        const auto e = host.find(']');
        host = e == std::string::npos ? host : host.substr(0, e + 1);
    } else if (const auto c = host.rfind(':'); c != std::string::npos) {
        host = host.substr(0, c);
    }
    for (char& ch : host) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return host == "localhost" || host == "127.0.0.1" || host == "[::1]";
}

bool isLocalOrigin(const std::string& origin) {
    for (const char* scheme : {"http://", "https://"})
        if (origin.rfind(scheme, 0) == 0) return isLocalHost(origin.substr(std::string(scheme).size()));
    return false;
}

// Mcp-Name / Mcp-Param-* values: "=?base64?...?=" is strict Base64 of
// UTF-8 (bad padding or characters: nullopt); anything else is literal.
std::optional<std::string> decodeHeaderValue(const std::string& v) {
    if (v.size() >= 11 && v.rfind("=?base64?", 0) == 0 && v.compare(v.size() - 2, 2, "?=") == 0) {
        const QByteArray b64 = QByteArray::fromStdString(v.substr(9, v.size() - 11));
        if (b64.size() % 4 != 0) return std::nullopt;
        auto r = QByteArray::fromBase64Encoding(b64, QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
        if (!r) return std::nullopt;
        return r.decoded.toStdString();
    }
    for (const char ch : v)
        if (static_cast<unsigned char>(ch) < 0x20 || static_cast<unsigned char>(ch) > 0x7e) return std::nullopt;   // headers carry visible ASCII
    return v;
}

// x-mcp-header (SEP-2243): every annotated argument present in the body must
// be mirrored in its Mcp-Param-{Name} header. Returns the problem, if any.
std::optional<std::string> checkParamHeaders(const json& schema, const json& args, const HttpRequest& req, const std::string& prefix = {}) {
    const auto props = schema.find("properties");
    if (props == schema.end() || !props->is_object() || !args.is_object()) return std::nullopt;
    for (const auto& [name, ps] : props->items()) {
        if (!ps.is_object()) continue;
        const json value = args.contains(name) ? args[name] : json();
        if (ps.contains("x-mcp-header") && ps["x-mcp-header"].is_string()) {
            std::string header = "mcp-param-" + ps["x-mcp-header"].get<std::string>();
            for (char& c : header) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            const auto got = req.header(header);
            if (value.is_null()) continue;   // nothing to mirror: the header is not expected
            if (!got) return "the " + header + " header is missing for argument '" + prefix + name + "'";
            const auto decoded = decodeHeaderValue(*got);
            if (!decoded) return "the " + header + " header is not valid (bad characters or Base64)";
            std::string want;
            if (value.is_string()) want = value.get<std::string>();
            else if (value.is_boolean()) want = value.get<bool>() ? "true" : "false";
            else if (value.is_number_integer()) want = std::to_string(value.get<long long>());
            else if (value.is_number()) {
                // Integers compare numerically (42.0 == 42).
                char* end = nullptr;
                const double h = std::strtod(decoded->c_str(), &end);
                if (end && *end == '\0' && h == value.get<double>()) continue;
                return "the " + header + " header does not match argument '" + prefix + name + "'";
            } else {
                continue;
            }
            if (value.is_number_integer()) {
                char* end = nullptr;
                const double h = std::strtod(decoded->c_str(), &end);
                if (end && *end == '\0' && h == static_cast<double>(value.get<long long>())) continue;
            }
            if (*decoded != want) return "the " + header + " header does not match argument '" + prefix + name + "'";
        } else if (ps.value("type", std::string()) == "object" && value.is_object()) {
            if (auto p = checkParamHeaders(ps, value, req, prefix + name + ".")) return p;
        }
    }
    return std::nullopt;
}

// Decodes a chunked body: nullopt while incomplete; `bad` when malformed.
// `used` is the number of bytes consumed.
std::optional<QByteArray> dechunk(const QByteArray& in, qsizetype& used, bool& bad) {
    QByteArray out;
    qsizetype pos = 0;
    for (;;) {
        const qsizetype nl = in.indexOf("\r\n", pos);
        if (nl < 0) return std::nullopt;
        bool ok = false;
        QByteArray sizeLine = in.mid(pos, nl - pos);
        if (const qsizetype semi = sizeLine.indexOf(';'); semi >= 0) sizeLine.truncate(semi);
        const qsizetype size = sizeLine.trimmed().toLongLong(&ok, 16);
        if (!ok || size < 0 || size > kMaxBodyBytes || out.size() + size > kMaxBodyBytes) {   // no overflow below
            bad = true;
            return QByteArray();
        }
        pos = nl + 2;
        if (size == 0) {
            const qsizetype end = in.indexOf("\r\n", pos);   // trailers end with an empty line
            if (end < 0) return std::nullopt;
            used = end + 2;
            return out;
        }
        if (in.size() < pos + size + 2) return std::nullopt;
        out += in.mid(pos, size);
        pos += size + 2;
    }
}

class HttpTransport : public QObject {
public:
    HttpTransport(McpServer& s, Hub& hub, BridgeWatcher& watcher, std::string token, QObject* parent)
        : QObject(parent), m_server(s), m_hub(hub), m_watcher(watcher), m_token(std::move(token)) {
        m_hubId = m_hub.add([this](const std::string& uri, const json& task) {
            QMetaObject::invokeMethod(this, [this, uri, task] { onChanged(uri, task); }, Qt::QueuedConnection);
        });
        connect(&m_keepAlive, &QTimer::timeout, this, [this] {
            for (auto& [sock, sub] : m_listens)
                if (sock) sock->write(":\r\n");
            for (auto& [sid, sess] : m_sessions)
                if (sess.stream) sess.stream->write(":\r\n");
        });
        m_keepAlive.start(15000);
    }
    ~HttpTransport() override { m_hub.remove(m_hubId); }

    bool listen(const QHostAddress& host, quint16 port) {
        connect(&m_tcp, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket* sock = m_tcp.nextPendingConnection()) {
                sock->setParent(this);
                connect(sock, &QTcpSocket::readyRead, this, [this, sock] { onData(sock); });
                connect(sock, &QTcpSocket::disconnected, this, [this, sock] {
                    if (auto it = m_inFlight.find(sock); it != m_inFlight.end()) *it->second = true;   // closed stream = cancel
                    m_inFlight.erase(sock);
                    m_buffers.erase(sock);
                    m_listens.erase(sock);
                    for (auto& [sid, sess] : m_sessions)
                        if (sess.stream == sock) sess.stream = nullptr;
                    sock->deleteLater();
                });
            }
        });
        return m_tcp.listen(host, port);
    }
    quint16 port() const { return m_tcp.serverPort(); }

private:
    struct Session {
        std::shared_ptr<Connection> conn;
        QPointer<QTcpSocket> stream;   // the GET notification stream
    };

    void onData(QTcpSocket* sock) {
        QByteArray& buf = m_buffers[sock];
        buf += sock->readAll();
        if (buf.size() > kMaxBodyBytes + kMaxHeaderBytes) {
            reply(sock, 413, "Payload Too Large", {}, {});
            sock->disconnectFromHost();
            return;
        }
        if (m_inFlight.count(sock) || m_listens.count(sock)) return;   // one request at a time per connection
        const qsizetype end = buf.indexOf("\r\n\r\n");
        if (end < 0) {
            if (buf.size() > kMaxHeaderBytes) {   // no end of headers in sight
                reply(sock, 431, "Request Header Fields Too Large", {}, {});
                sock->disconnectFromHost();
            }
            return;
        }
        if (end > kMaxHeaderBytes) {
            reply(sock, 431, "Request Header Fields Too Large", {}, {});
            sock->disconnectFromHost();
            return;
        }
        HttpRequest req;
        const QList<QByteArray> lines = buf.left(end).split('\n');
        const QList<QByteArray> start = lines.value(0).trimmed().split(' ');
        if (start.size() < 3) {
            reply(sock, 400, "Bad Request", {}, {});
            sock->disconnectFromHost();
            return;
        }
        req.method = start[0];
        req.path = start[1];
        for (int i = 1; i < lines.size(); ++i) {
            const qsizetype c = lines[i].indexOf(':');
            if (c <= 0) continue;
            req.headers[lines[i].left(c).trimmed().toLower().toStdString()] = lines[i].mid(c + 1).trimmed().toStdString();
        }
        const std::string te = req.header("transfer-encoding").value_or("");
        if (te.find("chunked") != std::string::npos) {
            qsizetype used = 0;
            bool bad = false;
            auto body = dechunk(buf.mid(end + 4), used, bad);
            if (bad) {
                reply(sock, 400, "Bad Request", {}, "Bad chunked body", "text/plain");
                sock->disconnectFromHost();
                return;
            }
            if (!body) return;   // wait for the rest
            req.body = *body;
            buf.remove(0, end + 4 + used);
        } else {
            qsizetype length = 0;
            if (const auto cl = req.header("content-length")) {
                // Digits only: no sign, no garbage (std::stoll would throw and end the server).
                std::uint64_t v = 0;
                const auto [p, ec] = std::from_chars(cl->data(), cl->data() + cl->size(), v);
                if (cl->empty() || ec != std::errc() || p != cl->data() + cl->size()) {
                    reply(sock, 400, "Bad Request", {}, "Bad Content-Length", "text/plain");
                    sock->disconnectFromHost();
                    return;
                }
                if (v > kMaxBodyBytes) {
                    reply(sock, 413, "Payload Too Large", {}, {});
                    sock->disconnectFromHost();
                    return;
                }
                length = static_cast<qsizetype>(v);
            }
            if (buf.size() < end + 4 + length) return;   // wait for the body
            req.body = buf.mid(end + 4, length);
            buf.remove(0, end + 4 + length);
        }
        handle(sock, req);
    }

    void reply(QTcpSocket* sock, int status, const char* reason, const std::vector<std::pair<std::string, std::string>>& headers,
               const QByteArray& body, const char* contentType = "application/json") {
        QByteArray out = "HTTP/1.1 " + QByteArray::number(status) + " " + reason + "\r\n";
        if (!body.isEmpty()) out += QByteArray("Content-Type: ") + contentType + "\r\n";
        out += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
        for (const auto& [k, v] : headers) out += QByteArray::fromStdString(k + ": " + v + "\r\n");
        out += "\r\n" + body;
        sock->write(out);
    }
    void replyJson(QTcpSocket* sock, int status, const char* reason, const json& j,
                   const std::vector<std::pair<std::string, std::string>>& headers = {}) {
        reply(sock, status, reason, headers, QByteArray::fromStdString(j.dump()));
    }
    void startSse(QTcpSocket* sock, const std::vector<std::pair<std::string, std::string>>& extra) {
        QByteArray head = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\n"
                          "X-Accel-Buffering: no\r\nConnection: close\r\n";
        for (const auto& [k, v] : extra) head += QByteArray::fromStdString(k + ": " + v + "\r\n");
        sock->write(head + "\r\n");
    }
    static QByteArray event(const json& j) { return "event: message\ndata: " + QByteArray::fromStdString(j.dump()) + "\n\n"; }

    static std::pair<int, const char*> statusFor(const json& response) {
        if (!response.contains("error")) return {200, "OK"};
        switch (response["error"].value("code", 0)) {
        case code::kMethodNotFound: return {404, "Not Found"};
        case code::kUnsupportedProtocolVersion:
        case code::kHeaderMismatch:
        case code::kMissingRequiredClientCapability:
        case code::kInvalidRequest:
        case code::kParseError: return {400, "Bad Request"};
        default: return {200, "OK"};
        }
    }

    void onChanged(const std::string& uri, const json& task) {
        for (auto& [sock, sub] : m_listens)
            if (sock)
                if (auto n = task.is_null() ? McpServer::updateFor(sub, uri) : McpServer::taskUpdateFor(sub, task)) sock->write(event(*n));
        if (!task.is_null()) return;
        for (auto& [sid, sess] : m_sessions)
            if (sess.stream)
                if (auto n = McpServer::legacyUpdateFor(*sess.conn, uri)) sess.stream->write(event(*n));
    }

    void handle(QTcpSocket* sock, const HttpRequest& req) {
        // DNS rebinding protection: only localhost Host and Origin values.
        if (!isLocalHost(req.header("host").value_or("")) || (req.header("origin") && !isLocalOrigin(*req.header("origin")))) {
            replyJson(sock, 403, "Forbidden", McpServer::errorResponse(nullptr, code::kInvalidRequest, "Forbidden: this server only accepts localhost requests"));
            return;
        }
        const QByteArray path = req.path.left(req.path.indexOf('?') < 0 ? req.path.size() : req.path.indexOf('?'));
        if (path != "/mcp") {
            reply(sock, 404, "Not Found", {}, "Not Found: the MCP endpoint is /mcp", "text/plain");
            return;
        }
        if (!m_token.empty()) {
            const std::string auth = req.header("authorization").value_or("");
            if (!constantTimeEquals(auth, "Bearer " + m_token)) {
                replyJson(sock, 401, "Unauthorized", McpServer::errorResponse(nullptr, code::kInvalidRequest, "Missing or wrong bearer token"),
                          {{"WWW-Authenticate", "Bearer"}});
                return;
            }
        }
        const auto sid = req.header("mcp-session-id");
        if (req.method == "DELETE") {
            if (sid && m_sessions.erase(*sid)) reply(sock, 200, "OK", {}, {});
            else reply(sock, 404, "Not Found", {}, {});
            return;
        }
        if (req.method == "GET") {
            // Legacy (2025) standalone notification stream for a session.
            auto it = sid ? m_sessions.find(*sid) : m_sessions.end();
            if (it == m_sessions.end() || req.header("accept").value_or("").find("text/event-stream") == std::string::npos) {
                reply(sock, 405, "Method Not Allowed", {{"Allow", "POST, DELETE"}}, {});
                return;
            }
            startSse(sock, {});
            it->second.stream = sock;
            m_inFlight[sock] = std::make_shared<std::atomic<bool>>(false);   // the socket is now a stream
            return;
        }
        if (req.method != "POST") {
            reply(sock, 405, "Method Not Allowed", {{"Allow", "POST, DELETE"}}, {});
            return;
        }
        json msg = json::parse(req.body.toStdString(), nullptr, false);
        if (msg.is_discarded()) {
            replyJson(sock, 400, "Bad Request", McpServer::parseError("the body is not JSON"));
            return;
        }
        if (!msg.is_object()) {
            replyJson(sock, 400, "Bad Request", McpServer::errorResponse(nullptr, code::kInvalidRequest, "Batches are not supported"));
            return;
        }
        // A legacy client answering one of our requests.
        if (auto rid = McpServer::responseId(msg)) {
            m_pending.deliver(*rid, msg);
            reply(sock, 202, "Accepted", {}, {});
            return;
        }
        const json id = msg.contains("id") ? msg["id"] : json(nullptr);
        const std::string method = msg.value("method", std::string());
        const auto versionHeader = req.header("mcp-protocol-version");
        const bool modern = McpServer::hasModernEnvelope(msg) || (versionHeader && *versionHeader == kModernVersion && method != "initialize");

        std::shared_ptr<Connection> conn;
        std::vector<std::pair<std::string, std::string>> extra;
        if (modern) {
            // 2026-07-28: headers mirror the body.
            const json& params = msg.contains("params") && msg["params"].is_object() ? msg["params"] : json::object();
            const std::string bodyVersion = params.contains("_meta") && params["_meta"].is_object()
                                                ? params["_meta"].value(meta::kProtocolVersion, std::string()) : std::string();
            std::string problem;
            if (!versionHeader) problem = "the MCP-Protocol-Version header is missing";
            else if (!bodyVersion.empty() && *versionHeader != bodyVersion)
                problem = "MCP-Protocol-Version header value '" + *versionHeader + "' does not match body value '" + bodyVersion + "'";
            else if (!req.header("mcp-method")) problem = "the Mcp-Method header is missing";
            else if (*req.header("mcp-method") != method)
                problem = "Mcp-Method header value '" + *req.header("mcp-method") + "' does not match body value '" + method + "'";
            else if (method == "tools/call" || method == "prompts/get" || method == "resources/read" || method == "tasks/get" ||
                     method == "tasks/update" || method == "tasks/cancel") {
                const std::string name = method == "resources/read"        ? params.value("uri", std::string())
                                         : method.rfind("tasks/", 0) == 0 ? params.value("taskId", std::string())
                                                                           : params.value("name", std::string());
                if (!req.header("mcp-name")) problem = "the Mcp-Name header is missing";
                else if (decodeHeaderValue(*req.header("mcp-name")) != std::optional<std::string>(name))
                    problem = "Mcp-Name header value '" + *req.header("mcp-name") + "' does not match body value '" + name + "'";
                else if (method == "tools/call") {
                    const auto& tools = m_server.tools();
                    auto t = std::find_if(tools.begin(), tools.end(), [&](const Tool& x) { return x.name == name; });
                    if (t != tools.end())
                        if (auto p = checkParamHeaders(t->inputSchema, params.value("arguments", json::object()), req)) problem = *p;
                }
            }
            if (!problem.empty()) {
                replyJson(sock, 400, "Bad Request", McpServer::errorResponse(id, code::kHeaderMismatch, "Header mismatch: " + problem));
                return;
            }
            if (msg.contains("id"))
                if (auto e = McpServer::envelopeProblem(msg)) {   // missing _meta fields, unknown version
                    replyJson(sock, 400, "Bad Request", *e);
                    return;
                }
            if (method == "subscriptions/listen" && msg.contains("id")) {
                auto r = m_server.listenRequest(msg);
                if (auto* err = std::get_if<json>(&r)) {
                    replyJson(sock, 400, "Bad Request", *err);
                    return;
                }
                auto& l = std::get<McpServer::Listen>(r);
                startSse(sock, {});
                sock->write(event(l.acknowledgement));
                for (const auto& u : l.sub.resources)
                    if (isAppUri(u)) m_watcher.want();
                m_listens[sock] = l.sub;
                return;
            }
            conn = std::make_shared<Connection>();   // stateless: a fresh connection per request
            conn->era = Connection::Era::Modern;
        } else if (method == "initialize") {
            std::random_device rd;
            char sidBuf[33];
            std::snprintf(sidBuf, sizeof sidBuf, "%08x%08x%08x%08x", rd(), rd(), rd(), rd());
            conn = std::make_shared<Connection>();
            m_sessions[sidBuf] = Session{conn, nullptr};
            extra.push_back({"Mcp-Session-Id", sidBuf});
        } else {
            if (!sid) {
                replyJson(sock, 400, "Bad Request",
                          McpServer::errorResponse(id, code::kInvalidRequest,
                                                   "Missing Mcp-Session-Id (send initialize first), or use protocol 2026-07-28"));
                return;
            }
            auto it = m_sessions.find(*sid);
            if (it == m_sessions.end()) {
                replyJson(sock, 404, "Not Found", McpServer::errorResponse(id, code::kInvalidRequest, "Unknown or expired session"));
                return;
            }
            if (versionHeader && std::find(legacyVersions().begin(), legacyVersions().end(), *versionHeader) == legacyVersions().end()) {
                replyJson(sock, 400, "Bad Request",
                          McpServer::errorResponse(id, code::kInvalidRequest, "Unsupported MCP-Protocol-Version: " + *versionHeader));
                return;
            }
            conn = it->second.conn;
        }

        if (!msg.contains("id")) {   // a notification
            if (auto cancelled = McpServer::cancelledRequestId(msg))
                if (auto it = m_runningById.find(cancelled->dump()); it != m_runningById.end()) *it->second = true;
            (void)m_server.handle(msg, *conn);
            reply(sock, 202, "Accepted", extra, {});
            return;
        }
        if (!isSlow(method)) {
            json r = m_server.handle(msg, *conn).value_or(json::object());
            if (!modern)
                for (const auto& u : conn->subscribed)
                    if (isAppUri(u)) m_watcher.want();
            const auto [status, reason] = statusFor(r);
            replyJson(sock, status, reason, r, extra);
            return;
        }
        // Slow requests run on a worker thread; closing the connection
        // cancels. When the client accepts SSE, the response is an event
        // stream carrying progress / log notifications and, in the legacy
        // era, our requests to the client, before the result.
        auto flag = std::make_shared<std::atomic<bool>>(false);
        m_inFlight[sock] = flag;
        m_runningById[id.dump()] = flag;
        QPointer<QTcpSocket> guard(sock);
        Connection snapshot = *conn;
        const bool sse = req.header("accept").value_or("").find("text/event-stream") != std::string::npos;
        // The event stream starts with the first notification or request to
        // the client; a result without either is a plain JSON response, so
        // errors keep their HTTP status.
        auto started = std::make_shared<bool>(false);   // main thread only
        auto streamWrite = [this, guard, started, extra](const QByteArray& ev) {
            QMetaObject::invokeMethod(this, [this, guard, started, extra, ev] {
                if (!guard) return;
                if (!*started) {
                    startSse(guard.data(), extra);
                    *started = true;
                }
                guard->write(ev);
            }, Qt::QueuedConnection);
        };
        Peer peer;
        peer.cancel = flag.get();
        if (sse) {
            peer.notify = [streamWrite](const json& j) { streamWrite(event(j)); };
            if (!modern)
                peer.request = [this, streamWrite](const std::string& m, const json& params, const std::atomic<bool>* c) -> Result<json> {
                    const std::string rid = m_pending.add();
                    streamWrite(event(json{{"jsonrpc", "2.0"}, {"id", rid}, {"method", m}, {"params", params}}));
                    return toResult(m_pending.wait(rid, c, 600s));
                };
        } else if (!modern) {
            peer.request = [](const std::string&, const json&, const std::atomic<bool>*) -> Result<json> {
                return Error{ErrorCode::InvalidArgument, "the client must accept text/event-stream for server requests"};
            };
        }
        const std::string idKey = id.dump();
        std::thread([this, msg, snapshot, flag, guard, extra, started, peer, idKey]() mutable {
            json r = m_server.handle(msg, snapshot, peer).value_or(json::object());
            QMetaObject::invokeMethod(this, [this, r, guard, extra, started, idKey] {
                m_runningById.erase(idKey);
                if (!guard) return;
                m_inFlight.erase(guard.data());
                if (*started) {
                    guard->write(event(r));
                    guard->disconnectFromHost();
                    return;
                }
                const auto [status, reason] = statusFor(r);
                replyJson(guard.data(), status, reason, r, extra);
                if (!m_buffers[guard.data()].isEmpty()) onData(guard.data());
            }, Qt::QueuedConnection);
        }).detach();
    }

    McpServer& m_server;
    Hub& m_hub;
    BridgeWatcher& m_watcher;
    std::string m_token;
    int m_hubId = 0;
    QTcpServer m_tcp;
    QTimer m_keepAlive;
    Pending m_pending;
    std::map<QTcpSocket*, QByteArray> m_buffers;
    std::map<QTcpSocket*, std::shared_ptr<std::atomic<bool>>> m_inFlight;
    std::map<std::string, std::shared_ptr<std::atomic<bool>>> m_runningById;
    std::map<QTcpSocket*, Subscription> m_listens;
    std::map<std::string, Session> m_sessions;
};

int usage(const char* why) {
    if (why) std::fprintf(stderr, "error: %s\n", why);
    std::fprintf(stderr,
                 "Usage: acestudio-mcp [--root DIR]... [--cli PATH] [--http PORT [--host 127.0.0.1] [--token TOKEN]]\n"
                 "  stdio by default; --http serves Streamable HTTP at http://127.0.0.1:PORT/mcp\n"
                 "  --token: require \"Authorization: Bearer TOKEN\" on HTTP (or set ACESTUDIO_MCP_TOKEN)\n"
                 "  Tools may use files inside the --root folders only (default: the current folder).\n"
                 "  Protocol: MCP 2026-07-28, and 2025-11-25 / 2025-06-18 / 2025-03-26 / 2024-11-05 via initialize.\n"
                 "  --print-config claude-code|cursor|vscode|codex   print a client setup for this executable\n"
                 "  --task-dir DIR   keep MCP tasks here (default: <user data>/mcp-tasks); --no-task-files: memory only\n"
                 "  --conformance-fixtures   (testing) add the MCP conformance suite's fixtures\n");
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    ToolEnv env;
    std::optional<int> httpPort;
    bool fixtures = false;   // test only: the conformance suite's fixtures
    std::string token;
    std::optional<fs::path> taskDir;
    bool taskFiles = true;
    if (qEnvironmentVariableIsSet("ACESTUDIO_MCP_TOKEN")) token = qEnvironmentVariable("ACESTUDIO_MCP_TOKEN").toStdString();
    QHostAddress host(QHostAddress::LocalHost);
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::optional<std::string> {
            if (i + 1 >= argc) return std::nullopt;
            return std::string(argv[++i]);
        };
        if (a == "--root") {
            auto v = next();
            if (!v) return usage("--root needs a folder");
            std::error_code ec;
            fs::path p = fs::weakly_canonical(fs::absolute(fs::path(std::u8string(v->begin(), v->end())), ec), ec);
            if (ec || !fs::is_directory(p, ec)) return usage(("--root " + *v + " is not a folder").c_str());
            env.roots.push_back(p);
        } else if (a == "--cli") {
            auto v = next();
            if (!v) return usage("--cli needs a path");
            env.cli = fs::path(std::u8string(v->begin(), v->end()));
        } else if (a == "--http") {
            auto v = next();
            if (!v) return usage("--http needs a port");
            httpPort = std::atoi(v->c_str());
            if (*httpPort < 0 || *httpPort > 65535) return usage("bad port");
        } else if (a == "--host") {
            auto v = next();
            if (!v || !host.setAddress(QString::fromStdString(*v))) return usage("bad --host");
            if (!host.isLoopback()) return usage("--host must be a loopback address (127.0.0.1 or ::1)");
        } else if (a == "--token") {
            auto v = next();
            if (!v || v->empty()) return usage("--token needs a value");
            token = *v;
        } else if (a == "--task-dir") {
            auto v = next();
            if (!v) return usage("--task-dir needs a folder");
            taskDir = fs::path(std::u8string(v->begin(), v->end()));
        } else if (a == "--no-task-files") {
            taskFiles = false;
        } else if (a == "--conformance-fixtures") {
            fixtures = true;
        } else if (a == "--print-config") {
            auto v = next();
            if (!v) return usage("--print-config needs claude-code, cursor, vscode or codex");
            for (const auto& c : clientConfigs(executableDir() / fs::path(argv[0]).filename())) {
                if (c.id != *v) continue;
                std::printf("# %s: %s\n# %s\n\n%s", c.name.c_str(), c.file.c_str(), c.note.c_str(), c.content.c_str());
                return 0;
            }
            return usage(("unknown client " + *v).c_str());
        } else if (a == "--version") {
            std::printf("acestudio-mcp %s\n", kVersionString);
            return 0;
        } else if (a == "--help" || a == "-h") {
            return usage(nullptr);
        } else {
            return usage(("unknown option " + a).c_str());
        }
    }
    if (env.roots.empty()) {
        std::error_code ec;
        env.roots.push_back(fs::weakly_canonical(fs::current_path(ec), ec));
    }
    if (env.cli.empty()) {
#ifdef _WIN32
        const char* exe = "acestudio-cli.exe";
#else
        const char* exe = "acestudio-cli";
#endif
        // Installed: next to acestudio-mcp. Development build: build/cli/.
        env.cli = executableDir() / exe;
        std::error_code ec;
        if (!fs::exists(env.cli, ec) && fs::exists(executableDir().parent_path() / "cli" / exe, ec))
            env.cli = executableDir().parent_path() / "cli" / exe;
    }
    Hub hub;
    BridgeWatcher watcher(hub);
    env.bridge = bridgeCall;
    env.resourceChanged = [&hub](const std::string& uri) { hub.changed(uri); };

    ServerIdentity id;
    id.version = kVersionString;
    id.instructions = kInstructions;
    ServerContent content = aceStudioContent(env);
    if (fixtures) {
        Fixtures f = conformanceFixtures();
        for (auto& t : f.tools) content.tools.push_back(std::move(t));
        for (auto& p : f.prompts) content.prompts.push_back(std::move(p));
        for (auto& r : f.resources) content.resources.push_back(std::move(r));
        for (auto& t : f.templates) content.templates.push_back(std::move(t));
    }
    McpServer server(id, std::move(content.tools), std::move(content.prompts), std::move(content.resources), std::move(content.templates));
    // Tasks outlive the process in the user's data folder (mcp-tasks).
    if (taskFiles) {
        if (!taskDir)
            if (auto d = userDataDir()) taskDir = d.value() / "mcp-tasks";
        if (taskDir) server.setTaskDirectory(*taskDir);
    }
    server.setTaskObserver([&hub](const json& task) { hub.taskChanged(task); });

    if (httpPort) {
        HttpTransport http(server, hub, watcher, token, &app);
        if (!http.listen(host, static_cast<quint16>(*httpPort))) {
            std::fprintf(stderr, "error: cannot listen on port %d\n", *httpPort);
            return 2;
        }
        std::fprintf(stderr, "acestudio-mcp %s: http://%s:%u/mcp%s\n", kVersionString,
                     host.protocol() == QAbstractSocket::IPv6Protocol ? "[::1]" : "127.0.0.1", http.port(),
                     token.empty() ? "" : " (bearer token required)");
        return app.exec();
    }
    StdioTransport stdio(server, hub, watcher);
    return stdio.run();
}
