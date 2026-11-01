#include "acestudio/core/AssetFiles.h"

#include "acestudio/core/Document.h"
#include "acestudio/core/FileUtil.h"
#include "acestudio/core/PngCodec.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
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
        if (item == "all") out |= kEngineGodot | kEngineUnity | kEngineTiled | kEnginePhaser | kEngineGameMaker;
        else if (item == "godot") out |= kEngineGodot;
        else if (item == "unity") out |= kEngineUnity;
        else if (item == "unity-rule-tiles" || item == "unity-ruletiles") out |= kEngineUnity | kEngineUnityRuleTiles;
        else if (item == "tiled") out |= kEngineTiled;
        else if (item == "phaser") out |= kEnginePhaser;
        else if (item == "gamemaker" || item == "gms2") out |= kEngineGameMaker;
        else return Error{ErrorCode::InvalidArgument,
                          "Unknown engine \"" + item + "\" (use godot, unity, unity-rule-tiles, tiled, phaser, gamemaker, all or none)"};
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
    // A file that may already be there with the same content (shared
    // scripts): then it is left alone and not reported.
    void shared(fs::path p, std::string s) {
        Item it{std::move(p), {}, std::move(s), false};
        it.skipIfSame = true;
        m_items.push_back(std::move(it));
    }

    Result<WrittenFiles> commit(bool overwrite, std::vector<std::string> notes, bool dryRun = false) {
        std::set<fs::path> seen;
        WrittenFiles w;
        for (auto& it : m_items) {
            if (!seen.insert(it.path).second) return Error{ErrorCode::Internal, "Two outputs share the path " + it.path.string()};
            std::error_code ec;
            const bool exists = fs::exists(it.path, ec);
            if (exists && it.skipIfSame) {
                auto bytes = readFileBytes(it.path, 4u << 20);
                if (bytes && std::string(bytes.value().begin(), bytes.value().end()) == it.text) {
                    it.skip = true;
                    continue;
                }
            }
            if (dryRun) {
                w.files.push_back(it.path);
                if (exists) w.existing.push_back(it.path);
                continue;
            }
            if (!overwrite && exists)
                return Error{ErrorCode::IoError, it.path.string() + " already exists (use overwrite / --force to replace it)"};
        }
        if (dryRun) {
            w.notes = std::move(notes);
            return w;
        }
        for (const auto& it : m_items) {
            if (it.skip) continue;
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
        bool skipIfSame = false;
        bool skip = false;
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

// Where AceStudio's Unity tile scripts go: if `dir` is inside a Unity project
// that already has them (anywhere under Assets), there; otherwise into `dir`.
// One copy per project, so their classes are not defined twice.
fs::path unityScriptPath(const fs::path& dir, const std::string& file) {
    std::error_code ec;
    fs::path abs = fs::weakly_canonical(fs::absolute(dir, ec), ec);
    for (fs::path p = abs; !p.empty(); p = p.parent_path()) {
        if (fs::is_directory(p / "Assets", ec) && fs::is_directory(p / "ProjectSettings", ec)) {
            int budget = 20000;   // entries looked at, so a huge project cannot stall an export
            for (fs::recursive_directory_iterator it(p / "Assets", fs::directory_options::skip_permission_denied, ec), end;
                 !ec && it != end && --budget > 0; it.increment(ec))
                if (it->path().filename() == file) return it->path();
            break;
        }
        if (p == p.parent_path()) break;
    }
    return dir / file;
}

void addUnityScripts(Plan& plan, const fs::path& dir) {
    for (const UnityScript& sc : unityTileScripts()) {
        const fs::path cs = unityScriptPath(dir, sc.file);
        plan.shared(cs, sc.text);
        plan.shared(fs::path(cs.string() + ".meta"), unityScriptMeta(sc.guid));
    }
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
        if (o.engines & kEngineUnity) {
            plan.text(fs::path(img.string() + ".meta"),
                      unityTextureMeta(tileGrid(levels[i], name), guidFor(img), ppuOr(o.pixelsPerUnit, levels[i].tileSize)));
            // AceStudio's tiles (no package needed): an autotile, or a Wang set for the dual grid.
            const std::string tile = levels[i].scheme == AutotileScheme::Wang16 ? unityWangSet(levels[i], name, name, guidFor(img))
                                                                                : unityAutoTile(levels[i], name, name, guidFor(img));
            const fs::path asset = dir / (name + ".asset");
            plan.text(asset, tile);
            plan.text(fs::path(asset.string() + ".meta"), unityAssetMeta(unityGuid("acestudio:" + fileName(asset)), 11400000));
            if (o.engines & kEngineUnityRuleTiles) {
                const std::string rule = unityRuleTile(levels[i], name + "_ruletile", name, guidFor(img));
                if (!rule.empty()) {
                    const fs::path ruleAsset = dir / (name + "_ruletile.asset");
                    plan.text(ruleAsset, rule);
                    plan.text(fs::path(ruleAsset.string() + ".meta"), unityAssetMeta(unityGuid("acestudio:" + fileName(ruleAsset)), 11400000));
                }
            }
        }
    }
    if (withBase && (o.engines & kEngineUnity)) {
        EngineSheet b;
        b.width = first.base.width();
        b.height = first.base.height();
        b.frames.push_back({stem + "_base", {0, 0, b.width, b.height}, {}, 0.5, 0.5});
        plan.text(fs::path(base.string() + ".meta"), unityTextureMeta(b, guidFor(base), ppuOr(o.pixelsPerUnit, first.tileSize)));
        const fs::path asset = dir / (stem + "_base.asset");
        plan.text(asset, unityTile(stem + "_base", unityFileId(guidFor(base) + "/" + stem + "_base"), guidFor(base)));
        plan.text(fs::path(asset.string() + ".meta"), unityAssetMeta(unityGuid("acestudio:" + fileName(asset)), 11400000));
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
    if (o.engines & kEngineUnity) {
        addUnityScripts(plan, dir);
        if (first.scheme == AutotileScheme::Wang16)
            notes.push_back("Unity: add the AceStudioDualGrid component to a Tilemap, set its Wang Set (the .asset), and paint with "
                            "the set's _terrain and _ground tiles; one Tilemap per level, lowest first. The AceStudio*.cs scripts "
                            "need no package; keep one copy per project");
        else
            notes.push_back("Unity: paint each level's .asset autotile on its own Tilemap, lowest first" +
                            std::string(withBase ? ", and the base with the _base tile" : "") +
                            ". The AceStudio*.cs scripts need no package; keep one copy per project");
        if ((o.engines & kEngineUnityRuleTiles) && first.scheme == AutotileScheme::Wang16)
            notes.push_back("Unity: Rule Tiles cannot express Wang16's dual grid; use the AceStudioDualGrid tiles");
        else if (o.engines & kEngineUnityRuleTiles)
            notes.push_back("Unity: the _ruletile.asset files need the 2D Tilemap Extras package (com.unity.2d.tilemap.extras)");
        if (first.phases > 1) notes.push_back("Unity: the tiles use the first of the tile variants only");
    }
    return plan.commit(o.overwrite, std::move(notes), o.dryRun);
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
    if (!o.engineFilesOnly) {
        plan.image(png, sheet.value().image);
        // Phaser looks frames up by number, so its sheets name frames "0", "1", ...
        plan.text(dir / (stem + ".json"),
                  spriteSheetJson(doc, sheet.value(), fileName(png), (o.engines & kEnginePhaser) ? std::string() : stem, JsonStyle::Hash));
    }
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
    if (o.engines & kEnginePhaser) {
        // Phaser 3 reads the Aseprite JSON directly (load.aseprite + anims.createFromAseprite).
        const std::string js = "// Phaser 3 loader for " + stem + " (written by AceStudio).\n"
                               "// In a scene: preload() { loadSheet(this); }  create() { createAnimations(this); }\n"
                               "// Animations are named after the tags; frame durations come from the JSON.\n"
                               "export const key = '" + stem + "';\n"
                               "export function loadSheet(scene, folder = '') {\n"
                               "  scene.load.aseprite(key, folder + '" + fileName(png) + "', folder + '" + stem + ".json');\n"
                               "}\n"
                               "export function createAnimations(scene) {\n"
                               "  const made = scene.anims.createFromAseprite(key);\n"
                               "  // As in Aseprite: a tag without a repeat count loops; \"repeat\": \"N\" plays N times.\n"
                               "  const tags = scene.cache.json.get(key).meta.frameTags || [];\n"
                               "  for (const anim of made) {\n"
                               "    const tag = tags.find((t) => t.name === anim.key);\n"
                               "    anim.repeat = tag && tag.repeat ? Number(tag.repeat) - 1 : -1;\n"
                               "  }\n"
                               "  return made;\n"
                               "}\n";
        plan.text(dir / (stem + "_phaser.js"), js);
    }
    if (o.engines & kEngineGameMaker) {
        // One strip per animation, untrimmed frames side by side; GameMaker
        // splits "<name>_stripN.png" into N frames when it is imported.
        const SpriteSheet& sh = sheet.value();
        std::set<std::string> used;
        std::string speeds = "GameMaker sprites for " + stem + " (drag the PNGs into the Asset Browser, or Import Strip Image).\n"
                             "Set each sprite's speed (frames per second) to:\n";
        for (const EngineAnimation& a : es.animations) {
            if (a.frames.empty()) continue;
            const int n = static_cast<int>(a.frames.size());
            Image strip(sh.frameWidth * n, sh.frameHeight);
            for (int k = 0; k < n; ++k) {
                const SheetFrame& f = sh.frames[static_cast<std::size_t>(a.frames[static_cast<std::size_t>(k)])];
                strip.blit(sh.image.copy(f.rect), k * sh.frameWidth + f.source.x, f.source.y);
            }
            std::string name = stem + "_" + a.name;
            for (char& c : name)
                if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') c = '_';
            while (!used.insert(name).second) name += "_";
            plan.image(dir / (name + "_strip" + std::to_string(n) + ".png"), std::move(strip));
            double total = 0;
            for (int d : a.durationsMs) total += d;
            const double fps = total > 0 ? 1000.0 * n / total : 10.0;
            char line[160];
            std::snprintf(line, sizeof line, "  %s_strip%d: %.2f fps%s\n", name.c_str(), n, fps, a.loop ? "" : " (plays once)");
            speeds += line;
        }
        plan.text(dir / (stem + "_gamemaker.txt"), speeds);
    }
    std::vector<std::string> notes;
    if (o.engines & kEngineTiled) {
        // Trimmed frames are not cells of the frame size; packed ones are not on a grid.
        const std::string tsx = so.trim || so.layout == SheetLayout::Packed ? std::string() : tiledSpriteSheet(es, fileName(png), stem);
        if (!tsx.empty()) plan.text(dir / (stem + ".tsx"), tsx);
        else notes.push_back("Tiled: the frames are not on a regular grid (trimmed or packed), so no .tsx was written; "
                             "export untrimmed with a horizontal, vertical or grid layout for Tiled tile animations");
    }
    return plan.commit(o.overwrite, std::move(notes), o.dryRun);
}

Result<WrittenFiles> writeAtlasFiles(const Atlas& atlas, const fs::path& pngIn, const AssetWriteOptions& o,
                                     const std::vector<NineSlice>& borders) {
    if (atlas.pages.empty()) return Error{ErrorCode::InvalidArgument, "The atlas has no pages"};
    fs::path png = pngIn;
    if (png.extension() != ".png") png += ".png";
    const fs::path dir = png.parent_path();
    // One page: <png>; several: <stem>_0.png, <stem>_1.png, ... and one JSON.
    std::vector<fs::path> pages;
    if (atlas.pages.size() == 1) pages.push_back(png);
    else
        for (std::size_t p = 0; p < atlas.pages.size(); ++p) pages.push_back(dir / (png.stem().string() + "_" + std::to_string(p) + ".png"));
    std::vector<std::string> names;
    for (const auto& p : pages) names.push_back(fileName(p));
    if (o.engines & kEngineUnity) {
        bool rotated = false;
        for (const auto& s : atlas.sprites) rotated = rotated || s.rotated;
        if (rotated) return Error{ErrorCode::InvalidArgument, "Unity cannot use rotated atlas sprites; pack without rotation"};
    }
    Plan plan;
    for (std::size_t p = 0; p < pages.size(); ++p) {
        plan.image(pages[p], atlas.pages[p]);
        if (o.engines & kEngineUnity)
            plan.text(fs::path(pages[p].string() + ".meta"),
                      unityTextureMeta(engineSheet(atlas, static_cast<int>(p), borders), guidFor(pages[p]), ppuOr(o.pixelsPerUnit, 16)));
    }
    plan.text(dir / (png.stem().string() + ".json"), atlasJson(atlas, names));
    return plan.commit(o.overwrite, {}, o.dryRun);
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
    return plan.commit(o.overwrite, std::move(notes), o.dryRun);
}

} // namespace acestudio
