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

// Value noise that tiles: periodic in x over `gx` cells and in y over `gy`
// cells, so scrolling it by one period loops seamlessly.
struct TileNoise {
    double cw, ch;   // cell width and height in pixels
    int gx, gy;
    unsigned seed;
    double operator()(double x, double y) const {
        const double xf = x / cw, yf = y / ch;
        const double fx0 = std::floor(xf), fy0 = std::floor(yf);
        double tx = xf - fx0, ty = yf - fy0;
        tx = tx * tx * (3 - 2 * tx);
        ty = ty * ty * (3 - 2 * ty);
        auto wrap = [](long v, int n) { return static_cast<unsigned>(((v % n) + n) % n); };
        const long ix = static_cast<long>(fx0), iy = static_cast<long>(fy0);
        const unsigned x0 = wrap(ix, gx), x1 = wrap(ix + 1, gx), y0 = wrap(iy, gy), y1 = wrap(iy + 1, gy);
        const double a = hash01(x0, y0, seed), b = hash01(x1, y0, seed), c = hash01(x0, y1, seed), d = hash01(x1, y1, seed);
        return (a * (1 - tx) + b * tx) * (1 - ty) + (c * (1 - tx) + d * tx) * ty;
    }
};

// Fire, generated rather than bent. The drawn flame is only used for its
// place, size and colours; every frame a new flame is made:
//  - a teardrop "fuel" shape over the logs, strongest at the base centre;
//  - turbulence rising through it (tall noise cells make tongues), eating
//    it away more towards the tips, so tongues lick, lean (a sideways warp
//    that grows with height) and tear off as they rise;
//  - the result quantised to a ramp of the sprite's own fire colours, from
//    dark red at the edges to the pale yellow core;
//  - logs and stones stay in front; embers among the logs glow and fade.
// The noise rises exactly one period per loop: seamless. Bending the drawn
// flame (the earlier versions) could not make it look like fire.
EffectClip fire(const Image& src, const Mask& flameColours, const EffectOptions& o) {
    EffectClip out;
    const int W = src.width(), H = src.height();
    const int frames = o.frames > 0 ? o.frames : 12;
    auto lumaOf = [](const Rgba8& c) { return 0.299 * c.r + 0.587 * c.g + 0.114 * c.b; };
    // Flame, logs, embers. Dull warm colours (brown wood) are not flame.
    Mask flame(W, H), logs(W, H), embers(W, H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const Rgba8 c = src.pixel(x, y);
            if (!c.a) continue;
            const Hsv v = hsv(c);
            const bool f = flameColours.get(x, y) && !(v.v < 0.7 && v.h >= 12 && v.h <= 60);
            (f ? flame : logs).set(x, y, true);
        }
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            if (!flame.get(x, y)) continue;
            int n = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    n += (x + dx >= 0 && y + dy >= 0 && x + dx < W && y + dy < H && logs.get(x + dx, y + dy)) ? 1 : 0;
            if (n >= 4) { embers.set(x, y, true); flame.set(x, y, false); }
        }
    out.region = flame;
    const IntRect fb = maskBounds(flame);
    if (fb.empty()) {
        for (int f = 0; f < frames; ++f) { out.frames.push_back(src); out.durations.push_back(o.frameMs); }
        return out;
    }
    // Ramp: the flame's colours from red through orange to yellow (by hue,
    // pale ones last), plus a darker red for the outer edge.
    std::vector<Rgba8> cols;
    for (int y = fb.y; y < fb.y + fb.h; ++y)
        for (int x = fb.x; x < fb.x + fb.w; ++x)
            if (flame.get(x, y)) {
                const Rgba8 c = src.pixel(x, y);
                if (std::find(cols.begin(), cols.end(), c) == cols.end()) cols.push_back(c);
            }
    std::sort(cols.begin(), cols.end(), [&](const Rgba8& a, const Rgba8& b) {
        const Hsv ha = hsv(a), hb = hsv(b);
        const double ka = ha.h + (std::min({a.r, a.g, a.b}) > 200 ? 60 : 0), kb = hb.h + (std::min({b.r, b.g, b.b}) > 200 ? 60 : 0);
        return ka != kb ? ka < kb : lumaOf(a) < lumaOf(b);
    });
    std::vector<Rgba8> ramp;
    const int k = static_cast<int>(cols.size());
    for (int i = 0; i < 5; ++i) ramp.push_back(cols[static_cast<std::size_t>(std::lround(i * (k - 1) / 4.0))]);
    // The outer edge: the darkest red already in the sprite (an outline),
    // else the flame's own darkest colour. No new colours: the palette stays.
    Rgba8 edge = ramp[0];
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const Rgba8 c = src.pixel(x, y);
            if (!c.a) continue;
            const Hsv v = hsv(c);
            if ((v.h <= 20 || v.h >= 340) && v.s >= 0.5 && v.v >= 0.25 && lumaOf(c) < lumaOf(edge)) edge = c;
        }
    ramp.insert(ramp.begin(), edge);
    // The fuel shape.
    double sx = 0;
    std::vector<int> xs;
    for (int y = fb.y; y < fb.y + fb.h; ++y)
        for (int x = fb.x; x < fb.x + fb.w; ++x)
            if (flame.get(x, y)) { sx += x; xs.push_back(x); }
    std::sort(xs.begin(), xs.end());
    const double cx = sx / static_cast<double>(xs.size());
    const double halfW = std::max(2.0, (xs[xs.size() * 95 / 100] - xs[xs.size() * 5 / 100]) / 2.0);
    const int base = fb.y + fb.h - 1;
    const double flameH = fb.h;
    const double amount = std::clamp(o.amount, 0.25, 2.0);
    // Noise: one period in y is what the flame rises per loop.
    const int period = std::max(24, static_cast<int>(flameH * 1.5)) / 6 * 6;
    auto tile = [&](double cw, double ch, unsigned salt) {
        return TileNoise{cw, ch, std::max(1, static_cast<int>(std::ceil(W / cw)) + 1), std::max(1, static_cast<int>(std::lround(period / ch))), o.seed * 977u + salt};
    };
    const TileNoise n1 = tile(3.0, 7.5, 1), n2 = tile(1.8, 4.5, 2), n3 = tile(1.2, 3.0, 3), warp = tile(6.0, 12.0, 4);
    static const double kLevels[] = {0.06, 0.16, 0.30, 0.47, 0.66, 0.86};
    for (int f = 0; f < frames; ++f) {
        const double t = static_cast<double>(f) / frames;
        const double rise = t * period;
        Image img = src;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                if (flame.get(x, y)) img.setPixel(x, y, {0, 0, 0, 0});
        const int top = std::max(0, base - static_cast<int>(flameH * 1.3));
        for (int y = top; y <= base + 1 && y < H; ++y)
            for (int x = 0; x < W; ++x) {
                if (logs.get(x, y)) continue;   // logs and stones in front
                const double u = std::clamp((base + 1.0 - y) / (flameH * 1.15), 0.0, 1.5);
                const double wid = halfW * 1.25 * (1.05 - 0.55 * std::min(u, 1.0));
                const double d = std::abs(x - cx) / std::max(0.5, wid);
                const double shape = std::max(0.0, 1 - d * d) * std::max(0.0, 1 - std::pow(std::min(u, 1.0), 1.6)) * 1.45;
                if (shape <= 0) continue;
                const double xw = x + (warp(x, y + rise) - 0.5) * 2 * (1.0 + 3.5 * u);
                const double n = 0.55 * n1(xw, y + rise) + 0.3 * n2(xw, y + 2 * rise) + 0.15 * n3(xw, y + 3 * rise);
                const double I = shape * (1.0 - (0.25 + 1.35 * u) * n * amount);
                int level = -1;
                for (int i = 0; i < 6; ++i)
                    if (I > kLevels[i]) level = i;
                if (level >= 0) img.setPixel(x, y, ramp[static_cast<std::size_t>(level)]);
            }
        // Embers glow and fade.
        for (int y = fb.y; y <= base && y < H; ++y)
            for (int x = 0; x < W; ++x)
                if (embers.get(x, y) && n3(x * 3.0, y * 3.0 + 3 * rise) > 0.6) img.setPixel(x, y, ramp.back());
        out.frames.push_back(std::move(img));
        out.durations.push_back(o.frameMs > 0 ? o.frameMs : 80);
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
    // 0 = the effect's own loop length: fire needs longer to rise and break up.
    o.frames = std::clamp(o.frames > 0 ? o.frames : (o.effect == Effect::Fire ? 12 : 8), 2, 64);
    if (o.effect == Effect::Fire && in.frameMs == 90) o.frameMs = 80;
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
