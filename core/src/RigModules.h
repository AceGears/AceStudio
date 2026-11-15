#pragma once
// Internal to SpriteMotion: species motion on the part rig (ADR-ANIM-001
// step 5). Each module cuts the sprite into parts (PartRig.h), poses them
// frame by frame and bakes the frames. Nullopt means the module cannot rig
// this sprite; the caller then falls back to the older renderers.

#include "PartRig.h"
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

// The legged rig that gaitRender poses, built on its own so other motion
// sources (retargeted 3D motion) can pose it too. Nullopt when the body has
// no legs.
struct GaitLeg {
    int thigh = -1, shin = -1, foot = -1;
    rig::Vec hip, knee, ankle;   // rest joints
    double l1 = 1, l2 = 1;
    double phase = 0;
    int ground = 0;              // the row its foot stands on (lowest row of the foot)
    bool hind = false;           // a four-legged animal's hind leg: the hock bends back
    bool nearLeg = false;
};
struct GaitRig {
    rig::Rig rig;                // segmented, inpainted and prepared
    std::vector<GaitLeg> legs;
    std::vector<int> armParts;   // same order as Subject::arms
    int bodyPart = -1, shoulderPart = -1, pelvisPart = -1;
    bool front = false, stubby = false, twist = false;
    int dir = 1;                 // +1 faces right, -1 left
};
std::optional<GaitRig> buildGaitRig(const Image& sprite, const Subject& subj, Body body);

} // namespace acestudio::motion::detail
