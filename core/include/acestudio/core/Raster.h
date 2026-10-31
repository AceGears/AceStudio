#pragma once

// Pure raster algorithms (no document, no undo). All results are exact
// integer pixel sets: no anti-aliasing, so pixel art stays crisp.

#include "acestudio/core/Image.h"

#include <cstdint>
#include <vector>

namespace acestudio {

struct Point {
    int x = 0, y = 0;
    constexpr bool operator==(const Point&) const = default;
};

// Bresenham line, inclusive of both ends, no duplicates.
std::vector<Point> linePoints(Point a, Point b);

// Cubic Bezier rasterised to a connected 8-neighbour pixel path from a to b,
// with no duplicate consecutive points.
std::vector<Point> curvePoints(Point a, Point b, Point c1, Point c2);

// Normalised rectangle spanning two corner points (inclusive).
IntRect rectFromCorners(Point a, Point b) noexcept;

std::vector<Point> rectOutlinePoints(const IntRect& r);

// Ellipse inscribed in `r`. Filled = every inside pixel; outline = inside
// pixels with at least one 4-neighbour outside. Symmetric and gap-free.
std::vector<Point> ellipsePoints(const IntRect& r, bool filled);

// Pixel-perfect filter: removes L-shaped corner pixels from a freehand
// stroke so 1px lines have no doubled corners. Operates incrementally:
// given the points so far, returns the index of a point to drop, or -1.
int pixelPerfectDropIndex(const std::vector<Point>& stroke) noexcept;

// Binary mask, one byte per pixel (0 = unselected).
class Mask {
public:
    Mask() = default;
    Mask(int width, int height, bool value = false);

    int width() const noexcept { return m_w; }
    int height() const noexcept { return m_h; }
    bool isNull() const noexcept { return m_w == 0 || m_h == 0; }
    bool get(int x, int y) const noexcept {
        return x >= 0 && y >= 0 && x < m_w && y < m_h && m_bits[idx(x, y)] != 0;
    }
    void set(int x, int y, bool v) noexcept {
        if (x >= 0 && y >= 0 && x < m_w && y < m_h) m_bits[idx(x, y)] = static_cast<std::uint8_t>(v);
    }
    void fillRect(const IntRect& r, bool v) noexcept;
    void invert() noexcept;
    IntRect bounds() const noexcept;     // empty if nothing set
    std::size_t count() const noexcept;

private:
    std::size_t idx(int x, int y) const noexcept {
        return static_cast<std::size_t>(y) * static_cast<std::size_t>(m_w) + static_cast<std::size_t>(x);
    }
    int m_w = 0, m_h = 0;
    std::vector<std::uint8_t> m_bits;
};

enum class SelectionOp { Replace, Add, Subtract, Intersect };
// Combines `src` into `dst` (same size required; Replace copies).
void combineMask(Mask& dst, const Mask& src, SelectionOp op);

// Pixels whose centres lie inside the polygon (even-odd rule). Closed
// automatically; the outline pixels themselves are always included so a
// lasso drawn along an edge selects that edge.
Mask polygonMask(int width, int height, const std::vector<Point>& polygon);

// 4×4 ordered-dither (Bayer) threshold in 0..15.
int bayer4(int x, int y) noexcept;

// Pixels matching the seed colour within `tolerance` (max per-channel
// difference, 0 = exact). Contiguous uses 4-connectivity; otherwise every
// matching pixel in the image. `limit` (optional) restricts the region.
Mask floodRegion(const Image& img, Point seed, bool contiguous, int tolerance, const Mask* limit = nullptr);

} // namespace acestudio
