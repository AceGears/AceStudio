#include "AppBridgeServer.h"

#include "../DocumentView.h"
#include "../QtBridge.h"

#include "acestudio/core/AppBridge.h"
#include "acestudio/core/Editor.h"
#include "acestudio/core/Log.h"
#include "acestudio/core/PngCodec.h"
#include "acestudio/core/Script.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QFileInfo>
#include <QImage>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTimer>

#include <algorithm>
#include <thread>

namespace acestudio::ui {

using nlohmann::json;
namespace fs = std::filesystem;

AppBridgeServer::AppBridgeServer(Hooks hooks, QObject* parent) : QObject(parent), m_hooks(std::move(hooks)) {
    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(250);
    connect(m_debounce, &QTimer::timeout, this, [this] {
        const QByteArray line = "{\"event\":\"document\"}\n";
        for (auto& w : m_watchers)
            if (w && w->state() == QLocalSocket::ConnectedState) w->write(line);
        m_watchers.erase(std::remove_if(m_watchers.begin(), m_watchers.end(), [](const QPointer<QLocalSocket>& w) { return !w; }),
                         m_watchers.end());
    });
}

void AppBridgeServer::documentsChanged() {
    if (!m_watchers.empty()) m_debounce->start();
}

DocumentView* AppBridgeServer::viewFor(const json& params, std::string* error) const {
    const json which = params.value("document", json());
    if (which.is_null()) {
        DocumentView* v = m_hooks.currentView ? m_hooks.currentView() : nullptr;
        if (!v && error) *error = "No document is open in AceStudio";
        return v;
    }
    const auto views = m_hooks.views ? m_hooks.views() : std::vector<DocumentView*>{};
    if (which.is_number_integer()) {
        const auto i = which.get<long long>();
        if (i >= 0 && i < static_cast<long long>(views.size())) return views[static_cast<std::size_t>(i)];
        if (error) *error = "There is no open document " + std::to_string(i) + " (" + std::to_string(views.size()) + " open)";
        return nullptr;
    }
    if (which.is_string()) {
        const QFileInfo want(QString::fromStdString(which.get<std::string>()));
        for (DocumentView* v : views)
            if (!v->filePath().isEmpty() && QFileInfo(v->filePath()) == want) return v;
        if (error) *error = "No open document has the file " + which.get<std::string>();
        return nullptr;
    }
    if (error) *error = "document must be a tab index or a file path";
    return nullptr;
}

AppBridgeServer::~AppBridgeServer() {
    *m_cancel = true;   // stops a running script; its result is dropped
    stop();
}

bool AppBridgeServer::start() {
    if (m_server) return true;
    m_server = new QLocalServer(this);
    m_server->setSocketOptions(QLocalServer::UserAccessOption);   // this user only
    const QString name = QString::fromStdString(appBridgeName());
    if (!m_server->listen(name)) {
        // A stale socket file from a crash (POSIX); SingleInstance already
        // guarantees no other AceStudio of this user is running.
        QLocalServer::removeServer(name);
        if (!m_server->listen(name)) {
            ACE_LOG_WARN("bridge", "Cannot open the MCP bridge: " + m_server->errorString().toStdString());
            delete m_server;
            m_server = nullptr;
            return false;
        }
    }
    connect(m_server, &QLocalServer::newConnection, this, &AppBridgeServer::onConnection);
    ACE_LOG_INFO("bridge", "MCP bridge listening on " + name.toStdString());
    return true;
}

void AppBridgeServer::stop() {
    if (!m_server) return;
    m_server->close();
    delete m_server;
    m_server = nullptr;
}

bool AppBridgeServer::running() const { return m_server && m_server->isListening(); }

void AppBridgeServer::onConnection() {
    while (QLocalSocket* sock = m_server->nextPendingConnection()) {
        auto buffer = std::make_shared<QByteArray>();
        connect(sock, &QLocalSocket::readyRead, this, [this, sock, buffer] {
            *buffer += sock->readAll();
            if (buffer->size() > 16 * 1024 * 1024) {
                sock->disconnectFromServer();
                return;
            }
            for (qsizetype nl; (nl = buffer->indexOf('\n')) >= 0;) {
                const QByteArray line = buffer->left(nl);
                buffer->remove(0, nl + 1);
                onLine(sock, line);
            }
        });
        connect(sock, &QLocalSocket::disconnected, sock, &QObject::deleteLater);
    }
}

void AppBridgeServer::reply(QLocalSocket* sock, const json& id, const json& result) {
    if (!sock || sock->state() != QLocalSocket::ConnectedState) return;
    sock->write(QByteArray::fromStdString(json{{"id", id}, {"result", result}}.dump() + "\n"));
}

void AppBridgeServer::fail(QLocalSocket* sock, const json& id, const std::string& message) {
    if (!sock || sock->state() != QLocalSocket::ConnectedState) return;
    sock->write(QByteArray::fromStdString(json{{"id", id}, {"error", message}}.dump() + "\n"));
}

void AppBridgeServer::onLine(QLocalSocket* sock, const QByteArray& line) {
    const json req = json::parse(line.toStdString(), nullptr, false);
    if (!req.is_object() || !req.contains("method")) return fail(sock, nullptr, "invalid request");
    const json id = req.value("id", json());
    const std::string method = req.value("method", std::string());
    const json params = req.value("params", json::object());
    ACE_LOG_INFO("bridge", "MCP request: " + method);

    if (method == "watch") {
        m_watchers.push_back(sock);
        return reply(sock, id, json{{"watching", true}});
    }
    if (method == "documents") {
        json list = json::array();
        const auto views = m_hooks.views ? m_hooks.views() : std::vector<DocumentView*>{};
        DocumentView* cur = m_hooks.currentView ? m_hooks.currentView() : nullptr;
        for (std::size_t i = 0; i < views.size(); ++i) {
            const DocumentView* v = views[i];
            list.push_back({{"index", i},
                            {"name", v->displayName().toStdString()},
                            {"path", v->filePath().toStdString()},
                            {"width", v->document().width()},
                            {"height", v->document().height()},
                            {"frames", v->document().frameCount()},
                            {"modified", v->isModified()},
                            {"current", v == cur}});
        }
        return reply(sock, id, json{{"documents", list}});
    }
    if (method == "document") {
        std::string why;
        DocumentView* v = viewFor(params, &why);
        if (!v && params.contains("document")) return fail(sock, id, why);
        if (!v) return reply(sock, id, json{{"open", false}, {"message", "No document is open in AceStudio"}});
        const Document& d = v->document();
        json layers = json::array();
        int index = 0;
        for (const auto& l : d.layers())
            layers.push_back({{"index", index++}, {"name", l.name}, {"visible", l.visible}, {"opacity", int(l.opacity)},
                              {"group", l.kind == LayerKind::Group}});
        json tags = json::array();
        for (const auto& t : d.tags()) tags.push_back({{"name", t.name}, {"from", t.fromFrame}, {"to", t.toFrame}});
        const int frame = v->currentFrame();
        const Image flat = d.composite(frame);
        // Small canvases are scaled up so the model can see the pixels.
        const int scale = std::clamp(512 / std::max({d.width(), d.height(), 1}), 1, 8);
        QImage q = toQImage(flat);
        if (scale > 1) q = q.scaled(q.width() * scale, q.height() * scale, Qt::IgnoreAspectRatio, Qt::FastTransformation);
        QByteArray png;
        QBuffer buf(&png);
        buf.open(QIODevice::WriteOnly);
        q.save(&buf, "PNG");
        json r{{"open", true},
               {"name", v->displayName().toStdString()},
               {"path", v->filePath().toStdString()},
               {"width", d.width()},
               {"height", d.height()},
               {"frames", d.frameCount()},
               {"frame", frame},
               {"modified", v->isModified()},
               {"layers", layers},
               {"tags", tags},
               {"imageScale", scale},
               {"png", png.toBase64().toStdString()}};
        return reply(sock, id, r);
    }
    if (method == "open") {
        const QString path = QString::fromStdString(params.value("path", std::string()));
        QString error;
        if (!m_hooks.open || !m_hooks.open(path, &error)) return fail(sock, id, error.isEmpty() ? "Could not open " + path.toStdString() : error.toStdString());
        return reply(sock, id, json{{"opened", path.toStdString()}});
    }
    if (method == "run_lua") return runLua(sock, id, params);
    fail(sock, id, "unknown method " + method);
}

void AppBridgeServer::runLua(QLocalSocket* sock, const json& id, const json& params) {
    std::string why;
    DocumentView* v = viewFor(params, &why);
    if (!v) return fail(sock, id, why);
    if (m_busy > 0) return fail(sock, id, "Another script is still running in AceStudio");
    if (!params.contains("code") || !params["code"].is_string()) return fail(sock, id, "run_lua needs code");

    ScriptOptions o;
    if (!v->filePath().isEmpty()) o.allowedRoots.push_back(toPath(v->filePath()).parent_path());
    if (!m_hooks.scriptsFolder.empty()) o.allowedRoots.push_back(m_hooks.scriptsFolder);
    if (params.contains("args") && params["args"].is_array())
        for (const auto& a : params["args"])
            if (a.is_string()) o.args.push_back(a.get<std::string>());
    auto copy = std::make_shared<Editor>(v->document());
    o.document = copy.get();
    o.cancel = m_cancel.get();
    auto output = std::make_shared<std::string>();
    o.output = [output](const std::string& text) { *output += text; };   // worker thread only
    const std::size_t markBefore = v->editor().history().mark();
    const std::string code = params["code"].get<std::string>();
    QPointer<DocumentView> view(v);
    QPointer<QLocalSocket> s(sock);
    QPointer<AppBridgeServer> self(this);
    auto cancel = m_cancel;   // keeps the flag alive for the thread
    ++m_busy;
    std::thread([self, o, code, copy, view, s, id, markBefore, cancel, output] {
        auto r = runScript(code, "mcp", o);
        QMetaObject::invokeMethod(qApp, [self, r = std::move(r), copy, view, s, id, markBefore, output]() mutable {
            if (!self) return;
            AppBridgeServer* me = self.data();
            --me->m_busy;
            auto fail = [&](const std::string& m) {
                if (s && s->state() == QLocalSocket::ConnectedState)
                    s->write(QByteArray::fromStdString(json{{"id", id}, {"error", m}, {"output", *output}}.dump() + "\n"));
            };
            if (!r) return fail(r.error().message);
            const ScriptResult& res = r.value();
            json written = json::array();
            for (const auto& f : res.written) written.push_back(f.string());
            bool applied = false;
            if (res.documentChanged) {
                if (!view) return fail("The document was closed while the script ran; nothing was changed");
                if (view->editor().history().mark() != markBefore)
                    return fail("The document was edited while the script ran, so the script's changes were not applied. Run it again.");
                if (auto st = view->editor().replaceDocument(copy->document(), "MCP Script"); !st) return fail(st.error().message);
                applied = true;
            }
            me->reply(s, id, json{{"output", res.output}, {"documentChanged", applied}, {"written", written}});
        }, Qt::QueuedConnection);
    }).detach();
}

} // namespace acestudio::ui
