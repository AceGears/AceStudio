#include "acestudio/core/Recovery.h"
#include "acestudio/core/FileUtil.h"
#include "acestudio/core/ProjectIO.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <random>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <cerrno>
#  include <csignal>
#  include <unistd.h>
#endif

namespace fs = std::filesystem;
using nlohmann::json;

namespace acestudio {

std::int64_t currentProcessId() noexcept {
#ifdef _WIN32
    return static_cast<std::int64_t>(GetCurrentProcessId());
#else
    return static_cast<std::int64_t>(getpid());
#endif
}

bool isProcessRunning(std::int64_t pid) noexcept {
    if (pid <= 0) return false;
#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!h) return false;
    DWORD code = 0;
    const bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
#else
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}

std::string RecoveryStore::newId() {
    std::random_device rd;
    std::uniform_int_distribution<unsigned> dist(0, 15);
    std::string id = "doc-";
    for (int i = 0; i < 16; ++i) id += "0123456789abcdef"[dist(rd)];
    return id;
}

namespace {
bool safeId(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    for (char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    return true;
}
} // namespace

Status RecoveryStore::write(const std::string& id, const Document& doc, const std::string& originalPath,
                            const std::string& title) {
    if (!safeId(id)) return Error{ErrorCode::InvalidArgument, "Invalid recovery id"};
    // Snapshot first, metadata second: metadata only ever points at a
    // complete snapshot.
    SaveOptions opts;
    opts.keepBackup = false;
    if (auto st = saveProject(doc, m_dir / (id + ".acestudio"), opts); !st) return st;
    const json meta = {{"id", id}, {"originalPath", originalPath}, {"title", title},
                       {"savedUtc", nowUtcIso8601()}, {"pid", currentProcessId()}};
    const std::string text = meta.dump(2);
    return writeFileAtomic(m_dir / (id + ".json"),
                           std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

Status RecoveryStore::remove(const std::string& id) {
    if (!safeId(id)) return Error{ErrorCode::InvalidArgument, "Invalid recovery id"};
    std::error_code ec1, ec2;
    fs::remove(m_dir / (id + ".json"), ec1);
    fs::remove(m_dir / (id + ".acestudio"), ec2);
    if (ec1 || ec2) return Error{ErrorCode::IoError, "Could not remove recovery files for " + id};
    return {};
}

std::vector<RecoveryEntry> RecoveryStore::orphaned(std::vector<std::string>* problems) const {
    std::vector<RecoveryEntry> out;
    std::error_code ec;
    if (!fs::is_directory(m_dir, ec)) return out;
    for (const auto& e : fs::directory_iterator(m_dir, ec)) {
        if (e.path().extension() != ".json") continue;
        auto bytes = readFileBytes(e.path(), 1u << 20);
        json meta = bytes ? json::parse(bytes.value().begin(), bytes.value().end(), nullptr, false) : json();
        if (!meta.is_object() || !meta.contains("id") || !meta["id"].is_string()) {
            if (problems) problems->push_back("Unreadable recovery metadata: " + e.path().string());
            continue;
        }
        RecoveryEntry r;
        r.id = meta["id"].get<std::string>();
        if (!safeId(r.id) || e.path().stem().string() != r.id) {
            if (problems) problems->push_back("Mismatched recovery metadata: " + e.path().string());
            continue;
        }
        r.projectFile = m_dir / (r.id + ".acestudio");
        r.originalPath = meta.value("originalPath", std::string());
        r.title = meta.value("title", std::string("Untitled"));
        r.savedUtc = meta.value("savedUtc", std::string());
        r.writerPid = meta.value("pid", std::int64_t{0});
        if (r.writerPid == currentProcessId() || isProcessRunning(r.writerPid)) continue;   // live session
        if (!fs::exists(r.projectFile, ec)) {
            if (problems) problems->push_back("Recovery snapshot missing for " + r.title);
            continue;
        }
        out.push_back(std::move(r));
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.savedUtc > b.savedUtc; });
    return out;
}

} // namespace acestudio
