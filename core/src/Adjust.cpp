#include "acestudio/core/Adjust.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace acestudio {

namespace {

std::uint8_t clamp8(double v) noexcept { return static_cast<std::uint8_t>(std::clamp(std::lround(v), 0L, 255L)); }

void rgbToHsl(double r, double g, double b, double& h, double& s, double& l) noexcept {
    const double mx = std::max({r, g, b}), mn = std::min({r, g, b});
    l = (mx + mn) / 2.0;
    if (mx == mn) { h = 0; s = 0; return; }
    const double d = mx - mn;
    s = l > 0.5 ? d / (2.0 - mx - mn) : d / (mx + mn);
    if (mx == r) h = (g - b) / d + (g < b ? 6.0 : 0.0);
    else if (mx == g) h = (b - r) / d + 2.0;
    else h = (r - g) / d + 4.0;
    h /= 6.0;
}

double hueToRgb(double p, double q, double t) noexcept {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0 / 6) return p + (q - p) * 6 * t;
    if (t < 1.0 / 2) return q;
    if (t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6;
    return p;
}

void hslToRgb(double h, double s, double l, double& r, double& g, double& b) noexcept {
    if (s <= 0) { r = g = b = l; return; }
    const double q = l < 0.5 ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
    r = hueToRgb(p, q, h + 1.0 / 3);
    g = hueToRgb(p, q, h);
    b = hueToRgb(p, q, h - 1.0 / 3);
}

} // namespace

std::array<std::uint8_t, 256> curveTable(const std::vector<CurvePoint>& pts) {
    std::array<std::uint8_t, 256> t{};
    for (int i = 0; i < 256; ++i) t[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i);
    const std::size_t n = pts.size();
    if (n < 2) return t;
    // Fritsch-Carlson monotone cubic: no overshoot between points.
    std::vector<double> x(n), y(n), m(n), d(n - 1);
    for (std::size_t i = 0; i < n; ++i) { x[i] = pts[i].x; y[i] = pts[i].y; }
    for (std::size_t i = 0; i + 1 < n; ++i) d[i] = (y[i + 1] - y[i]) / std::max(1e-9, x[i + 1] - x[i]);
    m[0] = d[0];
    m[n - 1] = d[n - 2];
    for (std::size_t i = 1; i + 1 < n; ++i) m[i] = d[i - 1] * d[i] <= 0 ? 0.0 : (d[i - 1] + d[i]) / 2.0;
    for (std::size_t i = 0; i + 1 < n; ++i) {
        if (d[i] == 0) { m[i] = m[i + 1] = 0; continue; }
        const double a = m[i] / d[i], b = m[i + 1] / d[i], h = a * a + b * b;
        if (h > 9.0) { const double s = 3.0 / std::sqrt(h); m[i] = s * a * d[i]; m[i + 1] = s * b * d[i]; }
    }
    for (int v = 0; v < 256; ++v) {
        double out;
        if (v <= x[0]) out = y[0];
        else if (v >= x[n - 1]) out = y[n - 1];
        else {
            std::size_t k = 0;
            while (k + 2 < n && v > x[k + 1]) ++k;
            const double h = x[k + 1] - x[k], s = (v - x[k]) / h;
            const double h00 = 2 * s * s * s - 3 * s * s + 1, h10 = s * s * s - 2 * s * s + s;
            const double h01 = -2 * s * s * s + 3 * s * s, h11 = s * s * s - s * s;
            out = h00 * y[k] + h10 * h * m[k] + h01 * y[k + 1] + h11 * h * m[k + 1];
        }
        t[static_cast<std::size_t>(v)] = clamp8(out);
    }
    return t;
}

namespace {
// Per-channel value mapping for LUT-able types.
std::uint8_t channelMap(std::uint8_t v, const Adjustment& adj) noexcept {
    switch (adj.type) {
    case AdjustmentType::BrightnessContrast: {
        const double bright = adj.a * 2.55, C = adj.b * 2.55;
        const double f = (259.0 * (C + 255.0)) / (255.0 * (259.0 - C));
        return clamp8(f * (v + bright - 128.0) + 128.0);
    }
    case AdjustmentType::Invert: return static_cast<std::uint8_t>(255 - v);
    case AdjustmentType::Posterize: {
        const int n = std::clamp(adj.a, 2, 64);
        return clamp8(std::round(v * (n - 1) / 255.0) * 255.0 / (n - 1));
    }
    case AdjustmentType::Levels: {
        const int lo = std::clamp(adj.a, 0, 254), hi = std::clamp(adj.b, lo + 1, 255);
        const double g = std::clamp(adj.c, 10, 999) / 100.0;
        const double t = std::clamp((v - lo) / double(hi - lo), 0.0, 1.0);
        return clamp8(std::pow(t, 1.0 / g) * 255.0);
    }
    default: return v;
    }
}
bool isPerChannel(AdjustmentType t) noexcept {
    return t != AdjustmentType::HueSaturation && t != AdjustmentType::Threshold;
}
} // namespace

PreparedAdjustment::PreparedAdjustment(const Adjustment& adj) : m_adj(adj), m_lut(isPerChannel(adj.type)) {
    if (!m_lut) return;
    if (adj.type == AdjustmentType::Curves) m_table = curveTable(adj.points);
    else for (int v = 0; v < 256; ++v) m_table[static_cast<std::size_t>(v)] = channelMap(static_cast<std::uint8_t>(v), adj);
}

Rgba8 PreparedAdjustment::apply(Rgba8 c) const noexcept {
    if (m_lut) return {m_table[c.r], m_table[c.g], m_table[c.b], c.a};
    return adjustPixel(c, m_adj);
}

Rgba8 adjustPixel(Rgba8 c, const Adjustment& adj) noexcept {
    if (isPerChannel(adj.type)) {
        if (adj.type == AdjustmentType::Curves) {
            const auto t = curveTable(adj.points);   // slow path; compositing uses PreparedAdjustment
            return {t[c.r], t[c.g], t[c.b], c.a};
        }
        return {channelMap(c.r, adj), channelMap(c.g, adj), channelMap(c.b, adj), c.a};
    }
    switch (adj.type) {
    case AdjustmentType::HueSaturation: {
        double h, s, l;
        rgbToHsl(c.r / 255.0, c.g / 255.0, c.b / 255.0, h, s, l);
        h = std::fmod(h + adj.a / 360.0 + 1.0, 1.0);
        s = std::clamp(s * (1.0 + adj.b / 100.0), 0.0, 1.0);
        double r, g, b;
        hslToRgb(h, s, l, r, g, b);
        const double t = adj.c / 100.0;
        auto light = [t](double v) { return t >= 0 ? v + (1.0 - v) * t : v * (1.0 + t); };
        return {clamp8(light(r) * 255.0), clamp8(light(g) * 255.0), clamp8(light(b) * 255.0), c.a};
    }
    case AdjustmentType::Threshold: {
        const std::uint8_t v = c.luma() >= std::clamp(adj.a, 0, 255) ? 255 : 0;
        return {v, v, v, c.a};
    }
    default: break;
    }
    return c;
}

void applyAdjustment(Image& img, const Adjustment& adj) {
    const PreparedAdjustment prep(adj);
    for (auto& p : img.pixels())
        if (p.a) p = prep.apply(p);
}

} // namespace acestudio
