#pragma once

// Colour adjustments shared by adjustment layers (non-destructive) and
// "Apply" commands (destructive). Only RGB changes; alpha is kept.

#include "acestudio/core/Document.h"
#include "acestudio/core/Image.h"

namespace acestudio {

#include <array>

// An adjustment ready to apply: per-channel types (brightness/contrast,
// invert, posterize, levels, curves) become a 256-entry lookup table.
class PreparedAdjustment {
public:
    explicit PreparedAdjustment(const Adjustment& adj);
    Rgba8 apply(Rgba8 c) const noexcept;

private:
    Adjustment m_adj;
    bool m_lut = false;
    std::array<std::uint8_t, 256> m_table{};
};

// The curve through the control points as a 256-entry table (monotone cubic
// Hermite interpolation, flat beyond the end points).
std::array<std::uint8_t, 256> curveTable(const std::vector<CurvePoint>& points);

Rgba8 adjustPixel(Rgba8 c, const Adjustment& adj) noexcept;
void applyAdjustment(Image& img, const Adjustment& adj);

} // namespace acestudio
