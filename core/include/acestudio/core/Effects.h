#pragma once
// Procedural animation of environmental sprites without AI: fire, water,
// swaying plants and flags, rising smoke and pulsing magic.
//
// Every frame is sampled from the sprite's own pixels (nearest neighbour),
// so the palette is locked and edges stay hard; motion comes from noise
// that is periodic in time, so the cycle loops seamlessly. Only the part
// that should move does (the flames above the logs, the canopy above the
// trunk, the cloth of a flag): the rest stays exactly as drawn.

#include "acestudio/core/Image.h"
#include "acestudio/core/Raster.h"

#include <optional>
#include <string_view>
#include <vector>

namespace acestudio::fx {

enum class Effect { Fire, Water, Sway, Flag, Smoke, Pulse };
const char* toString(Effect e) noexcept;
std::optional<Effect> effectFromString(std::string_view s) noexcept;

// How a fire behaves (the same flame model, different physics):
//   Campfire  irregular flames of uneven height, moderate turbulence, sparks
//   Torch     narrow, fast flicker, leaning one way, a steady base
//   Furnace   strong, fast-changing, a large bright core, many embers
//   Raging    big and aggressive: tall tongues that split and merge often
//   Magic     slower, swirling tongues; the sprite's own (any) colours
enum class FireStyle { Campfire, Torch, Furnace, Raging, Magic };
const char* toString(FireStyle s) noexcept;
std::optional<FireStyle> fireStyleFromString(std::string_view s) noexcept;

struct EffectOptions {
    Effect effect = Effect::Fire;
    FireStyle fire = FireStyle::Campfire;
    int frames = 0;              // loop length; 0 = the effect's own (fire 12-16, the others 8)
    int frameMs = 0;             // 0 = the effect's own (fire 60-90, the others 90)
    double amount = 1.0;         // strength of the motion (0.25..2)
    unsigned seed = 1;           // the noise pattern (same seed, same animation)
    // Which pixels move. Empty = found automatically: warm bright colours
    // for fire, blue/cyan for water, everything above the base for sway,
    // the whole sprite otherwise.
    Mask region;
};

struct EffectClip {
    std::vector<Image> frames;   // all the sprite's size, aligned
    std::vector<int> durations;
    Mask region;                 // the pixels that were animated
};

EffectClip animateEffect(const Image& sprite, const EffectOptions& opt);

// Pixels an effect would animate when no region is given.
Mask autoRegion(const Image& sprite, Effect e);

} // namespace acestudio::fx
