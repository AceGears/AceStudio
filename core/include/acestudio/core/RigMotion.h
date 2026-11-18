#pragma once
// A 3D motion (BVH, glTF/GLB or a motion-library preset) applied to a
// document's rig as pose keys (ADR-ANIM-002 section 3.3, ADR-ANIM-004).
//
// Retarget (Mocap.h) cuts a flat sprite into parts each time and returns
// pictures. This works on a rig the user has made or auto-rigged and kept:
// each sampled frame of the motion becomes a key on the rig, which the user
// can then edit like any other key and bake (rigs::bakeKeys).
//
// For every frame the motion is projected by a camera, then:
//  - each rig bone matched to a stretch of the skeleton (say "leg1 shin" to
//    shin -> ankle) turns so it points the way that stretch points on screen;
//    a stretch seen end-on keeps the previous frame's direction;
//  - an IK chain whose two bones are both matched puts its tip where the
//    skeleton's does, measured from the chain's top joint and scaled to the
//    rig's bone lengths (the rig keeps its proportions), never below the
//    rig's ground (the lowest leg bone at rest);
//  - the root moves as the hips rise and sink (and travel, unless in place).
//
// Rig bones are matched by name: auto-rig names ("body", "head",
// "legN thigh|shin|foot", "armN"), the joint names of Mocap.h ("LThigh",
// "RForearm", ... case ignored), or explicit `bones` pairs. Wings and fins
// match nothing; they are refused.

#include "acestudio/core/Document.h"
#include "acestudio/core/Mocap.h"
#include "acestudio/core/Rig.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace acestudio::rigs {

struct MotionKeysOptions {
    // Nullopt = drawnCamera(rig).
    std::optional<mocap::Camera> camera;
    double start = 0;            // seconds into the motion
    double length = 0;           // seconds; 0 = to the end
    int fps = 12;                // keys per second of motion (1..60)
    int frames = 0;              // > 0: exactly this many keys (2..240)
    bool inPlace = true;         // remove the root's travel
    bool loop = true;            // the last key is one step before the first repeats
    double amount = 1;           // scales turns and moves (0.25..2)
    int first = -1;              // the first keyed frame; -1 = the one after the rest frame
    // Rig bone name -> skeleton joint, winning over the name match. A bone
    // paired with Hips points along the spine (hips -> neck).
    std::vector<std::pair<std::string, mocap::Bone>> bones;
    // Skeleton joint -> joint name in the motion (as retarget's --map).
    std::vector<std::pair<mocap::Bone, std::string>> mapOverrides;
};

struct MotionKeysResult {
    Document document;           // the rig with the new keys; frames added at the end as needed
    int first = 0, count = 0;    // keyed frames [first, first + count)
    int framesAdded = 0;
    mocap::Camera camera;
    // Rig bone -> what drives it ("LThigh -> LShin", "IK leg1 IK").
    std::vector<std::pair<std::string, std::string>> matched;
    std::vector<std::string> notes;
};

// The view the rig is drawn in: front (turn 0) when its first two numbered
// thighs stand apart, else from the side its feet point to (turn 90 = the
// character faces screen right, -90 = left; right when it cannot tell).
mocap::Camera drawnCamera(const Rig& rig);

// Fails when the document has no rig, the motion's hips are not found, no
// rig bone matches, or the keyed frames would cover the rest frame. Keys
// already on the keyed frames are replaced; others stay.
Result<MotionKeysResult> keysFromMotion(const Document& doc, const mocap::Motion& motion, const MotionKeysOptions& opt = {});

} // namespace acestudio::rigs
