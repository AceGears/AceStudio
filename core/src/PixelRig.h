#pragma once
// Internal to SpriteMotion: walk, run, sprint and idle of bodies with legs,
// made only of whole-pixel moves of the drawn parts.
//
// The previous renderer rotated and scaled parts with nearest-neighbour
// sampling. On generated sprites from the test PC that redrew 29-52 % of
// the leg pixels every frame (boots changed shape, outlines broke, strays
// appeared) and the idle squash dropped and doubled rows at random. Hand-made
// pixel animation moves pixels, it does not resample them; so does this:
//
//  - every pixel is copied, never interpolated: a part moves by whole
//    pixels, a leg bends by shifting each of its rows sideways (thigh and
//    shin separately, so there is a knee) and gets shorter or longer by
//    dropping or doubling rows in the thigh and shin; the foot itself is
//    only ever moved;
//  - a walk follows the classic key poses: contact, down, passing, up. The
//    standing foot moves back at constant speed (no sliding), the swinging
//    foot travels forward on an eased arc with the knee pushed forward, the
//    body is lowest just after contact and highest at the up pose, the head
//    follows a frame later;
//  - arms beside the body swing against the legs;
//  - an idle breathes: everything above the waist rises a pixel and settles,
//    the head a frame later; the legs never move.

#include "acestudio/core/SpriteMotion.h"

#include <optional>

namespace acestudio::motion::detail {

struct RigClip {
    std::vector<Image> frames;   // each `box` sized
    IntRect box;                 // in sprite coordinates
};

// The wings for a flap (see Options): above `line` (fraction of the height)
// between `from` and `to` (fractions of the width), or exactly `mask`.
struct WingSpec {
    double line = 0.45, from = 0.0, to = 1.0;
    const Mask* mask = nullptr;
};

// Nullopt when the rig does not handle this body and action.
std::optional<RigClip> rigRender(const Image& sprite, const Subject& subj, Body body, Action action, int frames, double amount,
                                 const WingSpec& wing = {});

// Wing flap of any body: the wings are copied row by row (never resampled),
// folding about their root with a fast downstroke, a slower recovery and the
// tips trailing the root; the body rises on the downstroke.
RigClip flapRender(const Image& sprite, const Subject& subj, int frames, double amount, const WingSpec& wing);

} // namespace acestudio::motion::detail
