#pragma once
// World Builder maps: tile layers painted from tile sheets, props placed in
// pixels, a collision grid, parallax backgrounds and point lights for a
// preview. Saved as JSON (.acemap) with the images beside it in
// "<name>.assets"; exported to Tiled (.tmx), which Godot, Unity and most
// engines import, plus a rendered PNG.

#include "acestudio/core/Image.h"
#include "acestudio/core/Result.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace acestudio::world {

struct TileSheet {
    std::string name;
    Image image;
    int tileW = 16, tileH = 16;
    // A terrain sheet from the Tileset Generator (its .json): "blob47" or
    // "cardinal16", laid out in that scheme's order, `terrainPhases` copies
    // per side. Empty for a plain sheet. The Terrain tool paints with it.
    std::string terrain;
    int terrainPhases = 1;
    int columns() const { return tileW > 0 ? image.width() / tileW : 0; }
    int rows() const { return tileH > 0 ? image.height() / tileH : 0; }
    int count() const { return columns() * rows(); }
    IntRect tileRect(int index) const;
};

// A cell holds -1 (empty) or makeCell(sheet, tile).
constexpr std::int32_t kEmpty = -1;
constexpr std::int32_t makeCell(int sheet, int tile) { return (sheet << 16) | (tile & 0xFFFF); }
constexpr int cellSheet(std::int32_t c) { return c >> 16; }
constexpr int cellTile(std::int32_t c) { return c & 0xFFFF; }

struct TileLayer {
    std::string name;
    std::vector<std::int32_t> cells;
    bool visible = true;
    int opacity = 255;
};

struct PropDef {
    std::string name;
    Image image;
};
struct Prop {
    int def = 0;
    int x = 0, y = 0;            // top-left, in map pixels
    bool solid = false;          // exported as a collision object
};

struct Parallax {
    std::string name;
    Image image;
    double factor = 0.5;         // 0 = fixed to the screen, 1 = moves with the map
    int y = 0;                   // vertical offset in map pixels
};

struct Light {
    int x = 0, y = 0;            // map pixels
    int radius = 64;
    Rgba8 color{255, 220, 160, 255};
};

class WorldMap {
public:
    static Result<WorldMap> create(int width, int height, int tileW, int tileH);

    int width() const { return m_width; }        // cells
    int height() const { return m_height; }
    int tileW() const { return m_tileW; }
    int tileH() const { return m_tileH; }
    int pixelWidth() const { return m_width * m_tileW; }
    int pixelHeight() const { return m_height * m_tileH; }

    std::vector<TileSheet> sheets;
    std::vector<TileLayer> layers;
    std::vector<PropDef> propDefs;
    std::vector<Prop> props;
    std::vector<Parallax> backgrounds;   // back to front
    std::vector<Light> lights;
    Rgba8 ambient{120, 120, 150, 255};   // light where no lamp reaches (lighting preview)

    TileLayer& addLayer(const std::string& name);
    Status addSheet(TileSheet sheet);    // tile size must fit the image
    std::int32_t cell(int layer, int x, int y) const;
    void setCell(int layer, int x, int y, std::int32_t value);
    void fillRect(int layer, const IntRect& cells, std::int32_t value);
    // 4-connected fill of the region of cells equal to the one at (x, y).
    void floodFill(int layer, int x, int y, std::int32_t value);
    bool solid(int x, int y) const;
    void setSolid(int x, int y, bool on);
    // Resizes, keeping the top-left content.
    void resize(int width, int height);
    // Marks cells solid wherever a layer has a tile (for a quick start).
    void solidFromLayer(int layer);
    // Terrain painting with a terrain sheet: sets or clears terrain at (x, y)
    // on the layer and re-picks the edge tiles of it and its 8 neighbours.
    // A cell is terrain when it holds a tile of that sheet. Outside the map
    // counts as the nearest edge cell, so terrain runs off the map edge
    // without a border. Errors when the sheet has no terrain scheme.
    Status paintTerrain(int layer, int sheet, int x, int y, bool on);

private:
    int m_width = 0, m_height = 0, m_tileW = 16, m_tileH = 16;
    std::vector<std::uint8_t> m_solid;
    friend Result<WorldMap> load(const std::filesystem::path&);
};

struct RenderOptions {
    bool props = true;
    bool collision = false;      // red overlay on solid cells
    bool lighting = false;
    bool grid = false;
    int cameraX = 0;             // parallax preview: left edge of the view, map pixels
    int viewWidth = 0;           // 0 = the whole map (no camera)
};
Image render(const WorldMap& map, const RenderOptions& opt = {});

// Solid cells merged into rectangles (greedy, row runs then down), in cells.
std::vector<IntRect> collisionRects(const WorldMap& map);

Status save(const WorldMap& map, const std::filesystem::path& file);
Result<WorldMap> load(const std::filesystem::path& file);

// Writes <name>.tmx with its tile sheets and prop images next to it, plus
// <name>.png (the rendered map). Returns the files written.
Result<std::vector<std::filesystem::path>> exportTiled(const WorldMap& map, const std::filesystem::path& tmxFile);

} // namespace acestudio::world
