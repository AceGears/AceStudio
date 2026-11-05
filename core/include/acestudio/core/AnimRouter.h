#pragma once
// Chooses how to animate a sprite: with an environmental effect (fire,
// water, plants, flags, smoke, magic: procedural, no AI), with body motion
// (the rig in SpriteMotion: legs, wings, a swimmer's wave), or with the AI
// pipeline (new poses that cannot be made by moving the drawn parts).
//
// The name or description of the asset is the strongest hint ("campfire",
// "oak tree"); without one the pixels decide (a sprite made mostly of warm
// flame colours is fire; anything with feet, wings or fins gets body
// motion). The user can always pick another method.

#include "acestudio/core/Effects.h"
#include "acestudio/core/Image.h"
#include "acestudio/core/SpriteMotion.h"

#include <string>
#include <string_view>

namespace acestudio::anim {

enum class Method { Body, Effect, Ai };
const char* toString(Method m) noexcept;

struct Route {
    Method method = Method::Body;
    fx::Effect effect = fx::Effect::Pulse;   // when method == Effect
    fx::FireStyle fireStyle = fx::FireStyle::Campfire;   // when effect == Fire
    motion::Body body = motion::Body::Object;
    std::string reason;                      // one sentence for the user
};

// `hint`: the asset's name or description (any case), may be empty.
// `action`: what the user wants (Idle by default); poses the rig cannot make
// (a turn, a weapon swing drawn differently) route to the AI pipeline.
Route routeAnimation(const Image& sprite, std::string_view hint, motion::Action action = motion::Action::Idle);

} // namespace acestudio::anim
