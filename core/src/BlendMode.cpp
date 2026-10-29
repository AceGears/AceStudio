#include "acestudio/core/BlendMode.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace acestudio {

namespace {
constexpr std::array<std::pair<BlendMode, std::string_view>, 9> kNames{{
    {BlendMode::Normal, "normal"},     {BlendMode::Multiply, "multiply"},
    {BlendMode::Screen, "screen"},     {BlendMode::Overlay, "overlay"},
    {BlendMode::Darken, "darken"},     {BlendMode::Lighten, "lighten"},
    {BlendMode::Add, "add"},           {BlendMode::Subtract, "subtract"},
    {BlendMode::Difference, "difference"},
}};

// Separable blend function B(cb, cs) on normalized channels.
float blendChannel(float cb, float cs, BlendMode mode) noexcept {
    switch (mode) {
    case BlendMode::Normal: return cs;
    case BlendMode::Multiply: return cb * cs;
    case BlendMode::Screen: return cb + cs - cb * cs;
    case BlendMode::Overlay:
        return cb <= 0.5f ? 2.f * cb * cs : 1.f - 2.f * (1.f - cb) * (1.f - cs);
    case BlendMode::Darken: return std::min(cb, cs);
    case BlendMode::Lighten: return std::max(cb, cs);
    case BlendMode::Add: return std::min(1.f, cb + cs);
    case BlendMode::Subtract: return std::max(0.f, cb - cs);
    case BlendMode::Difference: return std::fabs(cb - cs);
    }
    return cs;
}

std::uint8_t toByte(float v) noexcept {
    return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.f, 1.f) * 255.f));
}
} // namespace

std::string_view toString(BlendMode mode) noexcept {
    for (const auto& [m, n] : kNames)
        if (m == mode) return n;
    return "normal";
}

std::optional<BlendMode> blendModeFromString(std::string_view s) noexcept {
    for (const auto& [m, n] : kNames)
        if (n == s) return m;
    return std::nullopt;
}

Rgba8 blendPixel(Rgba8 dst, Rgba8 src, BlendMode mode, std::uint8_t opacity) noexcept {
    const float as = (src.a / 255.f) * (opacity / 255.f);
    if (as <= 0.f) return dst;
    const float ab = dst.a / 255.f;

    // Fast exact path for the overwhelmingly common pixel-art case.
    if (mode == BlendMode::Normal && as >= 1.f) return {src.r, src.g, src.b, 255};

    const float ao = as + ab * (1.f - as);
    const float cs[3] = {src.r / 255.f, src.g / 255.f, src.b / 255.f};
    const float cb[3] = {dst.r / 255.f, dst.g / 255.f, dst.b / 255.f};
    std::uint8_t out[3];
    for (int i = 0; i < 3; ++i) {
        // W3C: Cs' = (1 - ab) * Cs + ab * B(Cb, Cs); then source-over.
        const float mixed = (1.f - ab) * cs[i] + ab * blendChannel(cb[i], cs[i], mode);
        const float co = as * mixed + ab * cb[i] * (1.f - as);   // premultiplied result
        out[i] = toByte(ao > 0.f ? co / ao : 0.f);
    }
    return {out[0], out[1], out[2], toByte(ao)};
}

namespace {
// Exact-integer source-over for straight alpha (normal mode), the hot path.
inline Rgba8 normalOver(Rgba8 d, Rgba8 s, unsigned sa) noexcept {
    if (sa >= 255) return {s.r, s.g, s.b, 255};
    const unsigned inv = 255u - sa;
    const unsigned dw = (d.a * inv + 127u) / 255u;          // backdrop weight
    const unsigned oa = sa + dw;
    if (oa == 0) return Rgba8::transparent();
    auto ch = [&](unsigned cs, unsigned cd) {
        return static_cast<std::uint8_t>((cs * sa + cd * dw + oa / 2) / oa);
    };
    return {ch(s.r, d.r), ch(s.g, d.g), ch(s.b, d.b), static_cast<std::uint8_t>(oa)};
}
} // namespace

void blendImage(Image& dst, const Image& src, int ox, int oy, BlendMode mode, std::uint8_t opacity) noexcept {
    if (opacity == 0) return;
    const IntRect area = IntRect{ox, oy, src.width(), src.height()}.intersected(dst.bounds());
    if (area.empty()) return;
    const auto dpx = dst.pixels();
    const auto spx = src.pixels();
    const std::size_t dw = static_cast<std::size_t>(dst.width()), sw = static_cast<std::size_t>(src.width());
    for (int y = area.y; y < area.y + area.h; ++y) {
        Rgba8* drow = dpx.data() + static_cast<std::size_t>(y) * dw;
        const Rgba8* srow = spx.data() + static_cast<std::size_t>(y - oy) * sw;
        for (int x = area.x; x < area.x + area.w; ++x) {
            const Rgba8 s = srow[x - ox];
            if (s.a == 0) continue;
            Rgba8& d = drow[x];
            if (mode == BlendMode::Normal) {
                const unsigned sa = opacity == 255 ? s.a : (s.a * opacity + 127u) / 255u;
                if (sa) d = normalOver(d, s, sa);
            } else {
                d = blendPixel(d, s, mode, opacity);
            }
        }
    }
}

} // namespace acestudio
