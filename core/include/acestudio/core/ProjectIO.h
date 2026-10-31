#pragma once

// The .acestudio project format.
//
// A project file is a ZIP container (deflate) holding:
//   manifest.json     format id, schema version, writer version, timestamps
//   document.json     the document model (see docs/project-format.md)
//   cels/L<id>_F<n>.png  one PNG per non-empty cel
//   masks/L<id>_F<n>.png one PNG per layer-mask cel (schema 2)
//
// Integrity: every ZIP entry carries a CRC-32 that is verified on read;
// the document is structurally validated after load.
// Saving is atomic: the new file is fully written and flushed to a
// temporary sibling, then swapped into place; the previous version is
// kept as "<name>.acestudio.bak".

#include "acestudio/core/Document.h"
#include "acestudio/core/Result.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace acestudio {

// Schema 2 (0.6.0) adds layer masks, clipping, adjustment layers and tile
// mode. Files that use none of them are still written as schema 1.
inline constexpr int kProjectSchemaVersion = 2;
inline constexpr const char* kProjectFormatId = "acestudio.project";
inline constexpr const char* kProjectExtension = ".acestudio";

struct ProjectInfo {
    int schemaVersion = 0;
    std::string writerVersion;
    std::string createdUtc;
    std::string modifiedUtc;
};

struct SaveOptions {
    bool keepBackup = true;
    std::string createdUtc;             // preserved from load; empty = now
};

// Serialize to / from an in-memory byte buffer (used by tests and autosave).
Result<std::vector<std::uint8_t>> serializeProject(const Document& doc, const SaveOptions& opts = {});
Result<Document> deserializeProject(std::span<const std::uint8_t> bytes, ProjectInfo* infoOut = nullptr);

Status saveProject(const Document& doc, const std::filesystem::path& path, const SaveOptions& opts = {});
Result<Document> loadProject(const std::filesystem::path& path, ProjectInfo* infoOut = nullptr);

// document.json <-> Document (no pixel data); exposed for tooling.
nlohmann::json documentToJson(const Document& doc);
// Lowest schema version that can represent `doc`.
int requiredSchemaVersion(const Document& doc);

} // namespace acestudio
