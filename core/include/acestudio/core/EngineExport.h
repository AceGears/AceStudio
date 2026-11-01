#pragma once

// Engine export (Phase 7): resources that game engines and editors load
// directly, written next to the PNGs AceStudio already produces.
//
//   Godot 4.x  SpriteFrames (.tres) for animations, TileSet (.tres) with
//              terrain sets for autotiling, StyleBoxTexture (.tres) for
//              9-slice UI elements.
//   Unity      TextureImporter .meta files that slice a sheet into named
//              sprites (with 9-slice borders and pivots), AnimationClip
//              (.anim) files per animation, and Rule Tiles (2D Tilemap
//              Extras) for Blob47 and Cardinal16 tilesets.
//   Tiled      .tsx tilesets with Wang sets for the terrain brush, and .tsx
//              tilesets with tile animations for grid sprite sheets.
//
// All output is text and deterministic: the same input always gives the same
// bytes (GUIDs and IDs are derived from names, not random).

#include "acestudio/core/AssetTools.h"
#include "acestudio/core/Autotile.h"
#include "acestudio/core/Export.h"
#include "acestudio/core/Image.h"

#include <cstdint>
#include <string>
#include <vector>

namespace acestudio {

class Document;

// A sprite sheet described for engines: named frames (rects in the sheet
// image, top-left origin) and animations over them.
struct EngineAnimation {
    std::string name;
    std::vector<int> frames;         // indices into EngineSheet::frames, in playback order
    std::vector<int> durationsMs;    // one per entry of `frames`
    bool loop = true;
};
struct EngineFrame {
    std::string name;
    IntRect rect;                    // in the sheet image
    NineSlice border;                // zero = not stretchable
    double pivotX = 0.5, pivotY = 1.0;   // 0..1 from the left / from the top; default bottom centre
};
struct EngineSheet {
    int width = 0, height = 0;       // sheet image size
    std::vector<EngineFrame> frames;
    std::vector<EngineAnimation> animations;
};

// Frames "<baseName>_<n>" and one animation per tag (reverse and ping-pong
// tags expanded, repeat counts as non-looping); "default" over every frame
// when the document has no tags in the exported range.
EngineSheet engineSheet(const Document& doc, const SpriteSheet& sheet, const std::string& baseName);
// Frames from an atlas (one per sprite, no animations), e.g. a UI kit.
EngineSheet engineSheet(const Atlas& atlas, int page, const std::vector<NineSlice>& borders = {});

// ---- Godot 4

// `imagePath` is a res:// path to the sheet PNG.
std::string godotSpriteFrames(const EngineSheet& sheet, const std::string& imagePath);

// One TileSet for a terrain level set (Tileset levels as from the Tileset
// Generator, all the same scheme, tile size and phases). Each level becomes
// an atlas source and a terrain set: level 1 has terrains {base, terrain 1},
// higher levels {terrain k} over empty cells, matching renderLevels when each
// level is painted on its own TileMapLayer. Mode: Blob47 = match corners and
// sides, Wang16 = match corners, Cardinal16 = match sides. Only phase 0 of a
// set with phases is given terrain data (Godot picks among matching tiles at
// random, which would break the phase pattern). `baseImagePath` is the base
// tile PNG (Blob47 / Cardinal16 level 1), empty if there is none.
struct GodotTerrainNames {
    std::string base = "base";
    std::vector<std::string> levels;     // one per level; default "terrain 1", ...
};
std::string godotTileSet(const std::vector<Tileset>& levels, const std::vector<std::string>& imagePaths,
                         const std::string& baseImagePath, const GodotTerrainNames& names = {});

// A StyleBoxTexture for a 9-slice element at `region` of `imagePath`.
std::string godotStyleBox(const std::string& imagePath, const IntRect& region, const NineSlice& slice);

// ---- Unity

// A stable 32-hex-digit GUID / 64-bit file ID derived from a string.
std::string unityGuid(const std::string& seed);
std::int64_t unityFileId(const std::string& seed);

// TextureImporter settings for a pixel-art sprite sheet: Sprite (2D and UI),
// Multiple, point filtering, no compression, `pixelsPerUnit`, one sprite per
// frame (Unity's rect origin is the bottom-left, converted here) with its
// border and pivot. `guid` is the texture's GUID (unityGuid of its path).
std::string unityTextureMeta(const EngineSheet& sheet, const std::string& guid, int pixelsPerUnit);
// An AnimationClip that swaps the SpriteRenderer's sprite; `textureGuid` is
// the sheet's GUID. Sample rate from the shortest frame; durations are
// rounded to whole samples.
std::string unityAnimationClip(const EngineSheet& sheet, std::size_t animation, const std::string& textureGuid);
// A plain .meta for a generated asset (the .anim), so Unity keeps its GUID.
std::string unityAssetMeta(const std::string& guid, std::int64_t mainObjectFileId);

// The RuleTile script of com.unity.2d.tilemap.extras (RuleTile.cs.meta).
inline constexpr const char* kUnityRuleTileScriptGuid = "9d1514134bc4fbd41bb739b1b9a49231";
// A Rule Tile for a Blob47 or Cardinal16 level: one rule per tile, with the
// neighbours as This / Not This (Blob47 corners only where both sides are
// set, like AceStudio's placement). The sprites are `spriteStem_<k>` of the
// texture `textureGuid` (as unityTextureMeta names them); phase 0 only.
// Empty for Wang16 (a dual grid, which Rule Tiles cannot express).
std::string unityRuleTile(const Tileset& t, const std::string& name, const std::string& spriteStem, const std::string& textureGuid);
// A plain Tile (UnityEngine.Tilemaps.Tile) showing one sprite.
std::string unityTile(const std::string& name, std::int64_t spriteFileId, const std::string& textureGuid);

// ---- Tiled

// A .tsx tileset with one Wang set: Blob47 = mixed, Wang16 = corner,
// Cardinal16 = edge. Colour 1 is the terrain, colour 2 the base (left out for
// a level with a transparent base, whose empty side is colour 0). Only phase 0.
std::string tiledTileset(const Tileset& t, const std::string& imageFile, const std::string& name,
                         const std::string& terrainName, const std::string& baseName);
// A .tsx for a sprite sheet whose frames sit on a regular grid (pass only
// untrimmed horizontal, vertical or grid layouts): one tile per cell, and one tile
// animation per animation, on the tile of its first frame. Empty when the
// frames do not form a grid.
std::string tiledSpriteSheet(const EngineSheet& sheet, const std::string& imageFile, const std::string& name);

} // namespace acestudio
