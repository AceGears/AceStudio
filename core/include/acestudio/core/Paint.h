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
//   - pencil writes the color exactly (no blending), so pixel art stays exact

#include "acestudio/core/Editor.h"
#include "acestudio/core/Raster.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace acestudio {

enum class BrushShape { Square, Circle };
enum class MirrorMode { None, Horizontal, Vertical, Both };   // Horizontal = mirror across the vertical centre line

struct BrushSettings {
    int size = 1;                       // 1..64 px
    BrushShape shape = BrushShape::Square;
    MirrorMode mirror = MirrorMode::None;
};

class PaintSession {
public:
    // Fails if the layer is missing, a group, locked or hidden, or the
    // frame is out of range.
    static Result<std::unique_ptr<PaintSession>> begin(Editor& editor, LayerId layer, int frame, std::string label);
    ~PaintSession();   // cancels if neither committed nor cancelled

    PaintSession(const PaintSession&) = delete;
    PaintSession& operator=(const PaintSession&) = delete;

    void setSelection(const Mask* selection) { m_selection = selection; }
    void setBrush(const BrushSettings& b) { m_brush = b; }
    const BrushSettings& brush() const noexcept { return m_brush; }

    // Paints/erases a brush footprint centred on (x, y), with mirroring.
    void stamp(int x, int y, Rgba8 color);
    void eraseStamp(int x, int y);
    // Writes one pixel (no brush, no mirror) with all constraints applied.
    void writePixel(int x, int y, Rgba8 color);
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
    PaintSession(Editor& e, LayerId layer, int frame, std::string label);
    Image& work();
    void touch(int x, int y) noexcept;
    Rgba8 constrain(Rgba8 c) const;
    void footprint(int cx, int cy, std::vector<Point>& out) const;

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
    bool m_alphaLocked = false;
    bool m_finished = false;
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

void drawLine(PaintSession& s, Point a, Point b, Rgba8 color);
void drawRectangle(PaintSession& s, Point a, Point b, bool filled, Rgba8 color);
void drawEllipse(PaintSession& s, Point a, Point b, bool filled, Rgba8 color);
// Fills the region of the layer's own pixels around `seed`.
void fillRegion(PaintSession& s, Point seed, Rgba8 color, bool contiguous, int tolerance);

// Colour under (x, y): from the composited frame, or from one layer.
Rgba8 sampleColor(const Document& doc, int frame, Point p, LayerId layerOrNone = kNoLayer);

} // namespace acestudio
