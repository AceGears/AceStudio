#pragma once

// Thread-safe logger with a rotating file sink and optional stderr sink.
// Usage: ACE_LOG_INFO("project", "Saved " + path.string());

#include <cstdint>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

namespace acestudio {

enum class LogLevel { Trace, Debug, Info, Warning, Error, Critical };
std::string_view toString(LogLevel l) noexcept;

struct LogConfig {
    std::filesystem::path filePath;            // empty = no file sink
    std::uintmax_t maxFileBytes = 5u * 1024u * 1024u;
    int maxRotatedFiles = 5;
    bool toStderr = false;
    LogLevel minLevel = LogLevel::Info;
};

class Logger {
public:
    using Listener = std::function<void(LogLevel, std::string_view module, std::string_view line)>;

    static Logger& instance();

    void configure(const LogConfig& cfg);
    void setMinLevel(LogLevel l);
    LogLevel minLevel() const;
    void setListener(Listener l);              // e.g. an in-app log panel

    void log(LogLevel level, std::string_view module, std::string_view message);
    void flush();

private:
    Logger() = default;
    void rotateIfNeededLocked();

    mutable std::mutex m_mutex;
    LogConfig m_cfg;
    std::FILE* m_file = nullptr;
    std::chrono::steady_clock::time_point m_lastFlush{};
    std::uintmax_t m_fileBytes = 0;
    Listener m_listener;
};

} // namespace acestudio

#define ACE_LOG(level, module, msg) ::acestudio::Logger::instance().log((level), (module), (msg))
#define ACE_LOG_DEBUG(module, msg) ACE_LOG(::acestudio::LogLevel::Debug, module, msg)
#define ACE_LOG_INFO(module, msg) ACE_LOG(::acestudio::LogLevel::Info, module, msg)
#define ACE_LOG_WARN(module, msg) ACE_LOG(::acestudio::LogLevel::Warning, module, msg)
#define ACE_LOG_ERROR(module, msg) ACE_LOG(::acestudio::LogLevel::Error, module, msg)
