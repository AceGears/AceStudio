#pragma once

// Autotiling (Phase 6): which tile to draw where, for one terrain painted over
// a base, in three common schemes, plus an automated seam check.
//
//   Cardinal16  4-neighbour: one bit per same-terrain side (N=1, E=2, S=4,
//               W=8); 16 tiles, index = mask.
//   Blob47      8-neighbour "blob": sides plus corners, a corner counting only
//               when both of its sides do; 47 distinct tiles.
//   Wang16      corner Wang tiles / dual grid: tiles sit on the cell corners
//               and each tile corner takes the terrain of one of the four cells
//               it touches (TL=1, TR=2, BR=4, BL=8); 16 tiles. The map is
//               drawn as (w+1) x (h+1) tiles offset by half a tile.
//
// Every tile is described by a 3x3 grid of terrain samples (corners, edge
// midpoints, centre). Generators interpolate a transition from it and the seam
// check uses it to know which tiles may sit next to each other: two tiles may
// touch along a side exactly when the three samples on that side agree.

#include "acestudio/core/Image.h"
#include "acestudio/core/Result.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace acestudio {

enum class AutotileScheme { Cardinal16, Blob47, Wang16 };

std::string_view toString(AutotileScheme s) noexcept;
std::optional<AutotileScheme> autotileSchemeFromString(std::string_view s) noexcept;

// Number of tiles and the tileset grid (columns x rows) for a scheme.
int tileCount(AutotileScheme s) noexcept;
std::pair<int, int> tilesetGrid(AutotileScheme s) noexcept;

// Samples in row order: TL, T, TR, L, C, R, BL, B, BR, as terrain coverage
// 0 (base) .. 255 (terrain); Wang16 edge midpoints and centres can be partial.
using TileSamples = std::array<std::uint8_t, 9>;
// The base fill as a pseudo tile (all samples 0); index -1 in seam issues.
constexpr int kBaseTile = -1;
TileSamples tileSamples(AutotileScheme s, int tileIndex);

// Blob47: the 47 reduced 8-bit masks in tileset order (bits N=1, NE=2, E=4,
// SE=8, S=16, SW=32, W=64, NW=128).
const std::vector<std::uint8_t>& blobMasks();

// Terrain painted on a grid of cells (0 = base, 1 = terrain).
class TerrainMap {
public:
    TerrainMap() = default;
    TerrainMap(int width, int height);
    int width() const noexcept { return m_w; }
    int height() const noexcept { return m_h; }
    bool at(int x, int y) const noexcept;            // outside the map: base
    void set(int x, int y, bool terrain) noexcept;
    void fill(bool terrain) noexcept;
    bool operator==(const TerrainMap&) const = default;

private:
    int m_w = 0, m_h = 0;
    std::vector<std::uint8_t> m_cells;
};

// Tile placement for a map. Cardinal16 / Blob47: one entry per cell, -1 for
// base cells (draw the base fill). Wang16: (w+1) x (h+1) entries, all >= 0.
struct TilePlacement {
    int columns = 0, rows = 0;
    std::vector<int> tiles;       // row major
    int at(int x, int y) const { return tiles[static_cast<std::size_t>(y) * static_cast<std::size_t>(columns) + static_cast<std::size_t>(x)]; }
};
TilePlacement autotile(const TerrainMap& map, AutotileScheme s);

// A tileset image laid out in the scheme's grid order (tile i at column
// i % cols, row i / cols).
//
// Phases: a texture larger than one tile (phases x phases tiles) keeps its
// detail by giving every tile phases x phases variants, one per position in
// the texture. The image then holds phases x phases copies of the grid
// (copy (px, py) at px * cols * tileSize, py * rows * tileSize) and a cell
// (x, y) of a map draws phase (x mod phases, y mod phases). For Wang16 the
// dual-grid tile at (x, y) uses the same rule.
struct Tileset {
    AutotileScheme scheme = AutotileScheme::Blob47;
    int tileSize = 16;
    int phases = 1;
    Image image;          // phases*cols*tileSize x phases*rows*tileSize
    Image base;           // phases*tileSize square fill for base cells (Cardinal16/Blob47; may be null or transparent)
    Image tile(int index, int px = 0, int py = 0) const;
};
Status validateTileset(const Tileset& t);

// Draws the map with the tileset (base cells use the base tile).
Result<Image> renderMap(const TerrainMap& map, const Tileset& t);

// Seam check: every pair of tiles that may touch (by their samples), on both
// axes and in every phase, compared along the shared edge pixel by pixel. An
// edge is a seam when most of its pixels either jump much more than the image
// changes on either side of the edge (a texture that does not continue) or
// form a line along it (a border drawn on the tiles). Outline corners and
// ragged edges change only a few pixels and do not count. Transparent layers
// are judged over grey.
struct SeamIssue {
    int a = 0, b = 0;           // tile indices (kBaseTile = the base fill); b is right of (horizontal) or below (vertical) a
    bool horizontal = true;
    double jump = 0;            // mean colour difference across the edge (0..1)
    double typical = 0;         // mean difference next to the edge on either side (larger of the two)
    int phaseX = 0, phaseY = 0; // phase of tile a (b is in the next phase along the edge)
};
struct SeamReport {
    int pairsChecked = 0;
    std::vector<SeamIssue> issues;     // sorted by jump, largest first
    bool ok() const noexcept { return issues.empty(); }
};
struct SeamOptions {
    double factor = 2.5;        // a pixel jumps when it changes factor x more than beside the edge ...
    double floor = 0.06;        // ... and by at least this much (0..1); also the size of a line
};
Result<SeamReport> checkSeams(const Tileset& t, const SeamOptions& opts = {});

// Whether an image tiles seamlessly with itself (wrap check on both axes,
// same criterion as checkSeams). Used for terrain textures.
struct WrapCheck {
    bool horizontal = true, vertical = true;
    double jumpX = 0, jumpY = 0, typicalX = 0, typicalY = 0;
};
WrapCheck checkWrap(const Image& img, const SeamOptions& opts = {});

// Procedural tileset from two seamless textures: the terrain drawn over the
// base with transitions shaped by the tile samples. Tiles that may touch share
// their edge samples and the edge noise repeats once per tile, so the
// generated set passes checkSeams by construction. Deterministic on every
// platform (integer hashing, no <random> distributions).
//
// tileSize 0 = one tile per texture; otherwise the textures are square
// multiples of it (1 to 4 tiles a side) and the set gets that many phases.
// A null base texture gives a transparent base: the set is a layer to draw
// over a lower terrain (see renderLevels).
struct TilesetStyle {
    double roundness = 1.0;      // 0 = straight/diagonal transitions, 1 = rounded
    double edgeNoise = 0.3;      // 0..0.9: how ragged the terrain edge is
    int noiseScale = 4;          // noise features per tile side
    bool outline = true;         // darker 1 px line along the terrain edge (inside the terrain)
    std::optional<Rgba8> outlineColor;   // default: the terrain texture's mean darkened
    std::uint32_t seed = 1;
};
Result<Tileset> generateTileset(AutotileScheme s, const Image& baseTexture, const Image& terrainTexture,
                                const TilesetStyle& style = {}, int tileSize = 0);

// Several terrains: a map of levels (0 = the base, 1..n = terrains) drawn as
// layers, each terrain over everything below it. levels[0] is the set for
// level 1 over the base; levels[k] the set for level k+1, normally with a
// transparent base. All sets must share scheme, tile size and phases. A cell
// of level k counts as terrain for every layer up to k, so the higher terrain
// always sits on the one below it, as in RPG Maker / Tiled terrain layers.
class LevelMap {
public:
    LevelMap() = default;
    LevelMap(int width, int height);
    int width() const noexcept { return m_w; }
    int height() const noexcept { return m_h; }
    int at(int x, int y) const noexcept;             // outside the map: 0
    void set(int x, int y, int level) noexcept;      // clamped to 0..255
    void fill(int level) noexcept;
    TerrainMap atLeast(int level) const;             // cells with at(x, y) >= level
    // Sets the square of cells within `radius` of (x, y) to `level`. With
    // `margins`, cells around it change too so that neighbouring cells never
    // differ by more than one level: grass painted on water gets a ring of
    // sand, and water dug into grass a ring of sand inside it.
    void paint(int x, int y, int level, int radius = 0, bool margins = true) noexcept;
    // Lowers cells until no two neighbours (8-way) differ by more than one
    // level: the margins rule for a whole map.
    void limitSteps() noexcept;
    int maxLevel() const noexcept;
    bool operator==(const LevelMap&) const = default;

private:
    int m_w = 0, m_h = 0;
    std::vector<std::uint8_t> m_cells;
};
Result<Image> renderLevels(const LevelMap& map, const std::vector<Tileset>& levels);

// Metadata for a set of levels: {"scheme", "tileSize", "phases", "base",
// "levels": [{"level", "image", "json"}]}.
std::string terrainSetJson(const std::vector<Tileset>& levels, const std::vector<std::string>& imageFiles,
                           const std::vector<std::string>& jsonFiles, const std::string& baseFile);

// Terrain coverage (0..1) at normalised tile coordinates (u, v) for a tile's
// samples; exposed for tests and previews.
double tileCoverage(const TileSamples& samples, double u, double v, double roundness);

// Metadata for a tileset image: {"scheme", "tileSize", "columns", "rows",
// "phases", "phaseOffset": {"x", "y"}, "image", "base", "tiles": [{"index", "x", "y", "mask" | "corners",
// "samples"}]}. Cardinal16 masks use N=1 E=2 S=4 W=8, Blob47 masks N=1 NE=2
// E=4 SE=8 S=16 SW=32 W=64 NW=128, Wang16 corners TL=1 TR=2 BR=4 BL=8.
std::string tilesetJson(const Tileset& t, const std::string& imageFile, const std::string& baseFile = {});

// A guide image for drawing a tileset by hand: every tile shows its terrain
// samples (terrain = `terrain`, base = `base`) as a 3x3 pattern with a grid.
Image tilesetTemplate(AutotileScheme s, int tileSize, Rgba8 terrain, Rgba8 base);

} // namespace acestudio
