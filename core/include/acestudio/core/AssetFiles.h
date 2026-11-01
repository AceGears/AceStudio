#pragma once

// Writing generated assets to disk, with their metadata and engine files
// (Phase 7). One implementation shared by the app, acestudio-cli, the Lua API
// and the MCP server.
//
// Nothing is written when any target already exists and `overwrite` is false
// (the error names the first one). Files are written atomically.

#include "acestudio/core/AssetTools.h"
#include "acestudio/core/Atlas.h"
#include "acestudio/core/Autotile.h"
#include "acestudio/core/EngineExport.h"
#include "acestudio/core/Export.h"
#include "acestudio/core/Result.h"

#include <filesystem>
#include <string>
#include <vector>

namespace acestudio {

class Document;

enum EngineTarget : unsigned {
    kEngineNone = 0,
    kEngineGodot = 1,
    kEngineUnity = 2,
    kEngineTiled = 4,
    // Also Unity Rule Tiles (needs com.unity.2d.tilemap.extras) for Blob47
    // and Cardinal16; implies kEngineUnity.
    kEngineUnityRuleTiles = 8,
};
// "godot", "unity", "unity-rule-tiles", "tiled", comma separated, or "all" /
// "none" ("all" leaves out Rule Tiles).
Result<unsigned> parseEngineTargets(const std::string& list);

struct AssetWriteOptions {
    unsigned engines = kEngineNone;
    bool overwrite = false;
    int pixelsPerUnit = 0;            // Unity; 0 = the tile / frame height
    // Write nothing: return the files that would be written, and in
    // `existing` the ones already there (so a UI can ask once).
    bool dryRun = false;
    // writeSheetFiles only: the engine files, not the sheet PNG and JSON
    // (for exports that write the sheet themselves).
    bool engineFilesOnly = false;
};

struct WrittenFiles {
    std::vector<std::filesystem::path> files;    // in the order written
    std::vector<std::string> notes;              // limits the user should know about
    std::vector<std::filesystem::path> existing; // dry run: targets that already exist
};

Status writeTextFile(const std::filesystem::path& path, const std::string& text);

// Tileset levels from the generator: <stem>.png + .json for one level, or
// <stem>_levelN.png/.json + <stem>_terrain.json for several; <stem>_base.png
// when level 1 has a base tile. Engines: <stem>.tres (Godot TileSet),
// <stem>_levelN.tsx (Tiled), .png.meta per image (Unity, sliced into tiles).
Result<WrittenFiles> writeTilesetFiles(const std::vector<Tileset>& levels, const std::filesystem::path& png,
                                       const AssetWriteOptions& opts, const GodotTerrainNames& names = {});

// A document's frames as a sprite sheet: <png> + Aseprite-style JSON, and
// engines: <stem>.tres (Godot SpriteFrames, one animation per tag),
// <png>.meta + <stem>_<tag>.anim (+ .meta) per animation (Unity), <stem>.tsx
// with tile animations for grid layouts (Tiled).
Result<WrittenFiles> writeSheetFiles(const Document& doc, const FrameRange& range, const SheetOptions& sheetOpts,
                                     const std::filesystem::path& png, const AssetWriteOptions& opts);

// A packed atlas: <png> (several pages: <stem>_0.png, <stem>_1.png, ...) +
// one TexturePacker-style JSON; a Unity .meta per page with one sprite per
// item on it and their 9-slice borders.
Result<WrittenFiles> writeAtlasFiles(const Atlas& atlas, const std::filesystem::path& png, const AssetWriteOptions& opts,
                                     const std::vector<NineSlice>& borders = {});

// A UI kit: atlas PNG + JSON with 9-slice data, Godot StyleBoxTexture .tres
// per stretchable element (<stem>_<name>.tres), Unity .meta with borders.
Result<WrittenFiles> writeUiKitFiles(const std::vector<UiElement>& kit, const std::filesystem::path& png,
                                     const AssetWriteOptions& opts);

} // namespace acestudio
