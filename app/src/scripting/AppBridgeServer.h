#pragma once

// The app end of the MCP bridge (core/AppBridge.h): a per-user local socket
// that lets acestudio-mcp look at the open document, run Lua on it (one undo
// step) and open files. Scripts run on a copy on a worker thread; if the user
// edits the document meanwhile, the script's result is not applied.

#include <QObject>
#include <QPointer>

#include <atomic>
#include <filesystem>
#include <memory>
#include <vector>
#include <functional>

#include <nlohmann/json.hpp>

class QLocalServer;
class QLocalSocket;
class QTimer;

namespace acestudio::ui {

class DocumentView;

class AppBridgeServer : public QObject {
    Q_OBJECT
public:
    struct Hooks {
        std::function<DocumentView*()> currentView;
        std::function<std::vector<DocumentView*>()> views;   // every open tab, in order
        std::function<bool(const QString& path, QString* error)> open;
        std::filesystem::path scriptsFolder;
    };
    AppBridgeServer(Hooks hooks, QObject* parent = nullptr);
    ~AppBridgeServer() override;

    bool start();          // false if the socket could not be created (logged)
    void stop();
    bool running() const;
    // A document changed or another tab became current: tells watchers
    // (debounced), so MCP subscribers hear about it.
    void documentsChanged();

private:
    void onConnection();
    void onLine(QLocalSocket* sock, const QByteArray& line);
    void reply(QLocalSocket* sock, const nlohmann::json& id, const nlohmann::json& result);
    void fail(QLocalSocket* sock, const nlohmann::json& id, const std::string& message);
    void runLua(QLocalSocket* sock, const nlohmann::json& id, const nlohmann::json& params);
    // The document a request names ({"document": index | path}), or the
    // current one; null (with `error`) if there is none.
    DocumentView* viewFor(const nlohmann::json& params, std::string* error) const;

    Hooks m_hooks;
    QLocalServer* m_server = nullptr;
    int m_busy = 0;   // scripts running
    std::vector<QPointer<QLocalSocket>> m_watchers;
    ::QTimer* m_debounce = nullptr;
    std::shared_ptr<std::atomic<bool>> m_cancel = std::make_shared<std::atomic<bool>>(false);
};

} // namespace acestudio::ui
