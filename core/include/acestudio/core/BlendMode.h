#pragma once

#include "acestudio/core/Color.h"

#include <optional>
#include <string_view>

namespace acestudio {

enum class BlendMode {
    Normal,
    Multiply,
    Screen,
    Overlay,
    Darken,
    Lighten,
    Add,
    Subtract,
    Difference,
};

std::string_view toString(BlendMode mode) noexcept;
std::optional<BlendMode> blendModeFromString(std::string_view s) noexcept;

// Composites `src` over `dst` with the given mode and extra opacity (0..255).
// Both inputs and the result are straight-alpha RGBA8.
// Uses the W3C compositing model: B(Cb, Cs) mixed by backdrop alpha,
// then source-over.
Rgba8 blendPixel(Rgba8 dst, Rgba8 src, BlendMode mode, std::uint8_t opacity) noexcept;

} // namespace acestudio
