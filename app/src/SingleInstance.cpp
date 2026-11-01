#include "SingleInstance.h"

#include "QtBridge.h"

#include "acestudio/core/Log.h"

#include <QCryptographicHash>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace acestudio::ui {

SingleInstance::SingleInstance(const std::filesystem::path& dataDir, QObject* parent) : QObject(parent) {
    // The key depends on the (case-folded on Windows) absolute data folder, so
    // portable copies with their own data folders run side by side.
    QString dir = QDir::cleanPath(QDir(fromPath(dataDir)).absolutePath());
#ifdef _WIN32
    dir = dir.toLower();
#endif
    const QByteArray hash = QCryptographicHash::hash(dir.toUtf8(), QCryptographicHash::Sha256).toHex().left(16);
    m_key = QStringLiteral("AceStudio-") + QString::fromLatin1(hash);
}

SingleInstance::~SingleInstance() = default;

bool SingleInstance::forwardToRunning(const QStringList& files) {
    QLocalSocket socket;
    socket.connectToServer(m_key);
    if (!socket.waitForConnected(500)) return false;
#ifdef _WIN32
    // Let the running instance bring its window to the front.
    AllowSetForegroundWindow(ASFW_ANY);
#endif
    QJsonArray list;
    for (const QString& f : files) list.append(QDir(f).absolutePath());
    const QByteArray msg = QJsonDocument(QJsonObject{{QStringLiteral("open"), list}}).toJson(QJsonDocument::Compact) + '\n';
    socket.write(msg);
    if (!socket.waitForBytesWritten(2000)) return false;
    // Wait for the acknowledgement so the files are not lost if the running
    // instance is busy or hung.
    if (!socket.waitForReadyRead(3000)) return false;
    return socket.readAll().startsWith("ok");
}

bool SingleInstance::listen() {
    m_server = new QLocalServer(this);
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    if (!m_server->listen(m_key)) {
        // A crashed instance can leave a stale socket (Unix); remove it and retry.
        QLocalServer::removeServer(m_key);
        if (!m_server->listen(m_key)) {
            ACE_LOG_WARN("ui", "Single-instance listener failed: " + m_server->errorString().toStdString());
            return false;
        }
    }
    connect(m_server, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket* s = m_server->nextPendingConnection()) {
            connect(s, &QLocalSocket::disconnected, s, &QObject::deleteLater);
            connect(s, &QLocalSocket::readyRead, this, [this, s] {
                if (!s->canReadLine()) return;
                const QJsonDocument doc = QJsonDocument::fromJson(s->readLine());
                QStringList files;
                for (const auto& v : doc.object().value(QStringLiteral("open")).toArray()) files << v.toString();
                s->write("ok\n");
                s->flush();
                Q_EMIT openRequested(files);
            });
        }
    });
    return true;
}

} // namespace acestudio::ui
