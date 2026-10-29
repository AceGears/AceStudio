#include "AppContext.h"
#include "QtBridge.h"

#include "acestudio/core/FileUtil.h"
#include "acestudio/core/Log.h"
#include "acestudio/core/Version.h"

#include <nlohmann/json.hpp>

namespace acestudio::ui {

AppContext::AppContext(QObject* parent) : QObject(parent) {}

QString AppContext::initialize() {
    QString warnings;
    auto dir = userDataDir();
    if (!dir) {
        // Fall back to a temp location so the app still runs; tell the user.
        m_dataDir = std::filesystem::temp_directory_path() / "AceStudio";
        std::error_code ec;
        std::filesystem::create_directories(m_dataDir, ec);
        warnings += QStringLiteral("Could not use the normal settings folder (%1). Settings will not persist.\n")
                        .arg(qs(dir.error().message));
    } else {
        m_dataDir = dir.value();
    }

    LogConfig cfg;
    cfg.filePath = logDir() / "acestudio.log";
#ifndef NDEBUG
    cfg.toStderr = true;
    cfg.minLevel = LogLevel::Debug;
#endif
    Logger::instance().configure(cfg);
    ACE_LOG_INFO("app", std::string("AceStudio ") + kVersionString + " starting; data dir " + m_dataDir.string());

    std::string settingsWarning;
    if (auto st = m_settings.load(m_dataDir / "settings.json", &settingsWarning); !st) {
        ACE_LOG_ERROR("settings", st.error().describe());
        warnings += qs(st.error().message) + QStringLiteral("\nSettings changes in this session will not be saved.\n");
    }
    if (!settingsWarning.empty()) warnings += qs(settingsWarning) + "\n";

    const auto level = m_settings.get<std::string>("advanced.logLevel");
    if (level == "debug") Logger::instance().setMinLevel(LogLevel::Debug);
    else if (level == "warning") Logger::instance().setMinLevel(LogLevel::Warning);
    return warnings.trimmed();
}

QStringList AppContext::recentProjects() const {
    QStringList out;
    for (const auto& v : m_settings.get("general.recentProjects"))
        if (v.is_string()) out << qs(v.get<std::string>());
    return out;
}

void AppContext::setRecent(const QStringList& list) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& s : list) arr.push_back(s.toStdString());
    (void)m_settings.set("general.recentProjects", arr);
    Q_EMIT recentProjectsChanged();
}

void AppContext::addRecentProject(const QString& path) {
    QStringList list = recentProjects();
    list.removeAll(path);
    list.prepend(path);
    const int max = m_settings.get<int>("general.maxRecentProjects");
    while (list.size() > max) list.removeLast();
    setRecent(list);
}

void AppContext::removeRecentProject(const QString& path) {
    QStringList list = recentProjects();
    list.removeAll(path);
    setRecent(list);
}

void AppContext::clearRecentProjects() { setRecent({}); }

void AppContext::saveSettings() {
    if (m_settings.path().empty()) return;
    if (auto st = m_settings.save(); !st) ACE_LOG_ERROR("settings", "Could not save settings: " + st.error().message);
}

} // namespace acestudio::ui
