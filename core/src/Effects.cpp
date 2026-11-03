#include "acestudio/core/Effects.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace acestudio::fx {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTau = 2 * kPi;

struct Hsv { double h, s, v; };   // h in degrees

Hsv hsv(const Rgba8& c) {
    const double r = c.r / 255.0, g = c.g / 255.0, b = c.b / 255.0;
    const double mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
    double h = 0;
    if (d > 1e-9) {
        if (mx == r) h = 60 * std::fmod((g - b) / d, 6.0);
        else if (mx == g) h = 60 * ((b - r) / d + 2);
        else h = 60 * ((r - g) / d + 4);
    }
    if (h < 0) h += 360;
    return {h, mx > 0 ? d / mx : 0, mx};
}

double luma(const Rgba8& c) { return 0.299 * c.r + 0.587 * c.g + 0.114 * c.b; }

// Small deterministic hash -> [0, 1).
double hash01(unsigned a, unsigned b, unsigned seed) {
    std::uint32_t h = a * 374761393u + b * 668265263u + seed * 2147483647u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (h & 0xFFFFFF) / static_cast<double>(0x1000000);
}

// Smooth noise that is periodic in t (t in [0, 1) loops): a few travelling
// sine waves with integer time frequencies and seeded directions.
struct LoopNoise {
    struct Wave { double fx, fy, ft, phase, amp; };
    std::array<Wave, 4> w;
    LoopNoise(unsigned seed, double scaleX, double scaleY) {
        for (unsigned i = 0; i < w.size(); ++i) {
            const double a = hash01(i, 1, seed), b = hash01(i, 2, seed), c = hash01(i, 3, seed);
            w[i] = {scaleX * (0.6 + 1.2 * a), scaleY * (0.6 + 1.2 * b), static_cast<double>(1 + (i % 3)), kTau * c,
                    1.0 / (1 + i)};
        }
    }
    // About -1..1.
    double operator()(double x, double y, double t) const {
        double s = 0, norm = 0;
        for (const Wave& v : w) {
            s += v.amp * std::sin(kTau * (v.fx * x + v.fy * y) + kTau * v.ft * t + v.phase);
            norm += v.amp;
        }
        return s / norm;
    }
};

IntRect maskBounds(const Mask& m) {
    int x0 = m.width(), y0 = m.height(), x1 = -1, y1 = -1;
    for (int y = 0; y < m.height(); ++y)
        for (int x = 0; x < m.width(); ++x)
            if (m.get(x, y)) { x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y); }
    return x1 < 0 ? IntRect{} : IntRect{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
}

// The region's colours ordered dark to bright (a ramp for flicker and shimmer).
std::vector<Rgba8> ramp(const Image& img, const Mask& m) {
    std::vector<Rgba8> cols;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x)
            if (m.get(x, y)) {
                const Rgba8 c = img.pixel(x, y);
                if (std::find(cols.begin(), cols.end(), c) == cols.end()) cols.push_back(c);
            }
    std::sort(cols.begin(), cols.end(), [](const Rgba8& a, const Rgba8& b) { return luma(a) < luma(b); });
    return cols;
}

Rgba8 shifted(const std::vector<Rgba8>& r, const Rgba8& c, int by) {
    if (r.size() < 2 || by == 0) return c;
    const auto it = std::find(r.begin(), r.end(), c);
    if (it == r.end()) return c;
    const int i = std::clamp(static_cast<int>(it - r.begin()) + by, 0, static_cast<int>(r.size()) - 1);
    return r[static_cast<std::size_t>(i)];
}

EffectClip fire(const Image& src, const Mask& region, const EffectOptions& o) {
    EffectClip out;
    out.region = region;
    const IntRect fb = maskBounds(region);
    const int W = src.width(), H = src.height();
    const auto r = ramp(src, region);
    const LoopNoise sway(o.seed, 1.0 / std::max(4, fb.w), 1.0 / std::max(4, fb.h));
    const LoopNoise lick(o.seed + 7, 2.0 / std::max(4, fb.w), 0.5 / std::max(4, fb.h));
    const LoopNoise glow(o.seed + 13, 3.0 / std::max(4, fb.w), 3.0 / std::max(4, fb.h));
    const double base = fb.y + fb.h;                       // the bottom of the flames
    const double flameH = std::max(2, fb.h);
    // Embers: a few of the brightest pixels rising above the flames.
    const int embers = fb.empty() ? 0 : std::clamp(fb.w / 6, 1, 6);
    for (int f = 0; f < o.frames; ++f) {
        const double t = static_cast<double>(f) / o.frames;
        Image img = src;
        if (!fb.empty()) {
            // Search a little above the flames too: tips lick upwards.
            const int top = std::max(0, fb.y - static_cast<int>(std::ceil(flameH * 0.35 * o.amount)) - 1);
            const int x0 = std::max(0, fb.x - 2), x1 = std::min(W - 1, fb.x + fb.w + 1);
            for (int y = top; y < fb.y + fb.h; ++y)
                for (int x = x0; x <= x1; ++x) {
                    const bool flame = region.get(x, y);
                    const Rgba8 here = src.pixel(x, y);
                    if (!flame && here.a > 0) continue;    // logs, stones: never painted over
                    const double u = std::clamp((base - y) / flameH, 0.0, 1.4);   // 0 at the base, 1 at the tip
                    // Sample from lower down (flames stretch up) and sideways, more at the tips.
                    const double dy = flameH * 0.22 * o.amount * u * (0.55 + 0.45 * lick(x, y, t));
                    const double dx = 1.6 * o.amount * u * sway(x, y, t);
                    const int sx = static_cast<int>(std::lround(x - dx)), sy = static_cast<int>(std::lround(y + dy));
                    Rgba8 c{0, 0, 0, 0};
                    if (sx >= 0 && sy >= 0 && sx < W && sy < H && region.get(sx, sy)) {
                        c = src.pixel(sx, sy);
                        // Flicker: shift within the flame's own colours.
                        c = shifted(r, c, static_cast<int>(std::lround(1.2 * o.amount * glow(x, y, t))));
                    }
                    // A miss in the body of the flame keeps the pixel (no holes);
                    // only the upper flame thins out and flickers at its edge.
                    if (!c.a && flame && u < 0.6) c = shifted(r, here, static_cast<int>(std::lround(glow(x, y, t))));
                    img.setPixel(x, y, c.a ? c : (flame ? Rgba8{0, 0, 0, 0} : here));
                }
            for (int e = 0; e < embers && !r.empty(); ++e) {
                const double ex = fb.x + fb.w * (0.2 + 0.6 * hash01(static_cast<unsigned>(e), 5, o.seed));
                const double rise = std::fmod(t + hash01(static_cast<unsigned>(e), 6, o.seed), 1.0);
                const int px = static_cast<int>(std::lround(ex + 1.5 * std::sin(kTau * (rise + e * 0.3))));
                const int py = static_cast<int>(std::lround(fb.y - rise * flameH * 0.8));
                if (rise < 0.8 && px >= 0 && py >= 0 && px < W && py < H && src.pixel(px, py).a == 0) img.setPixel(px, py, r.back());
            }
        }
        out.frames.push_back(std::move(img));
        out.durations.push_back(o.frameMs);
    }
    return out;
}

EffectClip water(const Image& src, const Mask& region, const EffectOptions& o) {
    EffectClip out;
    out.region = region;
    const auto r = ramp(src, region);
    const LoopNoise shimmer(o.seed, 0.08, 0.15);
    for (int f = 0; f < o.frames; ++f) {
        const double t = static_cast<double>(f) / o.frames;
        Image img = src;
        for (int y = 0; y < src.height(); ++y) {
            // The row's water pixels in order; each takes the colour of one
            // further along, so the pattern flows and wraps once per loop.
            std::vector<int> xs;
            for (int x = 0; x < src.width(); ++x)
                if (region.get(x, y)) xs.push_back(x);
            if (xs.size() < 2) continue;
            const int n = static_cast<int>(xs.size());
            // Rows flow at slightly different whole-loop speeds for depth.
            const int laps = 1 + (y % 3 == 0 ? 1 : 0);
            const int shift = static_cast<int>(std::lround(t * n * laps * std::clamp(o.amount, 0.25, 2.0))) % n;
            for (int i = 0; i < n; ++i) {
                Rgba8 c = src.pixel(xs[static_cast<std::size_t>((i + shift) % n)], y);
                c = shifted(r, c, static_cast<int>(std::lround(0.8 * o.amount * shimmer(xs[static_cast<std::size_t>(i)], y, t))));
                img.setPixel(xs[static_cast<std::size_t>(i)], y, c);
            }
        }
        out.frames.push_back(std::move(img));
        out.durations.push_back(o.frameMs);
    }
    return out;
}

// Backward-mapped displacement of the region; the rest stays.
template <typename Disp>
Image displaced(const Image& src, const Mask& region, const IntRect& area, Disp disp) {
    Image img = src;
    // Clear the moving part, then draw it displaced.
    for (int y = 0; y < src.height(); ++y)
        for (int x = 0; x < src.width(); ++x)
            if (region.get(x, y)) img.setPixel(x, y, {0, 0, 0, 0});
    for (int y = std::max(0, area.y); y < std::min(src.height(), area.y + area.h); ++y)
        for (int x = std::max(0, area.x); x < std::min(src.width(), area.x + area.w); ++x) {
            const auto [dx, dy] = disp(x, y);
            const int sx = static_cast<int>(std::lround(x - dx)), sy = static_cast<int>(std::lround(y - dy));
            if (sx < 0 || sy < 0 || sx >= src.width() || sy >= src.height() || !region.get(sx, sy)) continue;
            const Rgba8 c = src.pixel(sx, sy);
            if (c.a) img.setPixel(x, y, c);
        }
    return img;
}

EffectClip sway(const Image& src, const Mask& region, const EffectOptions& o) {
    EffectClip out;
    out.region = region;
    const IntRect b = maskBounds(region);
    const double pivot = b.y + b.h;   // the region bends from its bottom
    const double h = std::max(1, b.h);
    const double amp = std::max(1.0, src.width() * 0.035) * o.amount;
    const LoopNoise flutter(o.seed, 0.25, 0.25);
    const IntRect area{b.x - static_cast<int>(amp) - 2, b.y - 1, b.w + 2 * static_cast<int>(amp) + 4, b.h + 2};
    for (int f = 0; f < o.frames; ++f) {
        const double t = static_cast<double>(f) / o.frames;
        out.frames.push_back(displaced(src, region, area, [&](int x, int y) {
            const double u = std::clamp((pivot - y) / h, 0.0, 1.0);
            // The top lags the middle a little: a bend, not a rigid tilt.
            const double dx = amp * std::pow(u, 1.5) * std::sin(kTau * t - 0.6 * u) + 0.6 * o.amount * u * flutter(x, y, t);
            return std::pair<double, double>{dx, 0.0};
        }));
        out.durations.push_back(o.frameMs);
    }
    return out;
}

EffectClip flag(const Image& src, const Mask& region, const EffectOptions& o) {
    EffectClip out;
    out.region = region;
    const IntRect b = maskBounds(region);
    const double w = std::max(1, b.w);
    const double amp = std::max(1.0, b.h * 0.08) * o.amount;
    const IntRect area{b.x, b.y - static_cast<int>(amp) - 2, b.w, b.h + 2 * static_cast<int>(amp) + 4};
    for (int f = 0; f < o.frames; ++f) {
        const double t = static_cast<double>(f) / o.frames;
        out.frames.push_back(displaced(src, region, area, [&](int x, int) {
            // Fixed at the pole (left edge), a wave travelling to the free end.
            const double u = std::clamp((x - b.x) / w, 0.0, 1.0);
            return std::pair<double, double>{0.0, amp * u * std::sin(kTau * (t - 1.3 * u))};
        }));
        out.durations.push_back(o.frameMs);
    }
    return out;
}

EffectClip smoke(const Image& src, const Mask& region, const EffectOptions& o) {
    EffectClip out;
    out.region = region;
    const IntRect b = maskBounds(region);
    const double h = std::max(2, b.h);
    const LoopNoise wobble(o.seed, 0.1, 0.12);
    const IntRect area{b.x - 4, b.y - 1, b.w + 8, b.h + 2};
    for (int f = 0; f < o.frames; ++f) {
        const double t = static_cast<double>(f) / o.frames;
        out.frames.push_back(displaced(src, region, area, [&](int x, int y) {
            // Rises one region height per loop (wrapping), wobbling more as it climbs.
            const double u = std::clamp((b.y + b.h - y) / h, 0.0, 1.0);
            // The pixel shown at y comes from `shift` rows lower; what rises
            // past the top comes back in at the bottom (one lap per loop).
            const double shift = std::fmod(t * h, h);
            double sy = y + shift;
            if (sy >= b.y + b.h) sy -= h;
            return std::pair<double, double>{2.0 * o.amount * u * wobble(x, y, t), y - sy};
        }));
        out.durations.push_back(o.frameMs);
    }
    return out;
}

EffectClip pulse(const Image& src, const Mask& region, const EffectOptions& o) {
    EffectClip out;
    out.region = region;
    const IntRect b = maskBounds(region);
    const auto r = ramp(src, region);
    const double cx = b.x + b.w / 2.0, cy = b.y + b.h / 2.0;
    const IntRect area{b.x - b.w / 6 - 2, b.y - b.h / 6 - 2, b.w + b.w / 3 + 4, b.h + b.h / 3 + 4};
    for (int f = 0; f < o.frames; ++f) {
        const double t = static_cast<double>(f) / o.frames;
        const double s = 1 + 0.08 * o.amount * std::sin(kTau * t);
        Image img = displaced(src, region, area, [&](int x, int y) {
            return std::pair<double, double>{(x - cx) * (1 - 1 / s), (y - cy) * (1 - 1 / s)};
        });
        // Glow: the region's colours step brighter at the peak of the pulse.
        const int by = static_cast<int>(std::lround(1.5 * o.amount * std::max(0.0, std::sin(kTau * t))));
        if (by)
            for (int y = 0; y < img.height(); ++y)
                for (int x = 0; x < img.width(); ++x)
                    if (img.pixel(x, y).a) img.setPixel(x, y, shifted(r, img.pixel(x, y), by));
        out.frames.push_back(std::move(img));
        out.durations.push_back(o.frameMs);
    }
    return out;
}

} // namespace

const char* toString(Effect e) noexcept {
    switch (e) {
    case Effect::Fire: return "fire";
    case Effect::Water: return "water";
    case Effect::Sway: return "sway";
    case Effect::Flag: return "flag";
    case Effect::Smoke: return "smoke";
    case Effect::Pulse: return "pulse";
    }
    return "fire";
}

std::optional<Effect> effectFromString(std::string_view s) noexcept {
    for (Effect e : {Effect::Fire, Effect::Water, Effect::Sway, Effect::Flag, Effect::Smoke, Effect::Pulse})
        if (s == toString(e)) return e;
    return std::nullopt;
}

Mask autoRegion(const Image& img, Effect e) {
    Mask m(img.width(), img.height());
    const IntRect bb = img.opaqueBounds();
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) {
            const Rgba8 c = img.pixel(x, y);
            if (c.a == 0) continue;
            const Hsv v = hsv(c);
            bool on = false;
            switch (e) {
            case Effect::Fire:   // warm and bright, or the pale hot core of the flame
                on = ((v.h <= 62 || v.h >= 345) && v.s >= 0.35 && v.v >= 0.55) ||
                     (v.v >= 0.9 && v.s <= 0.35 && c.r >= c.b && c.g >= c.b);
                break;
            case Effect::Water: on = v.h >= 165 && v.h <= 250 && v.s >= 0.2; break;                 // blue and cyan
            case Effect::Smoke: on = v.s <= 0.18 && v.v >= 0.35; break;                             // pale greys
            case Effect::Sway: on = !bb.empty() && y < bb.y + bb.h * 0.62; break;                   // above the trunk
            case Effect::Flag: on = !bb.empty() && x > bb.x + std::max(1, bb.w / 10); break;        // right of the pole
            case Effect::Pulse: on = true; break;
            }
            m.set(x, y, on);
        }
    return m;
}

EffectClip animateEffect(const Image& sprite, const EffectOptions& in) {
    EffectOptions o = in;
    o.frames = std::clamp(o.frames, 2, 64);
    o.amount = std::clamp(o.amount, 0.25, 2.0);
    const Mask region = o.region.isNull() || o.region.width() != sprite.width() || o.region.height() != sprite.height()
                            ? autoRegion(sprite, o.effect)
                            : o.region;
    switch (o.effect) {
    case Effect::Fire: return fire(sprite, region, o);
    case Effect::Water: return water(sprite, region, o);
    case Effect::Sway: return sway(sprite, region, o);
    case Effect::Flag: return flag(sprite, region, o);
    case Effect::Smoke: return smoke(sprite, region, o);
    case Effect::Pulse: return pulse(sprite, region, o);
    }
    return {};
}

} // namespace acestudio::fx
