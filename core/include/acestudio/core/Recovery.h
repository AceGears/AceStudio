#pragma once

// Crash recovery. While a document has unsaved changes the app writes a
// recovery copy at a regular interval into <data dir>/recovery:
//   <id>.acestudio   full project snapshot (normal project format)
//   <id>.json        metadata: original path, title, time, writer process
// Recovery files are deleted when the document is saved or closed
// normally, so anything left at startup came from a crash (or from
// another AceStudio instance that is still running, which is detected).

#include "acestudio/core/Document.h"
#include "acestudio/core/Result.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace acestudio {

struct RecoveryEntry {
    std::string id;
    std::filesystem::path projectFile;   // the snapshot
    std::string originalPath;            // empty if the document was never saved
    std::string title;
    std::string savedUtc;
    std::int64_t writerPid = 0;
};

class RecoveryStore {
public:
    explicit RecoveryStore(std::filesystem::path dir) : m_dir(std::move(dir)) {}

    const std::filesystem::path& directory() const noexcept { return m_dir; }

    // Random, filesystem-safe identifier for a new document session.
    static std::string newId();

    Status write(const std::string& id, const Document& doc, const std::string& originalPath, const std::string& title);
    Status remove(const std::string& id);

    // Entries left behind by processes that are no longer running.
    // Malformed metadata is reported in `problems` and skipped.
    std::vector<RecoveryEntry> orphaned(std::vector<std::string>* problems = nullptr) const;

private:
    std::filesystem::path m_dir;
};

std::int64_t currentProcessId() noexcept;
bool isProcessRunning(std::int64_t pid) noexcept;

} // namespace acestudio
