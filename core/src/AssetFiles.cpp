#include "acestudio/core/AssetFiles.h"

#include "acestudio/core/Document.h"
#include "acestudio/core/FileUtil.h"
#include "acestudio/core/PngCodec.h"

#include <algorithm>
#include <set>
#include <sstream>

namespace acestudio {

namespace fs = std::filesystem;

Result<unsigned> parseEngineTargets(const std::string& list) {
    unsigned out = kEngineNone;
    std::stringstream ss(list);
    std::string item;
    while (std::getline(ss, item, ',')) {
        item.erase(std::remove_if(item.begin(), item.end(), [](char c) { return c == ' '; }), item.end());
        std::transform(item.begin(), item.end(), item.begin(), [](char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c); });
        if (item.empty() || item == "none") continue;
        if (item == "all") out |= kEngineGodot | kEngineUnity | kEngineTiled;
        else if (item == "godot") out |= kEngineGodot;
        else if (item == "unity") out |= kEngineUnity;
        else if (item == "tiled") out |= kEngineTiled;
        else return Error{ErrorCode::InvalidArgument, "Unknown engine \"" + item + "\" (use godot, unity, tiled, all or none)"};
    }
    return out;
}

Status writeTextFile(const fs::path& path, const std::string& text) {
    return writeFileAtomic(path, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

namespace {

// Files to write, collected first so nothing is written when one exists.
class Plan {
public:
    void image(fs::path p, Image img) { m_items.push_back({std::move(p), std::move(img), {}, true}); }
    void text(fs::path p, std::string s) { m_items.push_back({std::move(p), {}, std::move(s), false}); }

    Result<WrittenFiles> commit(bool overwrite, std::vector<std::string> notes) {
        std::set<fs::path> seen;
        for (const auto& it : m_items) {
            if (!seen.insert(it.path).second) return Error{ErrorCode::Internal, "Two outputs share the path " + it.path.string()};
            std::error_code ec;
            if (!overwrite && fs::exists(it.path, ec))
                return Error{ErrorCode::IoError, it.path.string() + " already exists (use overwrite / --force to replace it)"};
        }
        WrittenFiles w;
        for (const auto& it : m_items) {
            Status st = it.isImage ? writePng(it.img, it.path) : writeTextFile(it.path, it.text);
            if (!st) return st.error();
            w.files.push_back(it.path);
        }
        w.notes = std::move(notes);
        return w;
    }

private:
    struct Item {
        fs::path path;
        Image img;
        std::string text;
        bool isImage = false;
    };
    std::vector<Item> m_items;
};

std::string fileName(const fs::path& p) { return p.filename().string(); }

// Unity: a texture's GUID from its file name, so re-exporting keeps it.
std::string guidFor(const fs::path& png) { return unityGuid("acestudio:" + fileName(png)); }

EngineSheet tileGrid(const Tileset& t, const std::string& stem) {
    // Every tile of every phase as its own sprite, named <stem>_<index>[_pX_Y].
    EngineSheet s;
    s.width = t.image.width();
    s.height = t.image.height();
    const auto [cols, rows] = tilesetGrid(t.scheme);
    (void)rows;
    for (int py = 0; py < t.phases; ++py)
        for (int px = 0; px < t.phases; ++px)
            for (int k = 0; k < tileCount(t.scheme); ++k) {
                EngineFrame f;
                f.name = stem + "_" + std::to_string(k) + (t.phases > 1 ? "_p" + std::to_string(px) + "_" + std::to_string(py) : std::string());
                f.rect = {(px * cols + k % cols) * t.tileSize, (py * tilesetGrid(t.scheme).second + k / cols) * t.tileSize, t.tileSize, t.tileSize};
                f.pivotX = 0.5;
                f.pivotY = 0.5;
                s.frames.push_back(f);
            }
    return s;
}

int ppuOr(int ppu, int fallback) { return ppu > 0 ? ppu : std::max(1, fallback); }

} // namespace

Result<WrittenFiles> writeTilesetFiles(const std::vector<Tileset>& levels, const fs::path& pngIn, const AssetWriteOptions& o,
                                       const GodotTerrainNames& names) {
    if (levels.empty()) return Error{ErrorCode::InvalidArgument, "No tileset to write"};
    fs::path png = pngIn;
    if (png.extension() != ".png") png += ".png";
    const fs::path dir = png.parent_path();
    const std::string stem = png.stem().string();
    const bool single = levels.size() == 1;
    const Tileset& first = levels.front();
    const bool withBase = !first.base.isNull() && first.scheme != AutotileScheme::Wang16;
    Plan plan;
    std::vector<std::string> notes;
    const fs::path base = dir / (stem + "_base.png");
    if (withBase) plan.image(base, first.base);
    std::vector<std::string> images, jsons;
    for (std::size_t i = 0; i < levels.size(); ++i) {
        const std::string name = single ? stem : stem + "_level" + std::to_string(i + 1);
        const fs::path img = dir / (name + ".png"), js = dir / (name + ".json");
        plan.image(img, levels[i].image);
        plan.text(js, tilesetJson(levels[i], fileName(img), i == 0 && withBase ? fileName(base) : std::string()));
        images.push_back(fileName(img));
        jsons.push_back(fileName(js));
        if (o.engines & kEngineTiled) {
            const std::string terrain = i < names.levels.size() && !names.levels[i].empty() ? names.levels[i] : "terrain " + std::to_string(i + 1);
            plan.text(dir / (name + ".tsx"), tiledTileset(levels[i], fileName(img), name, terrain, names.base));
        }
        if (o.engines & kEngineUnity)
            plan.text(fs::path(img.string() + ".meta"),
                      unityTextureMeta(tileGrid(levels[i], name), guidFor(img), ppuOr(o.pixelsPerUnit, levels[i].tileSize)));
    }
    if (withBase && (o.engines & kEngineUnity)) {
        EngineSheet b;
        b.width = first.base.width();
        b.height = first.base.height();
        b.frames.push_back({stem + "_base", {0, 0, b.width, b.height}, {}, 0.5, 0.5});
        plan.text(fs::path(base.string() + ".meta"), unityTextureMeta(b, guidFor(base), ppuOr(o.pixelsPerUnit, first.tileSize)));
    }
    if (!single) plan.text(dir / (stem + "_terrain.json"), terrainSetJson(levels, images, jsons, withBase ? fileName(base) : std::string()));
    if (o.engines & kEngineGodot)
        plan.text(dir / (stem + ".tres"), godotTileSet(levels, images, withBase ? fileName(base) : std::string(), names));
    if (first.phases > 1 && (o.engines & (kEngineGodot | kEngineTiled)))
        notes.push_back("Godot and Tiled terrain brushes use the first of the " + std::to_string(first.phases) + "x" +
                        std::to_string(first.phases) + " tile variants only; the others are in the image for placing by position "
                        "(cell x mod " + std::to_string(first.phases) + ", y mod " + std::to_string(first.phases) + ")");
    if (levels.size() > 1 && (o.engines & kEngineGodot))
        notes.push_back("Godot: paint each terrain level on its own TileMapLayer (terrain set N on layer N), lowest first, and "
                        "paint the base a little beyond the area you use: Godot clears tiles at the edge of a painted area");
    if (first.scheme == AutotileScheme::Wang16 && levels.size() > 1 && (o.engines & kEngineGodot))
        notes.push_back("Godot's corner mode works on the cell grid: a single isolated cell of an upper level stays empty "
                        "(AceStudio draws Wang tiles on the dual grid); paint upper levels at least 2x2");
    if (o.engines & kEngineUnity)
        notes.push_back("Unity: the images are sliced into one sprite per tile; Unity's Rule Tiles are not generated");
    return plan.commit(o.overwrite, std::move(notes));
}

Result<WrittenFiles> writeSheetFiles(const Document& doc, const FrameRange& range, const SheetOptions& so, const fs::path& pngIn,
                                     const AssetWriteOptions& o) {
    fs::path png = pngIn;
    if (png.extension() != ".png") png += ".png";
    auto sheet = buildSpriteSheet(doc, range, so);
    if (!sheet) return sheet.error();
    const std::string stem = png.stem().string();
    const fs::path dir = png.parent_path();
    Plan plan;
    plan.image(png, sheet.value().image);
    plan.text(dir / (stem + ".json"), spriteSheetJson(doc, sheet.value(), fileName(png), stem, JsonStyle::Hash));
    const EngineSheet es = engineSheet(doc, sheet.value(), stem);
    if (o.engines & kEngineGodot) plan.text(dir / (stem + ".tres"), godotSpriteFrames(es, fileName(png)));
    if (o.engines & kEngineUnity) {
        const std::string guid = guidFor(png);
        plan.text(fs::path(png.string() + ".meta"), unityTextureMeta(es, guid, ppuOr(o.pixelsPerUnit, sheet.value().frameHeight)));
        std::set<std::string> used;
        for (std::size_t i = 0; i < es.animations.size(); ++i) {
            std::string name = stem + "_" + es.animations[i].name;
            for (char& c : name)
                if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
            while (!used.insert(name).second) name += "_";
            const fs::path anim = dir / (name + ".anim");
            plan.text(anim, unityAnimationClip(es, i, guid));
            plan.text(fs::path(anim.string() + ".meta"), unityAssetMeta(unityGuid("acestudio:" + fileName(anim)), 7400000));
        }
    }
    std::vector<std::string> notes;
    if (o.engines & kEngineTiled) notes.push_back("Tiled has no sprite animation format; only the sheet and JSON were written for it");
    return plan.commit(o.overwrite, std::move(notes));
}

Result<WrittenFiles> writeAtlasFiles(const Atlas& atlas, const fs::path& pngIn, const AssetWriteOptions& o,
                                     const std::vector<NineSlice>& borders) {
    if (atlas.pages.size() != 1) return Error{ErrorCode::InvalidArgument, "Engine files need a single-page atlas"};
    fs::path png = pngIn;
    if (png.extension() != ".png") png += ".png";
    const fs::path dir = png.parent_path();
    Plan plan;
    plan.image(png, atlas.pages.front());
    plan.text(dir / (png.stem().string() + ".json"), atlasJson(atlas, {fileName(png)}));
    if (o.engines & kEngineUnity) {
        bool rotated = false;
        for (const auto& s : atlas.sprites) rotated = rotated || s.rotated;
        if (rotated) return Error{ErrorCode::InvalidArgument, "Unity cannot use rotated atlas sprites; pack without rotation"};
        plan.text(fs::path(png.string() + ".meta"), unityTextureMeta(engineSheet(atlas, 0, borders), guidFor(png), ppuOr(o.pixelsPerUnit, 16)));
    }
    return plan.commit(o.overwrite, {});
}

Result<WrittenFiles> writeUiKitFiles(const std::vector<UiElement>& kit, const fs::path& pngIn, const AssetWriteOptions& o) {
    fs::path png = pngIn;
    if (png.extension() != ".png") png += ".png";
    auto files = packUiKit(kit, fileName(png));
    if (!files) return files.error();
    const Atlas& atlas = files.value().atlas;
    const fs::path dir = png.parent_path();
    const std::string stem = png.stem().string();
    Plan plan;
    plan.image(png, atlas.pages.front());
    plan.text(dir / (stem + ".json"), files.value().json);
    std::vector<NineSlice> borders;
    for (const auto& e : kit) borders.push_back(e.slice);
    if (o.engines & kEngineGodot)
        for (std::size_t i = 0; i < kit.size(); ++i)
            if (kit[i].stretchable) plan.text(dir / (stem + "_" + kit[i].name + ".tres"), godotStyleBox(fileName(png), atlas.sprites[i].rect, kit[i].slice));
    if (o.engines & kEngineUnity)
        plan.text(fs::path(png.string() + ".meta"), unityTextureMeta(engineSheet(atlas, 0, borders), guidFor(png), ppuOr(o.pixelsPerUnit, 16)));
    std::vector<std::string> notes;
    if (o.engines & kEngineTiled) notes.push_back("Tiled has no UI format; only the atlas and JSON were written for it");
    return plan.commit(o.overwrite, std::move(notes));
}

} // namespace acestudio
