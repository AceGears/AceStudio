#pragma once

#include "acestudio/core/Result.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace acestudio {

Result<std::vector<std::uint8_t>> readFileBytes(const std::filesystem::path& path,
                                                std::uintmax_t maxBytes = 2ull << 30);

// Writes `data` to `path` atomically: a reader sees either the complete old
// file or the complete new file, never a partial one. The data is flushed
// to stable storage before the swap. If `backupPath` is non-empty and
// `path` already exists, the previous contents end up at `backupPath`.
Status writeFileAtomic(const std::filesystem::path& path,
                       std::span<const std::uint8_t> data,
                       const std::filesystem::path& backupPath = {});

// Current UTC time as ISO-8601 with milliseconds, e.g. 2026-10-29T17:33:00.123Z
std::string nowUtcIso8601();

// Per-user application data directory (created if missing):
//   Windows: %APPDATA%\AceStudio
//   Linux:   $XDG_CONFIG_HOME/AceStudio or ~/.config/AceStudio
// ACESTUDIO_DATA_DIR overrides both (tests); portable mode (see isPortable) comes next.
Result<std::filesystem::path> userDataDir();

// Per-user, non-roaming folder for large files (AI runtime, models):
//   Windows: %LOCALAPPDATA%\AceStudio
//   Linux:   $XDG_DATA_HOME/AceStudio or ~/.local/share/AceStudio
// ACESTUDIO_DATA_DIR overrides it as well (everything in one folder).
Result<std::filesystem::path> userLocalDataDir();

// Folder containing the running executable (empty if unknown).
std::filesystem::path executableDir();

// True when a file named "portable" sits next to the executable: then both
// data folders are "<exe dir>/data" (unless ACESTUDIO_DATA_DIR is set).
bool isPortable();

} // namespace acestudio
