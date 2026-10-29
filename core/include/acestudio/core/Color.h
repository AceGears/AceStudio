#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace acestudio {

// 8-bit straight (non-premultiplied) RGBA. This is the storage format for
// all raster data; premultiplication happens only inside the compositor.
struct Rgba8 {
    std::uint8_t r = 0, g = 0, b = 0, a = 0;

    constexpr bool operator==(const Rgba8&) const = default;

    static constexpr Rgba8 transparent() { return {0, 0, 0, 0}; }
    static constexpr Rgba8 opaque(std::uint8_t r, std::uint8_t g, std::uint8_t b) { return {r, g, b, 255}; }

    // "#RRGGBB" or "#RRGGBBAA" (case-insensitive, leading '#' optional).
    static std::optional<Rgba8> fromHex(std::string_view hex);
    std::string toHex(bool withAlpha = true) const;

    // Rec. 709 luma, used for grayscale conversion.
    std::uint8_t luma() const noexcept;
};

} // namespace acestudio
