#include "acestudio/core/FileUtil.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <memory>
#include <optional>
#include <random>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <shlobj.h>
#  include <share.h>
#else
#  include <cerrno>
#  include <cstring>
#  include <fcntl.h>
#  include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace acestudio {

namespace {

std::string randomSuffix() {
    std::random_device rd;
    std::uniform_int_distribution<unsigned> dist(0, 0xFFFFFFu);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%06x", dist(rd));
    return buf;
}

#ifdef _WIN32
std::string lastErrorString() {
    const DWORD code = GetLastError();
    char* msg = nullptr;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<LPSTR>(&msg), 0, nullptr);
    std::string s = msg ? msg : ("error " + std::to_string(code));
    if (msg) LocalFree(msg);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

Status writeAndFlush(const fs::path& p, std::span<const std::uint8_t> data) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return Error{ErrorCode::IoError, "Cannot create " + p.string() + ": " + lastErrorString()};
    std::size_t off = 0;
    while (off < data.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(data.size() - off, 1u << 30));
        DWORD written = 0;
        if (!WriteFile(h, data.data() + off, chunk, &written, nullptr) || written == 0) {
            const auto err = lastErrorString();
            CloseHandle(h);
            return Error{ErrorCode::IoError, "Write failed for " + p.string() + ": " + err};
        }
        off += written;
    }
    const BOOL flushed = FlushFileBuffers(h);
    CloseHandle(h);
    if (!flushed) return Error{ErrorCode::IoError, "Flush failed for " + p.string() + ": " + lastErrorString()};
    return {};
}
#else
Status writeAndFlush(const fs::path& p, std::span<const std::uint8_t> data) {
    const int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) return Error{ErrorCode::IoError, "Cannot create " + p.string() + ": " + std::strerror(errno)};
    std::size_t off = 0;
    while (off < data.size()) {
        const ssize_t n = ::write(fd, data.data() + off, data.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            const std::string err = std::strerror(errno);
            ::close(fd);
            return Error{ErrorCode::IoError, "Write failed for " + p.string() + ": " + err};
        }
        off += static_cast<std::size_t>(n);
    }
    if (::fsync(fd) != 0) {
        const std::string err = std::strerror(errno);
        ::close(fd);
        return Error{ErrorCode::IoError, "fsync failed for " + p.string() + ": " + err};
    }
    ::close(fd);
    return {};
}

void syncDirectory(const fs::path& dir) {
    const int fd = ::open(dir.empty() ? "." : dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) { (void)::fsync(fd); ::close(fd); }
}
#endif

} // namespace

Result<std::vector<std::uint8_t>> readFileBytes(const fs::path& path, std::uintmax_t maxBytes) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return Error{ErrorCode::NotFound, "File not found: " + path.string()};
    const auto size = fs::file_size(path, ec);
    if (ec) return Error{ErrorCode::IoError, "Cannot stat " + path.string() + ": " + ec.message()};
    if (size > maxBytes) return Error{ErrorCode::InvalidArgument, "File too large: " + path.string()};

    std::FILE* f = nullptr;
#ifdef _WIN32
    // Shared read: _wfopen_s opens exclusively, which fails when any other
    // handle (another app, antivirus, our own log writer) has the file open.
    f = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
#else
    f = std::fopen(path.c_str(), "rb");
#endif
    if (!f) return Error{ErrorCode::IoError, "Cannot open " + path.string()};
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(size));
    const std::size_t got = buf.empty() ? 0 : std::fread(buf.data(), 1, buf.size(), f);
    const bool err = std::ferror(f) != 0;
    std::fclose(f);
    if (err || got != buf.size()) return Error{ErrorCode::IoError, "Short read on " + path.string()};
    return buf;
}

Status writeFileAtomic(const fs::path& path, std::span<const std::uint8_t> data, const fs::path& backupPath) {
    if (path.empty()) return Error{ErrorCode::InvalidArgument, "Empty output path"};
    std::error_code ec;
    const fs::path dir = path.has_parent_path() ? path.parent_path() : fs::path(".");
    if (!fs::exists(dir, ec)) {
        fs::create_directories(dir, ec);
        if (ec) return Error{ErrorCode::IoError, "Cannot create directory " + dir.string() + ": " + ec.message()};
    }

    const fs::path tmp = path.string() + ".tmp-" + randomSuffix();
    if (auto st = writeAndFlush(tmp, data); !st) {
        fs::remove(tmp, ec);
        return st;
    }

    const bool hadOriginal = fs::exists(path, ec);
    if (hadOriginal && !backupPath.empty()) {
        fs::copy_file(path, backupPath, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            fs::remove(tmp, ec);
            return Error{ErrorCode::IoError, "Could not create backup " + backupPath.string() + ": " + ec.message()};
        }
    }

#ifdef _WIN32
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const auto err = lastErrorString();
        fs::remove(tmp, ec);
        return Error{ErrorCode::IoError, "Could not replace " + path.string() + ": " + err};
    }
#else
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        const std::string err = std::strerror(errno);
        fs::remove(tmp, ec);
        return Error{ErrorCode::IoError, "Could not replace " + path.string() + ": " + err};
    }
    syncDirectory(dir);
#endif
    return {};
}

std::string nowUtcIso8601() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    const std::time_t t = system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &tm);
    char out[40];
    std::snprintf(out, sizeof out, "%s.%03dZ", buf, static_cast<int>(ms.count()));
    return out;
}

namespace {
// Reads an environment variable as a path (wide on Windows for Unicode).
std::optional<fs::path> envPath(const char* name) {
#ifdef _WIN32
    const std::wstring wname(name, name + std::strlen(name));
    const DWORD needed = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (needed <= 1) return std::nullopt;               // unset or empty
    std::wstring value(needed, L'\0');
    const DWORD got = GetEnvironmentVariableW(wname.c_str(), value.data(), needed);
    if (got == 0 || got >= needed) return std::nullopt;
    value.resize(got);
    return fs::path(value);
#else
    const char* v = std::getenv(name);
    if (!v || !*v) return std::nullopt;
    return fs::path(v);
#endif
}
} // namespace

Result<fs::path> userDataDir() {
    fs::path base;
    if (auto over = envPath("ACESTUDIO_DATA_DIR")) {
        base = *over;
    } else {
#ifdef _WIN32
        PWSTR appData = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData)))
            return Error{ErrorCode::NotFound, "Cannot locate %APPDATA%"};
        base = fs::path(appData) / "AceStudio";
        CoTaskMemFree(appData);
#else
        if (auto xdg = envPath("XDG_CONFIG_HOME"))
            base = *xdg / "AceStudio";
        else if (auto home = envPath("HOME"))
            base = *home / ".config" / "AceStudio";
        else
            return Error{ErrorCode::NotFound, "Cannot determine home directory"};
#endif
    }
    std::error_code ec;
    fs::create_directories(base, ec);
    if (ec) return Error{ErrorCode::IoError, "Cannot create " + base.string() + ": " + ec.message()};
    return base;
}

} // namespace acestudio
