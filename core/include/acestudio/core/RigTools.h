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

#include <optional>
#include <string>
#include <vector>

namespace acestudio::rigs {

struct AutoRigResult {
    Document document;
    LayerId group = kNoLayer;          // the new group holding the part layers
    std::vector<std::string> notes;    // what was found, for the user
};

// What auto-rig cuts. Auto: legs when the analysis finds them, else wings
// for a sprite it reads as a flyer and fins for a swimmer. The others force
// one cut (a winged creature that also has legs reads as legged: ask for
// Wings to rig its wings).
enum class AutoRigBody { Auto, Legs, Wings, Fins };
const char* toString(AutoRigBody b);
std::optional<AutoRigBody> autoRigBodyFromString(const std::string& s);

// Rigs the raster layer `layer` as drawn on `frame`. Its pixels become part
// layers in a new group above it, with the pixels each part hides filled in
// so parts can move apart; the source layer is hidden, not deleted. Then:
//  - legs: parts body, thigh/shin/foot per leg, arms, a ground shadow; bones
//    "body" (the root, from the hips up), per leg "legN thigh", "legN shin"
//    and "legN foot", per arm "armN" (a shadow stays unbound); an IK chain
//    per leg ("legN IK") whose tip is the shin, so the ankle reaches (short
//    legs drawn as thigh and boot: the foot, so the toe does);
//  - wings (the flap animator's cut below the wing line): "body" from the
//    body's middle towards the head, "near wing" and "far wing" (or "wing")
//    from each shoulder to the wing tip; legs, head and tail stay in the body;
//  - fins (the swim animator's cut): "body" from the tail stalk to the head,
//    "tail fin" past the stalk (an eel has none), and "dorsal fin" /
//    "ventral fin" per other fin. The body is one rigid part.
// Fails if the document already has a rig, or the cut finds nothing (no
// legs, wings or fins): rig those by hand.
Result<AutoRigResult> autoRig(const Document& doc, LayerId layer, int frame, AutoRigBody body = AutoRigBody::Auto);

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
