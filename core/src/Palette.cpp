#include "acestudio/core/Palette.h"

#include <limits>
#include <unordered_map>

namespace acestudio {

Status Palette::add(Rgba8 c) {
    if (m_colors.size() >= kMaxColors)
        return Error{ErrorCode::InvalidArgument, "Palette is full (256 colors)"};
    m_colors.push_back(c);
    return {};
}

Status Palette::set(std::size_t i, Rgba8 c) {
    if (i >= m_colors.size()) return Error{ErrorCode::InvalidArgument, "Palette index out of range"};
    m_colors[i] = c;
    return {};
}

Status Palette::remove(std::size_t i) {
    if (i >= m_colors.size()) return Error{ErrorCode::InvalidArgument, "Palette index out of range"};
    m_colors.erase(m_colors.begin() + static_cast<std::ptrdiff_t>(i));
    return {};
}

Status Palette::insert(std::size_t i, Rgba8 c) {
    if (m_colors.size() >= kMaxColors) return Error{ErrorCode::InvalidArgument, "Palette is full (256 colors)"};
    if (i > m_colors.size()) return Error{ErrorCode::InvalidArgument, "Palette index out of range"};
    m_colors.insert(m_colors.begin() + static_cast<std::ptrdiff_t>(i), c);
    return {};
}

std::optional<std::size_t> Palette::find(Rgba8 c) const noexcept {
    for (std::size_t i = 0; i < m_colors.size(); ++i)
        if (m_colors[i] == c) return i;
    return std::nullopt;
}

std::optional<std::size_t> Palette::nearest(Rgba8 c) const noexcept {
    std::optional<std::size_t> best;
    long bestDist = std::numeric_limits<long>::max();
    for (std::size_t i = 0; i < m_colors.size(); ++i) {
        const auto& p = m_colors[i];
        const long dr = long{p.r} - c.r, dg = long{p.g} - c.g, db = long{p.b} - c.b, da = long{p.a} - c.a;
        const long d = dr * dr + dg * dg + db * db + da * da;
        if (d < bestDist) { bestDist = d; best = i; }
    }
    return best;
}

Palette Palette::defaultPalette() {
    // Original 16-color starter palette (neutral ramp + basic hues).
    Palette p("AceStudio Starter 16");
    for (const char* hex : {"#000000", "#1D1D24", "#3B3B47", "#6B6B7A", "#A3A3B0", "#E3E3EA", "#FFFFFF",
                            "#7A1E2C", "#D2453A", "#F29B38", "#F7DC5C", "#5BA84A", "#2B6B5E",
                            "#3A6FD8", "#7FC4F0", "#8A4FB8"}) {
        (void)p.add(*Rgba8::fromHex(hex));
    }
    return p;
}

namespace {
std::uint32_t rgbKey(Rgba8 c) noexcept {
    return (std::uint32_t{c.r} << 16) | (std::uint32_t{c.g} << 8) | std::uint32_t{c.b};
}
} // namespace

Rgba8 snapToPalette(const Palette& p, Rgba8 c) noexcept {
    if (c.a == 0) return c;
    const Rgba8* best = nullptr;
    long bestDist = 0;
    for (const Rgba8& q : p.colors()) {
        if (q.a == 0) continue;
        const long dr = long{q.r} - c.r, dg = long{q.g} - c.g, db = long{q.b} - c.b;
        const long dist = dr * dr + dg * dg + db * db;
        if (!best || dist < bestDist) { best = &q; bestDist = dist; }
    }
    return best ? Rgba8{best->r, best->g, best->b, c.a} : c;
}

std::size_t snapToPalette(const Palette& p, Image& img) {
    if (img.isNull()) return 0;
    std::unordered_map<std::uint32_t, Rgba8> memo;
    std::size_t changed = 0;
    for (Rgba8& px : img.pixels()) {
        if (px.a == 0) continue;
        const std::uint32_t key = rgbKey(px);
        auto it = memo.find(key);
        if (it == memo.end()) it = memo.emplace(key, snapToPalette(p, Rgba8{px.r, px.g, px.b, 255})).first;
        const Rgba8 out{it->second.r, it->second.g, it->second.b, px.a};
        if (out == px) continue;
        px = out;
        ++changed;
    }
    return changed;
}

std::size_t countOffPalette(const Palette& p, const Image& img) {
    if (img.isNull()) return 0;
    std::unordered_map<std::uint32_t, bool> on;
    for (const Rgba8& q : p.colors())
        if (q.a > 0) on[rgbKey(q)] = true;
    std::size_t off = 0;
    for (const Rgba8& px : img.pixels())
        if (px.a > 0 && !on.count(rgbKey(px))) ++off;
    return off;
}

} // namespace acestudio
