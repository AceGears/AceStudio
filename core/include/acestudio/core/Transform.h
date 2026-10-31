#pragma once

// Pixel-art-safe image transforms. None of these introduce new colours:
// flips/rotations by 90° are exact permutations, scaling is nearest
// neighbour, and RotSprite samples existing pixels only.

#include "acestudio/core/Image.h"

namespace acestudio {

Image flipHorizontal(const Image& src);
Image flipVertical(const Image& src);
// clockwise = true rotates 90° clockwise; result is h×w.
Image rotate90(const Image& src, bool clockwise);
Image rotate180(const Image& src);

// Nearest-neighbour resample to an exact size (pixel centres sampled).
Result<Image> scaleNearest(const Image& src, int width, int height);

// Scale2x (EPX) using a colour-similarity threshold (max per-channel
// difference). threshold = 0 is classic exact-match Scale2x.
Image scale2xSimilar(const Image& src, int threshold);

// RotSprite: rotates by `degrees` (clockwise) around the image centre,
// keeping the same canvas size. The source is upscaled 8× with Scale2x
// (3 passes, similarity threshold), rotated with nearest sampling and
// sampled back down, so edges stay clean and no new colours appear.
// Multiples of 90° are routed to the exact rotations.
// With `expand`, the result is enlarged to hold the whole rotated image
// (centred on the same point) instead of keeping the source size.
Image rotSprite(const Image& src, double degrees, int similarity = 24, bool expand = false);

// Fast nearest-neighbour rotation (no Scale2x pass), for interactive
// previews; same geometry as rotSprite.
Image rotateNearest(const Image& src, double degrees, bool expand = true);

// Free transform of a lifted block: nearest-neighbour scale by (sx, sy) —
// negative factors flip — then rotation by `degrees` clockwise, expanded to
// fit. `quality` uses RotSprite for the rotation, otherwise rotateNearest.
Image freeTransform(const Image& src, double sx, double sy, double degrees, bool quality);

enum class OutlineMode { Outside, Inside };
// Draws a 1px outline in `color` around opaque pixels (alpha > 0).
// Outside: transparent pixels touching the shape become `color`.
// Inside: edge pixels of the shape become `color`.
// diagonal = true also treats 8-connected neighbours as touching.
Image outline(const Image& src, Rgba8 color, OutlineMode mode, bool diagonal);

} // namespace acestudio
