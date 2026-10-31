#include "acestudio/core/Character.h"

#include "ZipArchive.h"
#include "acestudio/core/FileUtil.h"
#include "acestudio/core/Log.h"
#include "acestudio/core/PngCodec.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <random>

namespace acestudio {

using nlohmann::json;
namespace fs = std::filesystem;

namespace {
constexpr const char* kFormat = "acestudio.character";
constexpr std::uint64_t kMaxFileBytes = 64ull << 20;
Error corrupt(std::string m) { return Error{ErrorCode::CorruptData, std::move(m)}; }
std::span<const std::uint8_t> bytesOf(const std::string& s) { return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()}; }
} // namespace

Status validateCharacter(const CharacterProfile& c) {
    auto bad = [](std::string m) { return Status{Error{ErrorCode::InvalidArgument, std::move(m)}}; };
    if (c.name.empty() || c.name.size() > 100) return bad("A character needs a name of up to 100 characters");
    if (c.id.empty() || c.id.size() > 120 || c.id.find_first_of("/\\:*?\"<>|") != std::string::npos || c.id[0] == '.')
        return bad("Invalid character id");
    if (c.prompt.size() > 2000 || c.negative.size() > 2000) return bad("Prompts are limited to 2000 characters");
    if (c.notes.size() > 10000) return bad("Notes are limited to 10000 characters");
    if (c.width < 8 || c.width > 1024 || c.height < 8 || c.height > 1024) return bad("Sprite size must be 8 to 1024 px");
    if (c.references.size() > static_cast<std::size_t>(CharacterProfile::kMaxReferences))
        return bad("At most " + std::to_string(CharacterProfile::kMaxReferences) + " reference images");
    for (const auto& r : c.references)
        if (r.isNull() || r.width() > 2048 || r.height() > 2048) return bad("Reference images must be 1 to 2048 px");
    if (c.loraStrength < 0 || c.loraStrength > 200) return bad("Style strength must be 0 to 200 %");
    if (c.referenceStrength < 0 || c.referenceStrength > 100) return bad("Reference strength must be 0 to 100 %");
    if (c.palette.size() > Palette::kMaxColors) return bad("Palettes hold at most 256 colours");
    return {};
}

std::string newCharacterId(const std::string& name) {
    std::string base;
    for (char ch : name) {
        const unsigned char u = static_cast<unsigned char>(ch);
        if (std::isalnum(u)) base += static_cast<char>(std::tolower(u));
        else if (!base.empty() && base.back() != '-') base += '-';
        if (base.size() >= 40) break;
    }
    while (!base.empty() && base.back() == '-') base.pop_back();
    if (base.empty()) base = "character";
    std::random_device rd;
    std::uniform_int_distribution<unsigned> dist(0, 0xFFFFFF);
    char suffix[8];
    std::snprintf(suffix, sizeof suffix, "%06x", dist(rd));
    return base + "-" + suffix;
}

Result<std::vector<std::uint8_t>> serializeCharacter(const CharacterProfile& c) {
    if (auto st = validateCharacter(c); !st) return st.error();
    json colors = json::array();
    for (const auto& p : c.palette.colors()) colors.push_back(p.toHex(true));
    json j = {{"format", kFormat},
              {"schemaVersion", CharacterProfile::kSchemaVersion},
              {"id", c.id},
              {"name", c.name},
              {"prompt", c.prompt},
              {"negative", c.negative},
              {"palette", colors},
              {"references", static_cast<int>(c.references.size())},
              {"size", {c.width, c.height}},
              {"seed", c.seed},
              {"loraStrength", c.loraStrength},
              {"referenceStrength", c.referenceStrength},
              {"facing", c.facingRight ? "right" : "left"},
              {"notes", c.notes}};
    detail::ZipWriter zip;
    const std::string text = j.dump(2);
    if (auto st = zip.add("character.json", bytesOf(text), true); !st) return st.error();
    for (std::size_t i = 0; i < c.references.size(); ++i) {
        auto png = encodePng(c.references[i]);
        if (!png) return png.error();
        if (auto st = zip.add("refs/" + std::to_string(i) + ".png", png.value(), false); !st) return st.error();
    }
    return zip.finish();
}

Result<CharacterProfile> deserializeCharacter(std::span<const std::uint8_t> bytes) {
    detail::ZipReader zip(bytes);
    if (!zip.ok()) return corrupt("Not a character file (ZIP expected)");
    auto text = zip.read("character.json");
    if (!text) return text.error();
    json j;
    try {
        j = json::parse(text.value().begin(), text.value().end());
    } catch (const json::exception& e) {
        return corrupt(std::string("character.json is not valid JSON: ") + e.what());
    }
    try {
        if (j.value("format", std::string()) != kFormat) return corrupt("Not a character file");
        if (j.value("schemaVersion", 0) > CharacterProfile::kSchemaVersion)
            return Error{ErrorCode::UnsupportedVersion, "This character was made by a newer AceStudio"};
        CharacterProfile c;
        c.id = j.at("id").get<std::string>();
        c.name = j.at("name").get<std::string>();
        c.prompt = j.value("prompt", std::string());
        c.negative = j.value("negative", std::string());
        for (const auto& hex : j.value("palette", json::array())) {
            auto col = Rgba8::fromHex(hex.get<std::string>());
            if (!col) return corrupt("Invalid palette colour");
            if (auto st = c.palette.add(*col); !st) return corrupt(st.error().message);
        }
        const int refs = j.value("references", 0);
        if (refs < 0 || refs > CharacterProfile::kMaxReferences) return corrupt("Invalid reference count");
        for (int i = 0; i < refs; ++i) {
            auto png = zip.read("refs/" + std::to_string(i) + ".png");
            if (!png) return png.error();
            auto img = decodePng(png.value());
            if (!img) return corrupt("Reference " + std::to_string(i) + ": " + img.error().message);
            c.references.push_back(std::move(img).value());
        }
        const auto size = j.value("size", json::array({64, 64}));
        if (!size.is_array() || size.size() != 2) return corrupt("Invalid sprite size");
        c.width = size[0].get<int>();
        c.height = size[1].get<int>();
        c.seed = j.value("seed", std::int64_t{-1});
        c.loraStrength = j.value("loraStrength", 100);
        c.referenceStrength = j.value("referenceStrength", 60);
        c.facingRight = j.value("facing", std::string("right")) != "left";
        c.notes = j.value("notes", std::string());
        if (auto st = validateCharacter(c); !st) return corrupt(st.error().message);
        return c;
    } catch (const json::exception& e) {
        return corrupt(std::string("character.json: ") + e.what());
    }
}

Status saveCharacter(const CharacterProfile& c, const fs::path& path) {
    auto bytes = serializeCharacter(c);
    if (!bytes) return bytes.error();
    return writeFileAtomic(path, bytes.value());
}

Result<CharacterProfile> loadCharacter(const fs::path& path) {
    auto bytes = readFileBytes(path, kMaxFileBytes);
    if (!bytes) return bytes.error();
    return deserializeCharacter(bytes.value());
}

std::vector<CharacterEntry> listCharacters(const fs::path& dir) {
    std::vector<CharacterEntry> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec) || e.path().extension() != ".acechar") continue;
        auto c = loadCharacter(e.path());
        if (!c) { ACE_LOG_WARN("characters", "Skipping " + e.path().filename().string() + ": " + c.error().message); continue; }
        out.push_back({e.path(), c.value().id, c.value().name});
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    return out;
}

} // namespace acestudio
