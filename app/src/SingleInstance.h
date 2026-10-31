#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <filesystem>

class QLocalServer;

namespace acestudio::ui {

// One AceStudio per data folder. Two instances sharing a data folder would
// run two AI workers on one GPU, race on model downloads and overwrite each
// other's settings, so a second launch hands its files to the running
// instance and exits. `--new-instance` skips this.
class SingleInstance : public QObject {
    Q_OBJECT
public:
    explicit SingleInstance(const std::filesystem::path& dataDir, QObject* parent = nullptr);
    ~SingleInstance() override;

    // Sends `files` to a running instance. True if one received them (this
    // process should then exit).
    bool forwardToRunning(const QStringList& files);
    // Becomes the running instance. False if listening failed (the app still
    // runs, just without forwarding).
    bool listen();
    const QString& key() const { return m_key; }

Q_SIGNALS:
    // Another launch asked to open these files (may be empty: just come to the front).
    void openRequested(const QStringList& files);

private:
    QString m_key;
    QLocalServer* m_server = nullptr;
};

} // namespace acestudio::ui
