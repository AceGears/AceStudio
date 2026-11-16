#pragma once

#include "acestudio/core/Color.h"
#include "acestudio/core/Image.h"
#include "acestudio/core/Result.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace acestudio {

// An ordered palette of up to 256 colors (indexed-mode limit).
class Palette {
public:
    static constexpr std::size_t kMaxColors = 256;

    Palette() = default;
    explicit Palette(std::string name) : m_name(std::move(name)) {}

    const std::string& name() const noexcept { return m_name; }
    void setName(std::string n) { m_name = std::move(n); }

    std::size_t size() const noexcept { return m_colors.size(); }
    bool empty() const noexcept { return m_colors.empty(); }
    const std::vector<Rgba8>& colors() const noexcept { return m_colors; }
    Rgba8 at(std::size_t i) const { return m_colors.at(i); }

    Status add(Rgba8 c);
    Status set(std::size_t i, Rgba8 c);
    Status remove(std::size_t i);
    Status insert(std::size_t i, Rgba8 c);
    void clear() noexcept { m_colors.clear(); }

    // Exact match index, if present.
    std::optional<std::size_t> find(Rgba8 c) const noexcept;
    // Nearest color by squared RGBA distance; nullopt if empty.
    std::optional<std::size_t> nearest(Rgba8 c) const noexcept;

    // A small neutral default so new documents are immediately usable.
    static Palette defaultPalette();

    bool operator==(const Palette&) const = default;

private:
    std::string m_name = "Untitled";
    std::vector<Rgba8> m_colors;
};

// ---- Palette lock (ADR-ANIM-006) ----
// The opaque palette colour nearest to c's RGB, with c's alpha kept; c itself
// when it is transparent or the palette has no opaque colour.
Rgba8 snapToPalette(const Palette& p, Rgba8 c) noexcept;
// Snaps every pixel of `img` as above; returns how many pixels changed.
std::size_t snapToPalette(const Palette& p, Image& img);
// Pixels with alpha > 0 whose RGB is not an opaque palette colour.
std::size_t countOffPalette(const Palette& p, const Image& img);

} // namespace acestudio
