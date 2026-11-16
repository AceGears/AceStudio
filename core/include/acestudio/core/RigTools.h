#pragma once
// Making and using a document's rig (Rig.h, ADR-ANIM-004).
//
// autoRig cuts a sprite layer into part layers with the analysis the sprite
// animator already uses (legs, arms, body), and builds bones and leg IK on
// them. bakePose writes a pose into a frame's part cels with the pixel-art
// renderer of the part rig (whole-pixel moves, RotSprite turns, no new
// colours). Both work on a copy and return it; the caller commits it in one
// undo step (Editor::replaceDocument).

#include "acestudio/core/Document.h"
#include "acestudio/core/Rig.h"

#include <string>
#include <vector>

namespace acestudio::rigs {

struct AutoRigResult {
    Document document;
    LayerId group = kNoLayer;          // the new group holding the part layers
    std::vector<std::string> notes;    // what was found, for the user
};

// Rigs the raster layer `layer` as drawn on `frame`:
//  - its pixels become part layers (body, legs as thigh/shin/foot, arms, a
//    ground shadow) in a new group above it, with the pixels each part hides
//    filled in so parts can move apart; the source layer is hidden, not
//    deleted;
//  - bones: "body" (the root, from the hips up), per leg "legN thigh",
//    "legN shin" and "legN foot", per arm "armN"; a ground shadow stays
//    unbound (it never moves);
//  - an IK chain per leg ("legN IK") whose tip is the shin, so the ankle
//    reaches (short legs drawn as thigh and boot: the foot, so the toe does).
// Fails if the document already has a rig, or the sprite has no legs the
// analysis can find (a blob, a flyer, an object): rig those by hand.
Result<AutoRigResult> autoRig(const Document& doc, LayerId layer, int frame);

// Draws every bound part layer's cel of `restFrame` (the parts as drawn at
// rest) under `pose` into its cel on `frame` (which may be the same frame).
// Each part turns about its bone's rest head and moves with it; a part with
// no cel on the rest frame has none on `frame` either. Unbound layers are
// untouched, and a cel of `frame` linked to other frames is unlinked first.
// An empty pose copies the rest cels exactly.
Result<Document> bakePose(const Document& doc, int restFrame, int frame, const RigPose& pose);

// Bakes the rig's keys into frames [first, last] (each frame gets
// poseAt(frame), drawn from the rig's rest frame). The rest frame itself is
// skipped, so the parts at rest are never overwritten. With first > last,
// every frame from the first key to the last is baked. Fails when there
// are no keys.
Result<Document> bakeKeys(const Document& doc, int first = 0, int last = -1);

} // namespace acestudio::rigs
