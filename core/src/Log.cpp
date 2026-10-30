#include "acestudio/core/Log.h"

#include <chrono>
#include "acestudio/core/FileUtil.h"

#include <cstdio>
#include <string>

#ifdef _WIN32
#  include <share.h>
#endif

namespace fs = std::filesystem;

namespace acestudio {

std::string_view toString(LogLevel l) noexcept {
    switch (l) {
    case LogLevel::Trace: return "TRACE";
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO";
    case LogLevel::Warning: return "WARN";
    case LogLevel::Error: return "ERROR";
    case LogLevel::Critical: return "CRIT";
    }
    return "INFO";
}

namespace {
std::FILE* openAppend(const fs::path& p) {
#ifdef _WIN32
    return _wfsopen(p.c_str(), L"ab", _SH_DENYWR);   // others may read, not write
#else
    return std::fopen(p.c_str(), "ab");
#endif
}
} // namespace

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::configure(const LogConfig& cfg) {
    std::lock_guard lock(m_mutex);
    if (m_file) { std::fclose(m_file); m_file = nullptr; }
    m_cfg = cfg;
    m_fileBytes = 0;
    if (!m_cfg.filePath.empty()) {
        std::error_code ec;
        fs::create_directories(m_cfg.filePath.parent_path(), ec);
        m_fileBytes = fs::exists(m_cfg.filePath, ec) ? fs::file_size(m_cfg.filePath, ec) : 0;
        m_file = openAppend(m_cfg.filePath);
    }
}

void Logger::setMinLevel(LogLevel l) { std::lock_guard lock(m_mutex); m_cfg.minLevel = l; }
LogLevel Logger::minLevel() const { std::lock_guard lock(m_mutex); return m_cfg.minLevel; }
void Logger::setListener(Listener l) { std::lock_guard lock(m_mutex); m_listener = std::move(l); }

void Logger::rotateIfNeededLocked() {
    if (!m_file || m_fileBytes < m_cfg.maxFileBytes) return;
    std::fclose(m_file);
    m_file = nullptr;
    std::error_code ec;
    const auto rotated = [&](int i) { return fs::path(m_cfg.filePath.string() + "." + std::to_string(i)); };
    fs::remove(rotated(m_cfg.maxRotatedFiles), ec);
    for (int i = m_cfg.maxRotatedFiles - 1; i >= 1; --i)
        if (fs::exists(rotated(i), ec)) fs::rename(rotated(i), rotated(i + 1), ec);
    fs::rename(m_cfg.filePath, rotated(1), ec);
    m_file = openAppend(m_cfg.filePath);
    m_fileBytes = 0;
}

void Logger::log(LogLevel level, std::string_view module, std::string_view message) {
    Listener listener;
    std::string line;
    {
        std::lock_guard lock(m_mutex);
        if (level < m_cfg.minLevel) return;
        line.reserve(message.size() + 64);
        line += nowUtcIso8601();
        line += " [";
        line += toString(level);
        line += "] [";
        line += module;
        line += "] ";
        line += message;
        line += '\n';
        if (m_file) {
            rotateIfNeededLocked();
            if (m_file) {
                std::fwrite(line.data(), 1, line.size(), m_file);
                // Warnings and errors reach the disk immediately; other lines
                // at least once a second, so a crash loses little.
                const auto now = std::chrono::steady_clock::now();
                if (level >= LogLevel::Warning || now - m_lastFlush > std::chrono::seconds(1)) {
                    std::fflush(m_file);
                    m_lastFlush = now;
                }
                m_fileBytes += line.size();
            }
        }
        if (m_cfg.toStderr) std::fwrite(line.data(), 1, line.size(), stderr);
        listener = m_listener;
    }
    if (listener) listener(level, module, std::string_view(line).substr(0, line.size() - 1));
}

void Logger::flush() {
    std::lock_guard lock(m_mutex);
    if (m_file) std::fflush(m_file);
}

} // namespace acestudio
