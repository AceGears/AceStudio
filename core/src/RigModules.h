#pragma once
// Internal to SpriteMotion: species motion on the part rig (ADR-ANIM-001
// step 5). Each module cuts the sprite into parts (PartRig.h), poses them
// frame by frame and bakes the frames. Nullopt means the module cannot rig
// this sprite; the caller then falls back to the older renderers.

#include "PixelRig.h"

#include <optional>

namespace acestudio::motion::detail {

// Wing flap and flight (5b): each wing is its own part that turns about its
// shoulder; the far wing is drawn behind the body and trails the near wing,
// the near wing in front; the body heaves against the beat.
std::optional<RigClip> flightRender(const Image& sprite, const Subject& subj, Action action, int frames, double amount,
                                    const WingSpec& wing);

// Swimming (5c): the body bends as a chain of columns (whole pixels, length
// kept), the tail fin beats behind the tail stalk and foreshortens, the
// other fins paddle on their own phases.
std::optional<RigClip> swimRigRender(const Image& sprite, const Subject& subj, Action action, int frames, double amount,
                                     SwimStyle style);

// Walk, run, sprint and idle of bodies with legs (5a): two-bone legs whose
// bones keep their length, a planted foot locked to the ground, rigid arms
// and held items that turn at the shoulder.
std::optional<RigClip> gaitRender(const Image& sprite, const Subject& subj, Body body, Action action, int frames, double amount);

} // namespace acestudio::motion::detail
