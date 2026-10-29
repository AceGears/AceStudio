#include "acestudio/core/Palette.h"

#include <limits>

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

} // namespace acestudio
