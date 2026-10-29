#pragma once

// Animated image encoders and decoders.
//
//   GIF   own LZW encoder; one global palette (exact when the frames use
//         <= 255 opaque colours, otherwise median cut); alpha < 128 becomes
//         the transparent index. GIF delays are in 1/100 s, so durations are
//         rounded to 10 ms with a 20 ms minimum (browsers slow down anything
//         shorter).
//   APNG  RGBA8, every frame full-canvas, lossless.
//   WebP  libwebp animation, lossless and exact (RGB under transparent
//         pixels preserved).
//
// Decoders return fully composited, canvas-sized frames.

#include "acestudio/core/Image.h"
#include "acestudio/core/Result.h"

#include <cstdint>
#include <span>
#include <vector>

namespace acestudio {

struct AnimFrame {
    Image image;
    int durationMs = 100;
};

struct DecodedAnimation {
    std::vector<AnimFrame> frames;
    int loopCount = 0;   // 0 = forever
};

// loopCount: 0 = forever, n = play n times.
Result<std::vector<std::uint8_t>> encodeGif(const std::vector<AnimFrame>& frames, int loopCount = 0);
Result<std::vector<std::uint8_t>> encodeApng(const std::vector<AnimFrame>& frames, int loopCount = 0);
Result<std::vector<std::uint8_t>> encodeWebp(const std::vector<AnimFrame>& frames, int loopCount = 0);

Result<DecodedAnimation> decodeGif(std::span<const std::uint8_t> bytes);
Result<DecodedAnimation> decodeApng(std::span<const std::uint8_t> bytes);
Result<DecodedAnimation> decodeWebp(std::span<const std::uint8_t> bytes);

// GIF stores delays in centiseconds; this is what a GIF of `ms` will play at.
int gifEffectiveDurationMs(int ms) noexcept;

} // namespace acestudio
