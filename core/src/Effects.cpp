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

// Fire, made of coherent shapes rather than per-pixel noise (which sliced
// the flames into diagonal stripes on the test PC's campfires):
//  - each column of the flame stretches or shrinks as a whole, by an amount
//    that varies smoothly along the fire and travels through it: two or three
//    tongues rise and fall in turn. The colours of a column come from that
//    column (sampled along its own height), so the bright core stays at the
//    base and the dark tips at the top;
//  - whole rows sway sideways, more towards the tips, by an amount that
//    changes smoothly with height (no tearing between rows);
//  - logs and stones are never painted over; a few embers rise above.
EffectClip fire(const Image& src, const Mask& flameColours, const EffectOptions& o) {
    EffectClip out;
    // The flame's dark outline is part of the flame where no log can be (the
    // upper part of the fire): left out, it stayed still while the flame
    // moved and showed as dark cracks.
    Mask region = flameColours;
    {
        const IntRect cb = maskBounds(flameColours);
        const int logLine = cb.y + static_cast<int>(cb.h * 0.6);
        for (int pass = 0; pass < 2 && !cb.empty(); ++pass) {
            Mask grown = region;
            for (int y = std::max(0, cb.y - 2); y < std::min(src.height(), logLine); ++y)
                for (int x = std::max(0, cb.x - 2); x < std::min(src.width(), cb.x + cb.w + 2); ++x) {
                    if (region.get(x, y) || src.pixel(x, y).a == 0) continue;
                    bool next = false;
                    for (int dy = -1; dy <= 1 && !next; ++dy)
                        for (int dx = -1; dx <= 1 && !next; ++dx)
                            next = x + dx >= 0 && y + dy >= 0 && x + dx < src.width() && y + dy < src.height() && region.get(x + dx, y + dy);
                    if (next) grown.set(x, y, true);
                }
            region = grown;
        }
    }
    out.region = region;
    const IntRect fb = maskBounds(region);
    const int W = src.width(), H = src.height();
    const auto r = ramp(src, region);
    const double flameH = std::max(2, fb.h);
    const double amp = 0.22 * std::clamp(o.amount, 0.25, 2.0);
    const double swayAmp = std::max(1.0, fb.w * 0.08) * std::clamp(o.amount, 0.25, 2.0);
    // Tongue width: wide enough that neighbouring columns differ by about a
    // pixel in height (narrow tongues put the dark tip of a short column
    // inside its taller neighbour: dark cracks).
    const double tongue = std::max(6.0, fb.w * 1.1);
    const double ph1 = kTau * hash01(1, 1, o.seed), ph2 = kTau * hash01(2, 2, o.seed);
    // Each column's flame: lowest and highest flame pixel.
    std::vector<int> colBase(static_cast<std::size_t>(W), -1), colTop(static_cast<std::size_t>(W), -1);
    for (int x = 0; x < W; ++x)
        for (int y = 0; y < H; ++y)
            if (region.get(x, y)) {
                if (colTop[static_cast<std::size_t>(x)] < 0) colTop[static_cast<std::size_t>(x)] = y;
                colBase[static_cast<std::size_t>(x)] = y;
            }
    const int embers = fb.empty() ? 0 : std::clamp(fb.w / 8, 1, 4);
    for (int f = 0; f < o.frames; ++f) {
        const double t = static_cast<double>(f) / o.frames;
        Image img = src;
        if (!fb.empty()) {
            // Clear the flame, then draw it again stretched and swayed.
            for (int y = fb.y; y < fb.y + fb.h; ++y)
                for (int x = fb.x; x < fb.x + fb.w; ++x)
                    if (region.get(x, y)) img.setPixel(x, y, {0, 0, 0, 0});
            const int reach = static_cast<int>(std::ceil(flameH * amp)) + 1;
            for (int x = fb.x; x < fb.x + fb.w; ++x) {
                const int base = colBase[static_cast<std::size_t>(x)], top = colTop[static_cast<std::size_t>(x)];
                if (base < 0) continue;
                // Tongues: a travelling wave along the fire (whole cycles per loop).
                const double v = 0.7 * std::sin(kTau * (x / tongue - t) + ph1) + 0.3 * std::sin(kTau * (x / (tongue * 0.5) + 2 * t) + ph2);
                const double hf = 1.0 + amp * v;
                const double len = base - top + 1;
                const int newTop = static_cast<int>(std::lround(base - len * hf + 1));
                for (int y = std::max(0, std::min(newTop, top) - reach); y <= base; ++y) {
                    if (y < newTop) continue;
                    // The row's sway: the same for the whole row (measured from
                    // the bottom of the whole fire, not this column's base,
                    // which tore rows apart), smooth in height.
                    const double u = std::clamp((fb.y + fb.h - 1 - y) / flameH, 0.0, 1.2);
                    const int dx = static_cast<int>(std::lround(swayAmp * std::pow(u, 1.5) * std::sin(kTau * (t - 0.35 * u)) ));
                    // Source row along this column's own flame.
                    const int sy = static_cast<int>(std::lround(base - (base - y) / hf));
                    if (sy < top || sy > base || !region.get(x, sy)) continue;
                    const int tx = x + dx;
                    if (tx < 0 || tx >= W || y < 0 || y >= H) continue;
                    const Rgba8 here = src.pixel(tx, y);
                    if (here.a && !region.get(tx, y)) continue;   // logs, stones stay in front
                    img.setPixel(tx, y, src.pixel(x, sy));
                }
            }
            for (int e = 0; e < embers && !r.empty(); ++e) {
                const double ex = fb.x + fb.w * (0.25 + 0.5 * hash01(static_cast<unsigned>(e), 5, o.seed));
                const double rise = std::fmod(t + static_cast<double>(e) / embers, 1.0);
                const int px = static_cast<int>(std::lround(ex + 1.5 * std::sin(kTau * (rise + e * 0.3))));
                const int py = static_cast<int>(std::lround(fb.y - rise * flameH * 0.6));
                if (rise < 0.7 && px >= 0 && py >= 0 && px < W && py < H && img.pixel(px, py).a == 0) img.setPixel(px, py, r.back());
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
            // Whole rows only (a per-pixel flutter tore the canopy apart),
            // rounded once per row so the row moves as one.
            (void)x;
            const double dx = std::round(amp * std::pow(u, 1.5) * std::sin(kTau * t - 0.6 * u) + 0.5 * o.amount * u * flutter(0, y, t));
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

// Smoke: a wave travels up the column (whole rows shift sideways, more as
// the smoke climbs), so it drifts and rises without being cut anywhere.
// Wrapping the column around (the first version) cut puffs off at the seam.
EffectClip smoke(const Image& src, const Mask& region, const EffectOptions& o) {
    EffectClip out;
    out.region = region;
    const IntRect b = maskBounds(region);
    const double h = std::max(2, b.h);
    const double amp = std::max(1.5, b.w * 0.18) * std::clamp(o.amount, 0.25, 2.0);
    const IntRect area{b.x - static_cast<int>(amp) - 2, b.y - 1, b.w + 2 * static_cast<int>(amp) + 4, b.h + 2};
    for (int f = 0; f < o.frames; ++f) {
        const double t = static_cast<double>(f) / o.frames;
        out.frames.push_back(displaced(src, region, area, [&](int, int y) {
            const double u = std::clamp((b.y + b.h - y) / h, 0.0, 1.0);
            // Phase falls with height: the bend travels upwards.
            return std::pair<double, double>{std::round(amp * std::pow(u, 1.2) * std::sin(kTau * (t - 1.2 * u))), 0.0};
        }));
        out.durations.push_back(o.frameMs);
    }
    return out;
}

// Magic: the drawing itself never changes shape (scaling it resampled every
// pixel); it glows. At the peak of the loop its colours step brighter and a
// one-pixel halo in its brightest colour lights up around it, then fades.
EffectClip pulse(const Image& src, const Mask& region, const EffectOptions& o) {
    EffectClip out;
    out.region = region;
    const auto r = ramp(src, region);
    const Rgba8 glowColour = r.empty() ? Rgba8{255, 255, 255, 255} : r.back();
    for (int f = 0; f < o.frames; ++f) {
        const double t = static_cast<double>(f) / o.frames;
        const double level = 0.5 - 0.5 * std::cos(kTau * t);   // 0 .. 1 .. 0
        Image img = src;
        // One colour step at the peak (two blew the crystal out to white).
        const int by = level * std::clamp(o.amount, 0.25, 2.0) > 0.55 ? 1 : 0;
        if (by)
            for (int y = 0; y < img.height(); ++y)
                for (int x = 0; x < img.width(); ++x)
                    if (region.get(x, y)) img.setPixel(x, y, shifted(r, src.pixel(x, y), by));
        // Halo: transparent pixels touching the region, on the brightest frames.
        if (level > 0.85)
            for (int y = 0; y < src.height(); ++y)
                for (int x = 0; x < src.width(); ++x) {
                    if (src.pixel(x, y).a) continue;
                    bool next = false;
                    for (int dy = -1; dy <= 1 && !next; ++dy)
                        for (int dx = -1; dx <= 1 && !next; ++dx)
                            if ((dx == 0) != (dy == 0))
                                next = x + dx >= 0 && y + dy >= 0 && x + dx < src.width() && y + dy < src.height() && region.get(x + dx, y + dy);
                    if (next) img.setPixel(x, y, glowColour);
                }
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
