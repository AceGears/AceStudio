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
// ACESTUDIO_DATA_DIR overrides both (used by tests and portable installs).
Result<std::filesystem::path> userDataDir();

} // namespace acestudio
