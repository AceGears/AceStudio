#include "acestudio/core/Raster.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace acestudio {

std::vector<Point> linePoints(Point a, Point b) {
    std::vector<Point> pts;
    const int dx = std::abs(b.x - a.x), sx = a.x < b.x ? 1 : -1;
    const int dy = -std::abs(b.y - a.y), sy = a.y < b.y ? 1 : -1;
    pts.reserve(static_cast<std::size_t>(std::max(dx, -dy)) + 1);
    int err = dx + dy;
    Point p = a;
    for (;;) {
        pts.push_back(p);
        if (p == b) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; p.x += sx; }
        if (e2 <= dx) { err += dx; p.y += sy; }
    }
    return pts;
}

IntRect rectFromCorners(Point a, Point b) noexcept {
    const int x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
    return {x0, y0, std::abs(b.x - a.x) + 1, std::abs(b.y - a.y) + 1};
}

std::vector<Point> rectOutlinePoints(const IntRect& r) {
    std::vector<Point> pts;
    if (r.empty()) return pts;
    const int x1 = r.x + r.w - 1, y1 = r.y + r.h - 1;
    for (int x = r.x; x <= x1; ++x) {
        pts.push_back({x, r.y});
        if (y1 != r.y) pts.push_back({x, y1});
    }
    for (int y = r.y + 1; y < y1; ++y) {
        pts.push_back({r.x, y});
        if (x1 != r.x) pts.push_back({x1, y});
    }
    return pts;
}

std::vector<Point> ellipsePoints(const IntRect& r, bool filled) {
    std::vector<Point> pts;
    if (r.empty()) return pts;
    // Inside test in doubled integer-ish space so the shape is exactly
    // symmetric for both odd and even sizes.
    const double cx = r.x + (r.w - 1) / 2.0, cy = r.y + (r.h - 1) / 2.0;
    const double rx = r.w / 2.0, ry = r.h / 2.0;
    auto inside = [&](int x, int y) {
        if (x < r.x || y < r.y || x >= r.x + r.w || y >= r.y + r.h) return false;
        const double nx = (x - cx) / rx, ny = (y - cy) / ry;
        return nx * nx + ny * ny <= 1.0 + 1e-9;
    };
    for (int y = r.y; y < r.y + r.h; ++y)
        for (int x = r.x; x < r.x + r.w; ++x) {
            if (!inside(x, y)) continue;
            if (filled || !inside(x - 1, y) || !inside(x + 1, y) || !inside(x, y - 1) || !inside(x, y + 1))
                pts.push_back({x, y});
        }
    // Tiny ellipses can end up empty with this test; fall back to the rect.
    if (pts.empty()) return filled ? std::vector<Point>{} : rectOutlinePoints(r);
    return pts;
}

int pixelPerfectDropIndex(const std::vector<Point>& s) noexcept {
    const std::size_t n = s.size();
    if (n < 3) return -1;
    const Point a = s[n - 3], b = s[n - 2], c = s[n - 1];
    // b is an L-corner if a and c are diagonal neighbours and b touches
    // each of them orthogonally.
    const bool diagonal = std::abs(a.x - c.x) == 1 && std::abs(a.y - c.y) == 1;
    const bool orthoAB = std::abs(a.x - b.x) + std::abs(a.y - b.y) == 1;
    const bool orthoBC = std::abs(b.x - c.x) + std::abs(b.y - c.y) == 1;
    return diagonal && orthoAB && orthoBC ? static_cast<int>(n - 2) : -1;
}

// ------------------------------------------------------------------ Mask

Mask::Mask(int width, int height, bool value) {
    if (width <= 0 || height <= 0) return;
    m_w = width;
    m_h = height;
    m_bits.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), value ? 1 : 0);
}

void Mask::fillRect(const IntRect& r, bool v) noexcept {
    const IntRect c = r.intersected({0, 0, m_w, m_h});
    for (int y = c.y; y < c.y + c.h; ++y)
        std::fill_n(m_bits.begin() + static_cast<std::ptrdiff_t>(idx(c.x, y)), c.w, v ? 1 : 0);
}

void Mask::invert() noexcept {
    for (auto& b : m_bits) b = b ? 0 : 1;
}

IntRect Mask::bounds() const noexcept {
    int minX = m_w, minY = m_h, maxX = -1, maxY = -1;
    for (int y = 0; y < m_h; ++y)
        for (int x = 0; x < m_w; ++x)
            if (m_bits[idx(x, y)]) {
                minX = std::min(minX, x); maxX = std::max(maxX, x);
                minY = std::min(minY, y); maxY = std::max(maxY, y);
            }
    if (maxX < 0) return {};
    return {minX, minY, maxX - minX + 1, maxY - minY + 1};
}

std::size_t Mask::count() const noexcept {
    return static_cast<std::size_t>(std::count(m_bits.begin(), m_bits.end(), std::uint8_t{1}));
}

// ------------------------------------------------------------------ flood

Mask floodRegion(const Image& img, Point seed, bool contiguous, int tolerance, const Mask* limit) {
    Mask out(img.width(), img.height());
    if (!img.contains(seed.x, seed.y)) return out;
    if (limit && !limit->isNull() && !limit->get(seed.x, seed.y)) return out;
    const Rgba8 target = img.pixel(seed.x, seed.y);
    tolerance = std::clamp(tolerance, 0, 255);
    auto matches = [&](int x, int y) {
        if (limit && !limit->isNull() && !limit->get(x, y)) return false;
        const Rgba8 p = img.pixel(x, y);
        // Fully transparent pixels match each other regardless of RGB.
        if (p.a == 0 && target.a == 0) return true;
        return std::abs(p.r - target.r) <= tolerance && std::abs(p.g - target.g) <= tolerance &&
               std::abs(p.b - target.b) <= tolerance && std::abs(p.a - target.a) <= tolerance;
    };

    if (!contiguous) {
        for (int y = 0; y < img.height(); ++y)
            for (int x = 0; x < img.width(); ++x)
                if (matches(x, y)) out.set(x, y, true);
        return out;
    }

    // Scanline flood fill with an explicit stack (no recursion depth risk).
    std::vector<Point> stack{seed};
    while (!stack.empty()) {
        Point p = stack.back();
        stack.pop_back();
        if (out.get(p.x, p.y) || !matches(p.x, p.y)) continue;
        int x0 = p.x, x1 = p.x;
        while (x0 - 1 >= 0 && !out.get(x0 - 1, p.y) && matches(x0 - 1, p.y)) --x0;
        while (x1 + 1 < img.width() && !out.get(x1 + 1, p.y) && matches(x1 + 1, p.y)) ++x1;
        for (int x = x0; x <= x1; ++x) {
            out.set(x, p.y, true);
            for (int ny : {p.y - 1, p.y + 1})
                if (ny >= 0 && ny < img.height() && !out.get(x, ny) && matches(x, ny)) stack.push_back({x, ny});
        }
    }
    return out;
}

} // namespace acestudio
