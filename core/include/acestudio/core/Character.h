#pragma once

// Character profiles (Phase 5): what stays the same every time a character is
// generated — description, palette, reference images, size, seed and the
// strength of the style/reference conditioning. Stored as `.acechar` files
// (ZIP: character.json + refs/<n>.png) in the character library folder.

#include "acestudio/core/Image.h"
#include "acestudio/core/Palette.h"
#include "acestudio/core/Result.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace acestudio {

struct CharacterProfile {
    static constexpr int kMaxReferences = 4;
    static constexpr int kSchemaVersion = 1;

    std::string id;                 // stable identifier (file name stem)
    std::string name;
    std::string prompt;             // added to every generation of this character
    std::string negative;
    Palette palette;                // results are reduced to these colours
    std::vector<Image> references;  // identity/style images for reference conditioning
    int width = 64, height = 64;    // sprite size
    std::int64_t seed = -1;         // fixed seed for consistency; -1 = random each time
    int loraStrength = 100;         // percent, 0..200
    int referenceStrength = 60;     // percent, 0..100 (reference-image conditioning)
    bool facingRight = true;
    std::string notes;
};

Status validateCharacter(const CharacterProfile& c);
// A file-name-safe id from the name plus a random suffix.
std::string newCharacterId(const std::string& name);

Result<std::vector<std::uint8_t>> serializeCharacter(const CharacterProfile& c);
Result<CharacterProfile> deserializeCharacter(std::span<const std::uint8_t> bytes);
Status saveCharacter(const CharacterProfile& c, const std::filesystem::path& path);
Result<CharacterProfile> loadCharacter(const std::filesystem::path& path);

struct CharacterEntry {
    std::filesystem::path path;
    std::string id, name;
};
// The .acechar files in `dir` (unreadable files are skipped and logged), by name.
std::vector<CharacterEntry> listCharacters(const std::filesystem::path& dir);

} // namespace acestudio
