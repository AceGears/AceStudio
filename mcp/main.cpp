// acestudio-mcp: the AceStudio MCP server (Phase 7).
//
//   acestudio-mcp [--root DIR]... [--cli PATH] [--http PORT [--host 127.0.0.1]]
//
// stdio (default): newline-delimited JSON-RPC on stdin/stdout, logs on stderr.
// --http PORT: Streamable HTTP on http://127.0.0.1:PORT/mcp (localhost only).
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
#include <condition_variable>
#include <cstdio>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <thread>

using namespace acestudio;
using namespace acestudio::mcp;
namespace fs = std::filesystem;

namespace {

const char* kInstructions =
    "AceStudio is a pixel-art and game-asset editor. Use these tools to create and inspect .acestudio projects, "
    "render frames to look at them, export sprite sheets with Godot/Unity files, generate seamless autotile "
    "tilesets, pack atlases, slice sheets, generate UI kits, run local AI image tools, and run sandboxed Lua "
    "scripts (the `ace` API) for anything else. app_* tools work on the document open in the running AceStudio "
    "app. Files are limited to the server's root folders; existing files are kept unless overwrite is true.";

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
    if (j.contains("error")) return Error{ErrorCode::InvalidArgument, j["error"].is_string() ? j["error"].get<std::string>() : j["error"].dump()};
    return j.value("result", json::object());
}

// ---------------------------------------------------------------- stdio

class StdioTransport {
public:
    explicit StdioTransport(McpServer& s) : m_server(s) {}

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
            if (auto cancelled = McpServer::cancelledRequestId(msg)) {
                std::lock_guard lk(m_mutex);
                if (auto it = m_running.find(cancelled->dump()); it != m_running.end()) *it->second = true;
                continue;
            }
            const bool isCall = msg.is_object() && msg.value("method", std::string()) == "tools/call" && msg.contains("id");
            if (!isCall) {
                if (auto r = m_server.handle(msg, m_conn, notifier())) send(*r);
                continue;
            }
            // Tool calls run in parallel so cancellation and other requests
            // are handled while they work.
            auto flag = std::make_shared<std::atomic<bool>>(false);
            const std::string key = msg["id"].dump();
            {
                std::lock_guard lk(m_mutex);
                m_running[key] = flag;
            }
            Connection snapshot = m_conn;
            {
                std::lock_guard lk(m_mutex);
                ++m_active;
            }
            std::thread([this, msg = std::move(msg), flag, key, snapshot]() mutable {
                auto r = m_server.handle(msg, snapshot, notifier(), flag.get());
                bool cancelled;
                {
                    std::lock_guard lk(m_mutex);
                    cancelled = flag->load();
                    m_running.erase(key);
                }
                // A cancelled request gets no response.
                if (r && !cancelled) send(*r);
                std::lock_guard lk(m_mutex);
                --m_active;
                m_done.notify_all();
            }).detach();
        }
        // stdin closed: let running calls finish and deliver their results
        // (the client may still be reading); the client kills us if it is not.
        std::unique_lock lk(m_mutex);
        m_done.wait(lk, [this] { return m_active == 0; });
        return 0;
    }

private:
    Notify notifier() {
        return [this](const json& n) { send(n); };
    }
    void send(const json& j) {
        const std::string s = j.dump() + "\n";
        std::lock_guard lk(m_outMutex);
        std::fwrite(s.data(), 1, s.size(), stdout);
        std::fflush(stdout);
    }

    McpServer& m_server;
    Connection m_conn;
    std::mutex m_mutex, m_outMutex;
    std::map<std::string, std::shared_ptr<std::atomic<bool>>> m_running;
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
    // host[:port], [v6]:port
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

std::string decodeHeaderValue(const std::string& v) {
    if (v.size() >= 11 && v.rfind("=?base64?", 0) == 0 && v.compare(v.size() - 2, 2, "?=") == 0)
        return QByteArray::fromBase64(QByteArray::fromStdString(v.substr(9, v.size() - 11))).toStdString();
    return v;
}

class HttpTransport : public QObject {
public:
    HttpTransport(McpServer& s, QObject* parent) : QObject(parent), m_server(s) {}

    bool listen(const QHostAddress& host, quint16 port) {
        connect(&m_tcp, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket* sock = m_tcp.nextPendingConnection()) {
                sock->setParent(this);
                connect(sock, &QTcpSocket::readyRead, this, [this, sock] { onData(sock); });
                connect(sock, &QTcpSocket::disconnected, this, [this, sock] {
                    if (auto it = m_inFlight.find(sock); it != m_inFlight.end()) *it->second = true;   // closed stream = cancel
                    m_buffers.erase(sock);
                    sock->deleteLater();
                });
            }
        });
        return m_tcp.listen(host, port);
    }
    quint16 port() const { return m_tcp.serverPort(); }

private:
    void onData(QTcpSocket* sock) {
        QByteArray& buf = m_buffers[sock];
        buf += sock->readAll();
        if (buf.size() > 64 * 1024 * 1024) {
            reply(sock, 413, "Payload Too Large", {}, {});
            sock->disconnectFromHost();
            return;
        }
        if (m_inFlight.count(sock)) return;   // one request at a time per connection
        const qsizetype end = buf.indexOf("\r\n\r\n");
        if (end < 0) return;
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
        const qsizetype length = req.header("content-length") ? std::stoll(*req.header("content-length")) : 0;
        if (buf.size() < end + 4 + length) return;   // wait for the body
        req.body = buf.mid(end + 4, length);
        buf.remove(0, end + 4 + length);
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

    static std::pair<int, const char*> statusFor(const json& response) {
        if (!response.contains("error")) return {200, "OK"};
        switch (response["error"].value("code", 0)) {
        case code::kMethodNotFound: return {404, "Not Found"};
        case code::kUnsupportedProtocolVersion:
        case code::kHeaderMismatch:
        case code::kInvalidRequest:
        case code::kParseError: return {400, "Bad Request"};
        default: return {200, "OK"};
        }
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
        if (req.method == "DELETE") {
            const auto sid = req.header("mcp-session-id");
            if (sid && m_sessions.erase(*sid)) reply(sock, 200, "OK", {}, {});
            else reply(sock, 404, "Not Found", {}, {});
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
        const json id = msg.contains("id") ? msg["id"] : json(nullptr);
        const auto versionHeader = req.header("mcp-protocol-version");
        const bool modern = McpServer::hasModernEnvelope(msg) || (versionHeader && *versionHeader == kModernVersion && msg.value("method", "") != "initialize");

        std::shared_ptr<Connection> conn;
        std::vector<std::pair<std::string, std::string>> extra;
        if (modern) {
            // 2026-07-28: headers mirror the body.
            const std::string method = msg.value("method", std::string());
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
            else if (method == "tools/call") {
                const std::string name = params.value("name", std::string());
                if (!req.header("mcp-name")) problem = "the Mcp-Name header is missing";
                else if (decodeHeaderValue(*req.header("mcp-name")) != name)
                    problem = "Mcp-Name header value '" + *req.header("mcp-name") + "' does not match body value '" + name + "'";
            }
            if (!problem.empty()) {
                replyJson(sock, 400, "Bad Request", McpServer::errorResponse(id, code::kHeaderMismatch, "Header mismatch: " + problem));
                return;
            }
            conn = std::make_shared<Connection>();   // stateless: a fresh connection per request
        } else if (msg.value("method", std::string()) == "initialize") {
            std::random_device rd;
            char sid[33];
            std::snprintf(sid, sizeof sid, "%08x%08x%08x%08x", rd(), rd(), rd(), rd());
            conn = std::make_shared<Connection>();
            m_sessions[sid] = conn;
            extra.push_back({"Mcp-Session-Id", sid});
        } else {
            const auto sid = req.header("mcp-session-id");
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
            conn = it->second;
        }

        if (!msg.contains("id")) {   // notification (or a response)
            (void)m_server.handle(msg, *conn);
            reply(sock, 202, "Accepted", extra, {});
            return;
        }
        if (msg.value("method", std::string()) != "tools/call") {
            json r = m_server.handle(msg, *conn).value_or(json::object());
            const auto [status, reason] = statusFor(r);
            replyJson(sock, status, reason, r, extra);
            return;
        }
        // Tool calls run on a worker thread; closing the connection cancels.
        // When the client accepts SSE, the response is an event stream that
        // carries progress / log notifications before the result.
        auto flag = std::make_shared<std::atomic<bool>>(false);
        m_inFlight[sock] = flag;
        QPointer<QTcpSocket> guard(sock);
        Connection snapshot = *conn;
        const bool sse = req.header("accept").value_or("").find("text/event-stream") != std::string::npos;
        if (sse) {
            QByteArray head = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\n"
                              "X-Accel-Buffering: no\r\nConnection: close\r\n";
            for (const auto& [k, v] : extra) head += QByteArray::fromStdString(k + ": " + v + "\r\n");
            sock->write(head + "\r\n");
        }
        auto sendEvent = [this, guard](const json& j) {
            const QByteArray ev = "event: message\ndata: " + QByteArray::fromStdString(j.dump()) + "\n\n";
            QMetaObject::invokeMethod(this, [guard, ev] { if (guard) guard->write(ev); }, Qt::QueuedConnection);
        };
        std::thread([this, msg, snapshot, flag, guard, extra, sse, sendEvent]() mutable {
            Notify notify;
            if (sse) notify = sendEvent;
            json r = m_server.handle(msg, snapshot, notify, flag.get()).value_or(json::object());
            QMetaObject::invokeMethod(this, [this, r, guard, extra, sse] {
                if (!guard) return;
                m_inFlight.erase(guard.data());
                if (sse) {
                    guard->write("event: message\ndata: " + QByteArray::fromStdString(r.dump()) + "\n\n");
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
    QTcpServer m_tcp;
    std::map<QTcpSocket*, QByteArray> m_buffers;
    std::map<QTcpSocket*, std::shared_ptr<std::atomic<bool>>> m_inFlight;
    std::map<std::string, std::shared_ptr<Connection>> m_sessions;
};

int usage(const char* why) {
    if (why) std::fprintf(stderr, "error: %s\n", why);
    std::fprintf(stderr,
                 "Usage: acestudio-mcp [--root DIR]... [--cli PATH] [--http PORT [--host 127.0.0.1]]\n"
                 "  stdio by default; --http serves Streamable HTTP at http://127.0.0.1:PORT/mcp\n"
                 "  Tools may use files inside the --root folders only (default: the current folder).\n"
                 "  Protocol: MCP 2026-07-28, and 2025-11-25 / 2025-06-18 / 2025-03-26 / 2024-11-05 via initialize.\n"
                 "  --print-config claude-code|cursor|vscode|codex   print a client setup for this executable\n"
                 "  --conformance-fixtures   (testing) add the MCP conformance suite's fixture tools\n");
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    ToolEnv env;
    std::optional<int> httpPort;
    bool fixtures = false;   // test only: the conformance suite's fixture tools
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
    env.bridge = bridgeCall;

    ServerIdentity id;
    id.version = kVersionString;
    id.instructions = kInstructions;
    std::vector<Tool> tools = aceStudioTools(env);
    if (fixtures)
        for (Tool& t : conformanceFixtures()) tools.push_back(std::move(t));
    McpServer server(id, std::move(tools));

    if (httpPort) {
        HttpTransport http(server, &app);
        if (!http.listen(host, static_cast<quint16>(*httpPort))) {
            std::fprintf(stderr, "error: cannot listen on port %d\n", *httpPort);
            return 2;
        }
        std::fprintf(stderr, "acestudio-mcp %s: http://%s:%u/mcp\n", kVersionString,
                     host.protocol() == QAbstractSocket::IPv6Protocol ? "[::1]" : "127.0.0.1", http.port());
        return app.exec();
    }
    StdioTransport stdio(server);
    return stdio.run();
}
