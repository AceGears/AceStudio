#pragma once

// Interactive painting. A PaintSession edits one cel live (so the canvas
// shows the stroke as it is drawn) and records a single undo step on
// commit containing only the changed rectangle.
//
// Painting rules applied to every pixel:
//   - only inside the active selection (if any)
//   - alpha lock: only where the layer already has pixels; alpha kept
//   - color mode: indexed snaps to the nearest opaque palette color,
//     grayscale converts to luma
//   - mirror: pixels are duplicated across the canvas centre lines
//   - tile mode: coordinates wrap around the canvas edges (seamless tiles)
//   - ink: Simple writes the colour exactly (pixel art stays exact), Blend
//     composites it over the existing pixel, Shading steps existing pixels
//     along a colour ramp
//   - mask target: painting edits the layer mask (luma of the colour; the
//     eraser hides) instead of the layer pixels

#include "acestudio/core/Editor.h"

#include <filesystem>
#include "acestudio/core/Raster.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace acestudio {

enum class BrushShape { Square, Circle, Custom };
enum class Ink { Simple, Blend, Shading };
enum class PaintTarget { Pixels, Mask };
enum class MirrorMode { None, Horizontal, Vertical, Both };   // Horizontal = mirror across the vertical centre line

struct BrushSettings {
    int size = 1;                       // 1..64 px
    BrushShape shape = BrushShape::Square;
    MirrorMode mirror = MirrorMode::None;
    // Ordered-dither density 1..16 (16 = solid). Pixels whose Bayer
    // threshold is >= density are skipped, giving a stable pattern that
    // tiles seamlessly across strokes.
    int ditherDensity = 16;
    Ink ink = Ink::Simple;
    // Custom brush tip (shape == Custom): pixels with alpha > 0 form the
    // footprint, centred on the cursor; with tipColors the tip's own colours
    // are stamped instead of the paint colour.
    std::shared_ptr<const Image> tip;
    bool tipColors = false;
};

// Brush tips saved as PNG files (any size up to 256x256).
Result<Image> loadBrushTip(const std::filesystem::path& file);
Status saveBrushTip(const Image& tip, const std::filesystem::path& file);
// A tip from the pixels of `img` inside `selection` (cropped to its bounds).
Result<Image> brushTipFromSelection(const Image& img, const Mask& selection);

class PaintSession {
public:
    // Fails if the layer is missing, a group, locked or hidden, or the
    // frame is out of range.
    static Result<std::unique_ptr<PaintSession>> begin(Editor& editor, LayerId layer, int frame, std::string label,
                                                       PaintTarget target = PaintTarget::Pixels);
    ~PaintSession();   // cancels if neither committed nor cancelled

    PaintSession(const PaintSession&) = delete;
    PaintSession& operator=(const PaintSession&) = delete;

    void setSelection(const Mask* selection) { m_selection = selection; }
    void setBrush(const BrushSettings& b) { m_brush = b; }
    const BrushSettings& brush() const noexcept { return m_brush; }
    // Shading ink: pixels whose colour is in `ramp` move `step` entries
    // along it (each pixel at most once per session).
    void setShading(std::vector<Rgba8> ramp, int step) { m_ramp = std::move(ramp); m_shadeStep = step; }
    PaintTarget target() const noexcept { return m_target; }
    // The brush footprint around (x, y), without mirroring.
    void footprintAt(int x, int y, std::vector<Point>& out) const { footprint(x, y, out); }

    // Paints/erases a brush footprint centred on (x, y), with mirroring.
    void stamp(int x, int y, Rgba8 color);
    void eraseStamp(int x, int y);
    // Writes one pixel (no brush, no mirror) with all constraints and the
    // ink applied.
    void writePixel(int x, int y, Rgba8 color);
    // Writes exactly (Simple ink) regardless of the brush ink.
    void writeExact(int x, int y, Rgba8 color);
    // Restores one pixel to its value at session start (pixel-perfect).
    void restorePixel(int x, int y);
    // Discards everything painted so far in this session (shape previews).
    void revertAll();

    // The live working image (canvas-sized) and the state at session start.
    const Image& pixels() const;
    const Image& original() const noexcept { return m_original; }
    IntRect touched() const noexcept { return m_touched; }
    // Area modified since the previous call (for incremental redraw).
    IntRect takeDirty() noexcept { const IntRect r = m_dirty; m_dirty = {}; return r; }
    LayerId layer() const noexcept { return m_layer; }
    int frame() const noexcept { return m_frame; }

    // Records one undo step if anything changed. Returns true if it did.
    bool commit();
    void cancel();

private:
    PaintSession(Editor& e, LayerId layer, int frame, std::string label, PaintTarget target);
    Image& work();
    const Cel* currentCel() const;
    void touch(int x, int y) noexcept;
    bool wrap(int& x, int& y) const noexcept;   // applies tile mode; false if outside the canvas
    Rgba8 constrain(Rgba8 c) const;
    void footprint(int cx, int cy, std::vector<Point>& out) const;
    void put(int x, int y, Rgba8 color, Ink ink);

    Editor& m_editor;
    LayerId m_layer;
    int m_frame;
    std::string m_label;
    std::optional<Cel> m_originalCel;   // exact cel before the session (nullopt = none)
    Image m_original;                   // canvas-sized pixels before the session
    IntRect m_touched;
    IntRect m_dirty;
    const Mask* m_selection = nullptr;
    BrushSettings m_brush;
    PaintTarget m_target = PaintTarget::Pixels;
    bool m_alphaLocked = false;
    bool m_finished = false;
    std::vector<Rgba8> m_ramp;
    int m_shadeStep = 1;
    Mask m_shaded;                      // pixels already shaded this session
};

// ---------------------------------------------------------------- tools

// Freehand pencil/eraser stroke with optional pixel-perfect corner removal
// (applies to 1px brushes).
class FreehandStroke {
public:
    FreehandStroke(PaintSession& s, Rgba8 color, bool erase, bool pixelPerfect);
    void moveTo(Point p);

private:
    void put(Point p);
    PaintSession& m_s;
    Rgba8 m_color;
    bool m_erase;
    bool m_pixelPerfect;
    std::vector<Point> m_path;
    std::optional<Point> m_lastRaw;
    bool m_started = false;
};

// Clone stamp: paints with pixels copied from `source` (a canvas-sized
// snapshot) at `offset` from the cursor (source = cursor - offset).
class CloneStroke {
public:
    CloneStroke(PaintSession& s, Image source, Point offset);
    void moveTo(Point p);

private:
    void put(Point p);
    PaintSession& m_s;
    Image m_source;
    Point m_offset;
    std::optional<Point> m_last;
};

void drawLine(PaintSession& s, Point a, Point b, Rgba8 color);
// Cubic Bezier from a to b with control points c1, c2.
void drawCurve(PaintSession& s, Point a, Point b, Point c1, Point c2, Rgba8 color);
// Closed polygon through `points` (outline with the brush, or filled).
void drawPolygon(PaintSession& s, const std::vector<Point>& points, bool filled, Rgba8 color);
void drawRectangle(PaintSession& s, Point a, Point b, bool filled, Rgba8 color);
void drawEllipse(PaintSession& s, Point a, Point b, bool filled, Rgba8 color);
enum class GradientShape { Linear, Radial };
enum class GradientDither { Smooth, Ordered };
// Fills the selection (or canvas) with a gradient from `c0` at `a` to `c1`
// at `b`. Ordered = only the two end colours, mixed with a Bayer pattern
// (classic pixel-art dithering); Smooth = per-pixel interpolation.
void drawGradient(PaintSession& s, Point a, Point b, Rgba8 c0, Rgba8 c1, GradientShape shape, GradientDither dither);

// Fills the region of the layer's own pixels around `seed`.
void fillRegion(PaintSession& s, Point seed, Rgba8 color, bool contiguous, int tolerance);

// Colour under (x, y): from the composited frame, or from one layer.
Rgba8 sampleColor(const Document& doc, int frame, Point p, LayerId layerOrNone = kNoLayer);

} // namespace acestudio
