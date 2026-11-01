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
template <BlendMode M> inline float blendChannelT(float cb, float cs) noexcept {
    if constexpr (M == BlendMode::Normal) return cs;
    else if constexpr (M == BlendMode::Multiply) return cb * cs;
    else if constexpr (M == BlendMode::Screen) return cb + cs - cb * cs;
    else if constexpr (M == BlendMode::Overlay) return cb <= 0.5f ? 2.f * cb * cs : 1.f - 2.f * (1.f - cb) * (1.f - cs);
    else if constexpr (M == BlendMode::Darken) return std::min(cb, cs);
    else if constexpr (M == BlendMode::Lighten) return std::max(cb, cs);
    else if constexpr (M == BlendMode::Add) return std::min(1.f, cb + cs);
    else if constexpr (M == BlendMode::Subtract) return std::max(0.f, cb - cs);
    else return std::fabs(cb - cs);   // Difference
}

// Same as std::lround for the clamped, non-negative range: v * 255 < 2^23, so
// adding 0.5 is exact and truncation rounds halves away from zero.
inline std::uint8_t toByte(float v) noexcept {
    return static_cast<std::uint8_t>(static_cast<int>(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f));
}

template <BlendMode M> inline Rgba8 blendPixelT(Rgba8 dst, Rgba8 src, std::uint8_t opacity) noexcept {
    const float as = (src.a / 255.f) * (opacity / 255.f);
    if (as <= 0.f) return dst;
    const float ab = dst.a / 255.f;

    // Fast exact path for the overwhelmingly common pixel-art case.
    if (M == BlendMode::Normal && as >= 1.f) return {src.r, src.g, src.b, 255};

    const float ao = as + ab * (1.f - as);
    const float cs[3] = {src.r / 255.f, src.g / 255.f, src.b / 255.f};
    const float cb[3] = {dst.r / 255.f, dst.g / 255.f, dst.b / 255.f};
    std::uint8_t out[3];
    for (int i = 0; i < 3; ++i) {
        // W3C: Cs' = (1 - ab) * Cs + ab * B(Cb, Cs); then source-over.
        const float mixed = (1.f - ab) * cs[i] + ab * blendChannelT<M>(cb[i], cs[i]);
        const float co = as * mixed + ab * cb[i] * (1.f - as);   // premultiplied result
        out[i] = toByte(ao > 0.f ? co / ao : 0.f);
    }
    return {out[0], out[1], out[2], toByte(ao)};
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
    switch (mode) {
    case BlendMode::Normal: return blendPixelT<BlendMode::Normal>(dst, src, opacity);
    case BlendMode::Multiply: return blendPixelT<BlendMode::Multiply>(dst, src, opacity);
    case BlendMode::Screen: return blendPixelT<BlendMode::Screen>(dst, src, opacity);
    case BlendMode::Overlay: return blendPixelT<BlendMode::Overlay>(dst, src, opacity);
    case BlendMode::Darken: return blendPixelT<BlendMode::Darken>(dst, src, opacity);
    case BlendMode::Lighten: return blendPixelT<BlendMode::Lighten>(dst, src, opacity);
    case BlendMode::Add: return blendPixelT<BlendMode::Add>(dst, src, opacity);
    case BlendMode::Subtract: return blendPixelT<BlendMode::Subtract>(dst, src, opacity);
    case BlendMode::Difference: return blendPixelT<BlendMode::Difference>(dst, src, opacity);
    }
    return blendPixelT<BlendMode::Normal>(dst, src, opacity);
}

namespace {
// Exact-integer source-over for straight alpha (normal mode), the hot path.
// floor((v + 127) / 255) for v in [0, 65280], without a division (exact;
// checked exhaustively).
inline unsigned div255r(unsigned v) noexcept {
    v += 128u;
    return (v + (v >> 8)) >> 8;
}

inline Rgba8 normalOver(Rgba8 d, Rgba8 s, unsigned sa) noexcept {
    if (sa >= 255) return {s.r, s.g, s.b, 255};
    const unsigned inv = 255u - sa;
    const unsigned dw = div255r(d.a * inv);                  // backdrop weight
    const unsigned oa = sa + dw;
    if (oa == 0) return Rgba8::transparent();
    if (oa == 255) {   // opaque result (opaque backdrop): the common case
        return {static_cast<std::uint8_t>(div255r(s.r * sa + d.r * dw)), static_cast<std::uint8_t>(div255r(s.g * sa + d.g * dw)),
                static_cast<std::uint8_t>(div255r(s.b * sa + d.b * dw)), 255};
    }
    auto ch = [&](unsigned cs, unsigned cd) {
        return static_cast<std::uint8_t>((cs * sa + cd * dw + oa / 2) / oa);
    };
    return {ch(s.r, d.r), ch(s.g, d.g), ch(s.b, d.b), static_cast<std::uint8_t>(oa)};
}
} // namespace

namespace {
template <BlendMode M>
void blendRows(Rgba8* dpx, const Rgba8* spx, std::size_t dw, std::size_t sw, const IntRect& area, int ox, int oy,
               std::uint8_t opacity) noexcept {
    for (int y = area.y; y < area.y + area.h; ++y) {
        Rgba8* drow = dpx + static_cast<std::size_t>(y) * dw;
        const Rgba8* srow = spx + static_cast<std::size_t>(y - oy) * sw;
        for (int x = area.x; x < area.x + area.w; ++x) {
            const Rgba8 s = srow[x - ox];
            if (s.a == 0) continue;
            Rgba8& d = drow[x];
            if constexpr (M == BlendMode::Normal) {
                const unsigned sa = opacity == 255 ? s.a : div255r(s.a * opacity);
                if (sa) d = normalOver(d, s, sa);
            } else {
                d = blendPixelT<M>(d, s, opacity);
            }
        }
    }
}
} // namespace

void blendImage(Image& dst, const Image& src, int ox, int oy, BlendMode mode, std::uint8_t opacity) noexcept {
    if (opacity == 0) return;
    const IntRect area = IntRect{ox, oy, src.width(), src.height()}.intersected(dst.bounds());
    if (area.empty()) return;
    Rgba8* d = dst.pixels().data();
    const Rgba8* s = src.pixels().data();
    const std::size_t dw = static_cast<std::size_t>(dst.width()), sw = static_cast<std::size_t>(src.width());
    switch (mode) {
#define ACE_BLEND_CASE(m) case BlendMode::m: blendRows<BlendMode::m>(d, s, dw, sw, area, ox, oy, opacity); break;
        ACE_BLEND_CASE(Normal) ACE_BLEND_CASE(Multiply) ACE_BLEND_CASE(Screen) ACE_BLEND_CASE(Overlay)
        ACE_BLEND_CASE(Darken) ACE_BLEND_CASE(Lighten) ACE_BLEND_CASE(Add) ACE_BLEND_CASE(Subtract)
        ACE_BLEND_CASE(Difference)
#undef ACE_BLEND_CASE
    }
}

} // namespace acestudio
