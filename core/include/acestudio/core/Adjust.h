#pragma once

// Colour adjustments shared by adjustment layers (non-destructive) and
// "Apply" commands (destructive). Only RGB changes; alpha is kept.

#include "acestudio/core/Document.h"
#include "acestudio/core/Image.h"

namespace acestudio {

Rgba8 adjustPixel(Rgba8 c, const Adjustment& adj) noexcept;
void applyAdjustment(Image& img, const Adjustment& adj);

} // namespace acestudio
