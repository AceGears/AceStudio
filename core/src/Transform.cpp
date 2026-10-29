#include "acestudio/core/Transform.h"

#include <cmath>
#include <cstdlib>

namespace acestudio {

Image flipHorizontal(const Image& src) {
    Image out(src.width(), src.height());
    for (int y = 0; y < src.height(); ++y)
        for (int x = 0; x < src.width(); ++x) out.setPixel(src.width() - 1 - x, y, src.pixel(x, y));
    return out;
}

Image flipVertical(const Image& src) {
    Image out(src.width(), src.height());
    for (int y = 0; y < src.height(); ++y)
        for (int x = 0; x < src.width(); ++x) out.setPixel(x, src.height() - 1 - y, src.pixel(x, y));
    return out;
}

Image rotate90(const Image& src, bool clockwise) {
    Image out(src.height(), src.width());
    const int h = src.height(), w = src.width();
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (clockwise) out.setPixel(h - 1 - y, x, src.pixel(x, y));
            else out.setPixel(y, w - 1 - x, src.pixel(x, y));
        }
    return out;
}

Image rotate180(const Image& src) { return flipVertical(flipHorizontal(src)); }

Result<Image> scaleNearest(const Image& src, int width, int height) {
    auto out = Image::create(width, height);
    if (!out) return out.error();
    if (src.isNull()) return out;
    Image& o = out.value();
    for (int y = 0; y < height; ++y) {
        const int sy = static_cast<int>((static_cast<long long>(y) * 2 + 1) * src.height() / (2LL * height));
        for (int x = 0; x < width; ++x) {
            const int sx = static_cast<int>((static_cast<long long>(x) * 2 + 1) * src.width() / (2LL * width));
            o.setPixel(x, y, src.pixel(sx, sy));
        }
    }
    return out;
}

namespace {
bool similar(Rgba8 a, Rgba8 b, int t) {
    if (a.a == 0 && b.a == 0) return true;
    return std::abs(a.r - b.r) <= t && std::abs(a.g - b.g) <= t && std::abs(a.b - b.b) <= t &&
           std::abs(a.a - b.a) <= t;
}
} // namespace

Image scale2xSimilar(const Image& src, int threshold) {
    Image out(src.width() * 2, src.height() * 2);
    for (int y = 0; y < src.height(); ++y)
        for (int x = 0; x < src.width(); ++x) {
            // B above, D left, E centre, F right, H below (edges clamp to E).
            const Rgba8 E = src.pixel(x, y);
            const Rgba8 B = y > 0 ? src.pixel(x, y - 1) : E;
            const Rgba8 D = x > 0 ? src.pixel(x - 1, y) : E;
            const Rgba8 F = x + 1 < src.width() ? src.pixel(x + 1, y) : E;
            const Rgba8 H = y + 1 < src.height() ? src.pixel(x, y + 1) : E;
            Rgba8 e0 = E, e1 = E, e2 = E, e3 = E;
            if (!similar(B, H, threshold) && !similar(D, F, threshold)) {
                if (similar(D, B, threshold)) e0 = D;
                if (similar(B, F, threshold)) e1 = F;
                if (similar(D, H, threshold)) e2 = D;
                if (similar(H, F, threshold)) e3 = F;
            }
            out.setPixel(2 * x, 2 * y, e0);
            out.setPixel(2 * x + 1, 2 * y, e1);
            out.setPixel(2 * x, 2 * y + 1, e2);
            out.setPixel(2 * x + 1, 2 * y + 1, e3);
        }
    return out;
}

Image rotSprite(const Image& src, double degrees, int similarity) {
    if (src.isNull()) return src;
    double norm = std::fmod(degrees, 360.0);
    if (norm < 0) norm += 360.0;
    // Exact paths for right angles (and identity).
    if (std::abs(norm) < 1e-9) return src;
    if (std::abs(norm - 180.0) < 1e-9) return rotate180(src);
    if ((std::abs(norm - 90.0) < 1e-9 || std::abs(norm - 270.0) < 1e-9) && src.width() == src.height())
        return rotate90(src, std::abs(norm - 90.0) < 1e-9);

    const Image big = scale2xSimilar(scale2xSimilar(scale2xSimilar(src, similarity), similarity), similarity);
    const double rad = norm * 3.14159265358979323846 / 180.0;
    const double c = std::cos(rad), s = std::sin(rad);
    const double cx = src.width() / 2.0, cy = src.height() / 2.0;
    Image out(src.width(), src.height());
    for (int y = 0; y < src.height(); ++y)
        for (int x = 0; x < src.width(); ++x) {
            // Inverse-rotate the destination pixel centre into source space.
            const double dx = x + 0.5 - cx, dy = y + 0.5 - cy;
            const double sx = c * dx + s * dy + cx;
            const double sy = -s * dx + c * dy + cy;
            const int bx = static_cast<int>(std::floor(sx * 8.0));
            const int by = static_cast<int>(std::floor(sy * 8.0));
            if (bx < 0 || by < 0 || bx >= big.width() || by >= big.height()) continue;
            out.setPixel(x, y, big.pixel(bx, by));
        }
    return out;
}

Image outline(const Image& src, Rgba8 color, OutlineMode mode, bool diagonal) {
    Image out = src;
    auto opaque = [&](int x, int y) { return src.contains(x, y) && src.pixel(x, y).a > 0; };
    static constexpr int kN4[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    static constexpr int kN8[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
    const int n = diagonal ? 8 : 4;
    for (int y = 0; y < src.height(); ++y)
        for (int x = 0; x < src.width(); ++x) {
            const bool self = opaque(x, y);
            if ((mode == OutlineMode::Outside) == self) continue;
            bool touches = false;
            for (int i = 0; i < n && !touches; ++i) {
                const int nx = x + (diagonal ? kN8[i][0] : kN4[i][0]);
                const int ny = y + (diagonal ? kN8[i][1] : kN4[i][1]);
                // Inside mode: the canvas edge counts as "outside".
                touches = mode == OutlineMode::Outside ? opaque(nx, ny) : !opaque(nx, ny);
            }
            if (touches) out.setPixel(x, y, color);
        }
    return out;
}

} // namespace acestudio
