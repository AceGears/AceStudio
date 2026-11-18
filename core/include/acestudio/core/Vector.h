#pragma once
// Vector shapes on a cel (schema 5; ADR-ANIM-003 phase 2).
//
// A cel can carry editable shapes: paths with Bezier anchors, rectangles and
// ellipses, each with a stroke (width, solid, dashed or dotted) and a fill
// (solid, or a linear or radial gradient, ordered-dithered or smooth). The
// shapes are rasterised pixel-perfectly into the cel's own pixels (no
// anti-aliasing, whole pixels, the colours given and nothing between them
// when dithered), so compositing, export and every raster tool see an
// ordinary cel. Editing the shapes redraws the cel; painting on it by hand
// is kept only until the shapes are edited again (rasterise to keep it).

#include "acestudio/core/Color.h"
#include "acestudio/core/Image.h"
#include "acestudio/core/Raster.h"
#include "acestudio/core/Result.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace acestudio {

// An anchor and its Bezier handles, as offsets from the anchor (canvas
// pixels). Both handles (0, 0) = a corner: the segments on either side are
// straight there.
struct VectorPoint {
    double x = 0, y = 0;
    double inX = 0, inY = 0;     // the handle towards the previous anchor
    double outX = 0, outY = 0;   // the handle towards the next anchor
    bool operator==(const VectorPoint&) const = default;
};

enum class ShapeKind { Path, Rectangle, Ellipse };
enum class StrokeStyle { None, Solid, Dashed, Dotted };
enum class FillStyle { None, Solid, Linear, Radial };

struct VectorShape {
    ShapeKind kind = ShapeKind::Path;
    // Path: the anchors, in order. Rectangle and ellipse: two opposite
    // corners of the box (inclusive pixels).
    std::vector<VectorPoint> points;
    bool closed = false;            // paths: the last anchor joins the first

    StrokeStyle stroke = StrokeStyle::Solid;
    Rgba8 strokeColor = Rgba8::opaque(0, 0, 0);
    int width = 1;                  // square pen, pixels
    int dash = 4, gap = 2;          // dashed: pixels on, pixels off along the outline

    FillStyle fill = FillStyle::None;   // a path is filled as if closed
    Rgba8 fillColor = Rgba8::opaque(255, 255, 255);
    Rgba8 fillColor2 = Rgba8::opaque(0, 0, 0);   // gradients: the colour at `to`
    bool dither = true;             // gradients: ordered (Bayer) dithering, else smooth
    double fromX = 0, fromY = 0, toX = 0, toY = 0;   // gradient axis (radial: centre, edge)

    bool operator==(const VectorShape&) const = default;
};

namespace vectors {

inline constexpr int kMaxShapes = 1000;     // per cel
inline constexpr int kMaxPoints = 4096;     // per shape
inline constexpr int kMaxWidth = 32;
inline constexpr double kMaxCoord = 1 << 20;

// Numbers finite and in range, enough points for the kind, width 1..32,
// dash and gap 1..256.
Status validate(const std::vector<VectorShape>& shapes);

// The shapes drawn in order (later ones on top) into a transparent
// width x height image: fills first, then each shape's stroke over its own
// fill. Pixels are replaced, not blended.
Image rasterize(const std::vector<VectorShape>& shapes, int width, int height);

// The outline of one shape as a connected pixel path, in drawing order
// (closed shapes end next to where they start). Exposed for tests and the
// canvas overlay.
std::vector<Point> outline(const VectorShape& shape);

nlohmann::json toJson(const std::vector<VectorShape>& shapes);
Result<std::vector<VectorShape>> fromJson(const nlohmann::json& j);

const char* toString(ShapeKind k);
const char* toString(StrokeStyle s);
const char* toString(FillStyle f);
std::optional<ShapeKind> shapeKindFromString(const std::string& s);
std::optional<StrokeStyle> strokeStyleFromString(const std::string& s);
std::optional<FillStyle> fillStyleFromString(const std::string& s);

} // namespace vectors

} // namespace acestudio
