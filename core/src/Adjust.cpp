#include "acestudio/core/Adjust.h"

#include <algorithm>
#include <cmath>

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

Rgba8 adjustPixel(Rgba8 c, const Adjustment& adj) noexcept {
    switch (adj.type) {
    case AdjustmentType::BrightnessContrast: {
        const double bright = adj.a * 2.55;
        const double C = adj.b * 2.55;
        const double f = (259.0 * (C + 255.0)) / (255.0 * (259.0 - C));
        auto ch = [&](std::uint8_t v) { return clamp8(f * (v + bright - 128.0) + 128.0); };
        return {ch(c.r), ch(c.g), ch(c.b), c.a};
    }
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
    case AdjustmentType::Invert:
        return {static_cast<std::uint8_t>(255 - c.r), static_cast<std::uint8_t>(255 - c.g),
                static_cast<std::uint8_t>(255 - c.b), c.a};
    case AdjustmentType::Posterize: {
        const int n = std::clamp(adj.a, 2, 64);
        auto ch = [n](std::uint8_t v) {
            const double step = std::round(v * (n - 1) / 255.0);
            return clamp8(step * 255.0 / (n - 1));
        };
        return {ch(c.r), ch(c.g), ch(c.b), c.a};
    }
    case AdjustmentType::Threshold: {
        const std::uint8_t v = c.luma() >= std::clamp(adj.a, 0, 255) ? 255 : 0;
        return {v, v, v, c.a};
    }
    }
    return c;
}

void applyAdjustment(Image& img, const Adjustment& adj) {
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) {
            const Rgba8 c = img.pixel(x, y);
            if (c.a == 0) continue;
            img.setPixel(x, y, adjustPixel(c, adj));
        }
}

} // namespace acestudio
