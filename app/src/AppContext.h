#pragma once

#include "acestudio/core/Settings.h"

#include <QObject>
#include <QString>
#include <QStringList>

#include <filesystem>

namespace acestudio::ui {

// Application-wide services owned by main(): settings, paths, recent files.
class AppContext : public QObject {
    Q_OBJECT
public:
    explicit AppContext(QObject* parent = nullptr);

    // Initialises data directory, logging and settings. Returns a
    // user-facing warning (may be empty) to show after the window opens.
    QString initialize();

    Settings& settings() { return m_settings; }
    const std::filesystem::path& dataDir() const { return m_dataDir; }
    std::filesystem::path logDir() const { return m_dataDir / "logs"; }

    QStringList recentProjects() const;
    void addRecentProject(const QString& path);
    void removeRecentProject(const QString& path);
    void clearRecentProjects();

    // Persists settings; reports errors through the log.
    void saveSettings();
    // Changes one setting, saves, and announces it (settingChanged). Returns
    // false with `error` for unknown keys or wrong types.
    bool setSetting(const std::string& key, const nlohmann::json& value, QString* error = nullptr);

Q_SIGNALS:
    void recentProjectsChanged();
    void settingChanged(const QString& key);

private:
    void setRecent(const QStringList& list);

    Settings m_settings;
    std::filesystem::path m_dataDir;
};

} // namespace acestudio::ui
