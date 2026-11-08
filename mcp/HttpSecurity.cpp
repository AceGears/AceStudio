// Security checks for the MCP HTTP transport (SEC-006). See HttpSecurity.h.

#include "HttpSecurity.h"

#include "acestudio/core/FileUtil.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <random>
#include <span>

#ifndef _WIN32
#  include <cerrno>
#  include <cstring>
#  include <fcntl.h>
#  include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace acestudio::mcp {

bool constantTimeEquals(const std::string& a, const std::string& b) {
    unsigned char diff = static_cast<unsigned char>(a.size() != b.size());
    const std::size_t n = std::max(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i)
        diff |= static_cast<unsigned char>((i < a.size() ? a[i] : 0) ^ (i < b.size() ? b[i] : 0));
    return diff == 0;
}

bool isLocalHost(std::string host) {
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

std::string generateHttpToken() {
    // std::random_device is the OS random source with MSVC (RtlGenRandom via
    // rand_s) and libstdc++/libc++ (getrandom, /dev/urandom or RDRAND).
    std::random_device rd;
    std::string out;
    out.reserve(64);
    for (int i = 0; i < 8; ++i) {
        const std::uint32_t v = static_cast<std::uint32_t>(rd());
        char buf[9];
        std::snprintf(buf, sizeof buf, "%08x", static_cast<unsigned>(v));
        out += buf;
    }
    return out;
}

Result<std::string> readTokenFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return Error{ErrorCode::NotFound, "cannot read token file " + path.string()};
    std::string line;
    std::getline(in, line);
    const auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!line.empty() && isSpace(static_cast<unsigned char>(line.back()))) line.pop_back();
    const auto first = std::find_if_not(line.begin(), line.end(), [&](char c) { return isSpace(static_cast<unsigned char>(c)); });
    line.erase(line.begin(), first);
    if (line.empty()) return Error{ErrorCode::InvalidArgument, "token file " + path.string() + " is empty"};
    return line;
}

Status writeTokenFile(const fs::path& path, const std::string& token) {
    const std::string text = token + "\n";
#ifdef _WIN32
    // %APPDATA% is readable by the user (and administrators) only.
    return writeFileAtomic(path, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
#else
    // Created 0600 from the start, so no other user can open it in between.
    const fs::path tmp = path.string() + ".tmp";
    ::unlink(tmp.c_str());
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) return Error{ErrorCode::IoError, "cannot create " + tmp.string() + ": " + std::strerror(errno)};
    std::size_t off = 0;
    while (off < text.size()) {
        const ssize_t n = ::write(fd, text.data() + off, text.size() - off);
        if (n <= 0) {
            const std::string err = std::strerror(errno);
            ::close(fd);
            ::unlink(tmp.c_str());
            return Error{ErrorCode::IoError, "cannot write " + tmp.string() + ": " + err};
        }
        off += static_cast<std::size_t>(n);
    }
    ::close(fd);
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        const std::string err = std::strerror(errno);
        ::unlink(tmp.c_str());
        return Error{ErrorCode::IoError, "cannot replace " + path.string() + ": " + err};
    }
    return Status::success();
#endif
}

std::optional<fs::path> defaultTokenFile() {
    auto d = userDataDir();
    if (!d) return std::nullopt;
    return d.value() / "mcp-http-token";
}

} // namespace acestudio::mcp
