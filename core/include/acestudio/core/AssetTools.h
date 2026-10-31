#pragma once

// Phase 6 asset tools: smart slicing of sprite sheets, nine-slice detection
// and rendering, a procedural UI kit with exact nine-slice metadata, and
// parallax background sets.

#include "acestudio/core/Atlas.h"
#include "acestudio/core/Image.h"
#include "acestudio/core/Result.h"

#include <optional>
#include <string>
#include <vector>

namespace acestudio {

// ---------------------------------------------------------------- slicing

struct SliceOptions {
    // Background: transparent pixels, or (when the sheet has no transparency)
    // the most common border colour, within `tolerance`.
    std::optional<Rgba8> background;     // force a colour key
    int tolerance = 8;
    int mergeDistance = 2;   // parts this close (px) belong to one sprite (a detached sword, a shadow)
    int minPixels = 4;       // smaller specks are noise
};

struct SlicedSprite {
    std::string name;        // sprite_000, sprite_001, ... in reading order
    IntRect rect;
};

// Sprites found by connected opaque regions, merged when close, in reading
// order (rows top to bottom, then left to right).
std::vector<SlicedSprite> detectSprites(const Image& sheet, const SliceOptions& opts = {});

// A regular grid guessed from the sheet: from fully empty separator rows and
// columns, or else from the spacing of the detected sprites.
struct GridGuess {
    int cellWidth = 0, cellHeight = 0;
    int offsetX = 0, offsetY = 0;
    int spacingX = 0, spacingY = 0;
    int columns = 0, rows = 0;
    bool found = false;
};
GridGuess detectGrid(const Image& sheet, const SliceOptions& opts = {});

// Cells of a grid, optionally skipping empty ones, in reading order.
std::vector<SlicedSprite> sliceGrid(const Image& sheet, const GridGuess& grid, bool skipEmpty = true,
                                    const SliceOptions& opts = {});

// Background colour the slicer uses for `sheet` (nullopt = transparency).
std::optional<Rgba8> sliceBackground(const Image& sheet, const SliceOptions& opts = {});

// JSON listing of slices (TexturePacker-style hash, so engines that read
// atlases can read slices of an existing sheet).
std::string slicesJson(const std::vector<SlicedSprite>& slices, const std::string& imageFile, int width, int height);

// ---------------------------------------------------------------- nine-slice

struct NineSlice {
    int left = 0, top = 0, right = 0, bottom = 0;   // fixed border widths (px)
    bool exact = false;    // true when the centre rows/columns are truly uniform
    bool operator==(const NineSlice&) const = default;
};

// Borders of an image meant to be stretched: the longest run of identical
// neighbouring columns (rows) is the stretchable middle. Falls back to a
// third of each side (exact = false) when no column repeats.
NineSlice detectNineSlice(const Image& img);

enum class NineSliceFill { Stretch, Tile };
// The image drawn at w x h with fixed corners and stretched (or tiled) edges
// and centre. Fails if w or h is smaller than the borders.
Result<Image> renderNineSlice(const Image& img, const NineSlice& n, int w, int h, NineSliceFill fill = NineSliceFill::Stretch);

// ---------------------------------------------------------------- UI kit

struct UiTheme {
    Rgba8 base{78, 92, 122, 255};
    Rgba8 light{128, 146, 180, 255};
    Rgba8 dark{44, 52, 72, 255};
    Rgba8 outline{20, 22, 32, 255};
    Rgba8 accent{232, 176, 64, 255};
    Rgba8 text{240, 240, 232, 255};
    int scale = 1;            // pixel size multiplier (1 = 1 px lines)
    bool rounded = true;      // cut the outer corner pixels
    Image texture;            // optional fill pattern for panels (tiled), may be null
};

struct UiElement {
    std::string name;         // e.g. button_normal
    Image image;
    NineSlice slice;          // exact by construction (zero for fixed-size elements)
    bool stretchable = true;
};

// Panel, inset panel, buttons (normal/hover/pressed/disabled), checkbox
// (off/on), radio (off/on), slider track and knob, progress bar (back/fill),
// tooltip, window frame with title bar, divider, scrollbar track and thumb.
std::vector<UiElement> generateUiKit(const UiTheme& theme);

// Atlas of the kit plus JSON: TexturePacker frames with a "nineSlice" object
// per element ({left, top, right, bottom}; Godot NinePatchRect patch margins
// use these directly, Unity sprite borders take them as L, B, R, T).
struct UiKitFiles {
    Atlas atlas;
    std::string json;
};
Result<UiKitFiles> packUiKit(const std::vector<UiElement>& kit, const std::string& imageFile, int padding = 1);

// ---------------------------------------------------------------- parallax

struct ParallaxLayer {
    std::string name;
    Image image;
    double scroll = 1.0;      // 0 = fixed (sky), 1 = moves with the camera
    int y = 0;                // vertical position in the view
    bool repeatX = true;
};

struct ParallaxSet {
    int viewWidth = 320, viewHeight = 180;
    std::vector<ParallaxLayer> layers;    // back to front
};

// Problems that would show in game: layers that repeat but do not wrap
// seamlessly, layers narrower than the view that do not repeat, layers outside
// the view.
// Rows in the longest clear step where the right edge wraps into the left
// (and, in `typical`, the most it changes between neighbouring columns:
// max(1.5 x 95th percentile, largest)).
int wrapStepRows(const Image& img, int* typical = nullptr);
std::vector<std::string> checkParallax(const ParallaxSet& set);

// The view at camera position `cameraX` (layers composited back to front).
Image renderParallax(const ParallaxSet& set, double cameraX);

// Makes an image wrap horizontally by cross-fading its last `overlap`
// columns into the first ones (with ordered dithering, so pixel art keeps
// hard pixels). The result is `overlap` px narrower.
Result<Image> makeSeamlessX(const Image& img, int overlap);

// Layer files and scroll factors: {"view": {w, h}, "layers": [{name, file,
// scroll, y, repeat}]}.
std::string parallaxJson(const ParallaxSet& set, const std::vector<std::string>& layerFiles);

} // namespace acestudio
