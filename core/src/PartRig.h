#pragma once
// Internal to SpriteMotion: the part rig (ADR-ANIM-001 steps 1-4).
//
// A sprite is cut into parts: separate layers that each keep the pixels other
// parts hide (filled in by `inpaintHidden`), a pivot (the joint it turns
// about) and a depth. A pose moves, turns and foreshortens each part, and
// `renderPose` composites them back to front. Species modules (RigFlight,
// RigSwim, RigGait) only produce poses; they never write pixels.
//
// Rendering stays pixel-art correct:
//  - a part that only moves is copied pixel for pixel, by whole pixels;
//  - a part that turns or is foreshortened is sampled nearest-neighbour from
//    an 8x Scale2x copy of itself (RotSprite), so edges stay crisp and no
//    colour is made up;
//  - alpha stays binary.
//
// The rig is built from the sprite every time a clip is made and the poses
// are baked into frames, so projects store nothing new (no schema change).

#include "acestudio/core/Image.h"
#include "acestudio/core/Raster.h"

#include <limits>
#include <string>
#include <vector>

namespace acestudio::motion::rig {

struct Vec {
    double x = 0, y = 0;
};

// Scale2x (EPX): doubles an image, rounding diagonal edges without new colours.
Image scale2x(const Image& in);

// ---- Masks (all sprite sized) ------------------------------------------------

Mask opaqueMask(const Image& sprite);
int count(const Mask& m);
IntRect bounds(const Mask& m);
Mask dilate(const Mask& m, int r);          // square neighbourhood
Mask erode(const Mask& m, int r);
Mask openMask(const Mask& m, int r);        // erode then dilate: thin parts go
Mask andMask(const Mask& a, const Mask& b);
Mask minusMask(const Mask& a, const Mask& b);
Mask orMask(const Mask& a, const Mask& b);
// 8-connected pieces, largest first.
std::vector<Mask> components(const Mask& m);

// ---- Parts and poses ------------------------------------------------------------

enum class Side { Far, Centre, Near };

struct Part {
    std::string name;
    Mask own;            // pixels of the source that belong to this part
    Image pixels;        // sprite sized: the part's pixels, plus the hidden fill
    Vec pivot;           // joint, sprite coordinates (pixel corners)
    int z = 0;           // drawn back (low) to front (high)
    Side side = Side::Centre;
    // Built by prepare(): the 8x Scale2x copy of `pixels` inside hiBox.
    IntRect hiBox;
    Image hi;
};

struct PartPose {
    double dx = 0, dy = 0;   // the pivot moves by this (rounded to whole pixels)
    double angle = 0;        // radians, clockwise on screen, about the pivot
    double sx = 1, sy = 1;   // foreshortening about the pivot, before turning
    int z = 0;               // depth this frame
    bool visible = true;
    // A part bent along its length: column k of the part (from its box's left
    // edge) moves down by colDy[k] pixels (whole pixels, before the pose's
    // own move). Empty = rigid.
    std::vector<int> colDy;
    // A part that curls: the turn grows from `angle` at the pivot to
    // `angle + bend` at `bendRadius` pixels from it and beyond (a wing whose
    // hand trails its arm). The part stays one piece: no seam opens.
    double bend = 0, bendRadius = 0;
    // Skinning to the body: below row `skinTop` (sprite coordinates) the turn
    // fades out over `skinDepth` rows, so the root of a wing blends into the
    // back it grows from (the back stays, the wing turns). 0 = off.
    double skinTop = 0, skinDepth = 0;
    // ... and, below skinTop, sideways too: full between skinX0 and skinX1,
    // fading to nothing over skinFade columns outside them (0 = no limit).
    double skinX0 = 0, skinX1 = 0, skinFade = 0;
    // Nothing of the part is drawn below this row (sprite coordinates, after
    // the pose): a turned shin never pokes through the ground its foot is on.
    int maxY = std::numeric_limits<int>::max();
};

struct Rig {
    int width = 0, height = 0;
    std::vector<Part> parts;
    // Rest pose: every part where it is drawn.
    std::vector<PartPose> rest() const;
    int find(const std::string& name) const;   // -1 if missing
};

// Adds a part made of the source pixels in `own` (which are taken out of
// every part added before it: the newest claim wins).
int addPart(Rig& rig, const Image& sprite, const std::string& name, const Mask& own, Vec pivot, int z,
            Side side = Side::Centre);

// Fills, for every part, the pixels that parts in front of it cover where the
// part plausibly continues underneath: between its own pixels along a row
// and along a column (at most `maxGap` pixels apart). The fill copies the
// part's own nearest inner colour; the part's outline colour closes the edge.
void inpaintHidden(Rig& rig, int maxGap);

// Builds the up-scaled copies used for turned and foreshortened parts.
void prepare(Rig& rig);

// Composites the parts (by pose z, then rig order) into an image of `box`
// (sprite coordinates).
Image renderPose(const Rig& rig, const std::vector<PartPose>& pose, IntRect box);

// Where the source point `p` of part `i` lands under `pose` (for joints).
Vec transformPoint(const Rig& rig, int i, const PartPose& pose, Vec p);

// Two-bone IK in the screen plane: from `root` with bones of length l1, l2 to
// reach `target`. The joint bends to the side `bend` (+1: the joint lies to
// the right of root->target, -1: to the left). Unreachable targets are
// clamped onto the reachable circle. Returns the joint position.
Vec solveTwoBone(Vec root, Vec target, double l1, double l2, int bend);

// Pairs of parts whose depth order flips between frame `frame` and the next
// while they overlap in both: one part pops through the other. A swap is
// only allowed where the parts are apart in one of the two frames (the
// geometry crosses). Empty = no pops.
struct ZSwap {
    int frame = 0, a = 0, b = 0;
};
std::vector<ZSwap> zPops(const Rig& rig, const std::vector<std::vector<PartPose>>& poses, IntRect box);

} // namespace acestudio::motion::rig
