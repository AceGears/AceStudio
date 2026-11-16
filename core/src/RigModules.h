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

// The flight rig that flightRender poses, built on its own so a saved rig
// (rigs::autoRig) can use the same cut. `needPair`: refuse a single wing
// unless the wings were painted. Nullopt when no wings are found.
struct WingCut {
    Mask mask;
    rig::Vec shoulder;
    double reach = 0;     // shoulder to the farthest pixel
    int sign = 1;         // +1: the tip sweeps clockwise on the downstroke
    int area = 0;
};
struct FlightWing {
    int part = -1;
    bool nearWing = true;
    WingCut cut;
};
struct FlightRig {
    rig::Rig rig;
    int bodyPart = -1, chestPart = -1;   // chestPart: the head end drawn over the near wing, or -1
    std::vector<WingCut> cuts;
    std::vector<FlightWing> wings;
    std::size_t nearIdx = 0;
    int headSide = 0;                    // +1 head to the right, -1 left, 0 unknown
    int rootRow = 0, skinDepth = 0;
    double skinX0 = 0, skinX1 = 0, bodyCx = 0;
    bool painted = false;
    Mask bodyOnly;
};
std::optional<FlightRig> buildFlightRig(const Image& sprite, const Subject& subj, const WingSpec& spec, bool needPair);

// Swimming (5c): the body bends as a chain of columns (whole pixels, length
// kept), the tail fin beats behind the tail stalk and foreshortens, the
// other fins paddle on their own phases.
std::optional<RigClip> swimRigRender(const Image& sprite, const Subject& subj, Action action, int frames, double amount,
                                     SwimStyle style);

// The swim rig that swimRigRender poses, built on its own so a saved rig
// (rigs::autoRig) can use the same cut: the trunk, the tail fin past the
// tail stalk (none for an eel) and the fins. Nullopt for tiny sprites.
struct SwimFin {
    int part = -1;
    rig::Vec root;
    double u = 0;      // along the body, 0 head .. 1 tail
    bool top = false;
};
struct SwimRig {
    rig::Rig rig;
    int bodyPart = -1, tailPart = -1;
    int stalk = -1;             // columns from the head to the tail stalk, or -1
    rig::Vec stalkPt;           // where the tail fin turns
    std::vector<SwimFin> fins;
    SwimStyle mode = SwimStyle::Auto;
    int dir = 1;                // +1: the head is on the left
    int headX = 0;
};
std::optional<SwimRig> buildSwimRig(const Image& sprite, const Subject& subj, SwimStyle style);

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
