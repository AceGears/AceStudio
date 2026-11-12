#pragma once
// A walk from drawn poses.
//
// A sheet of drawn walk poses (generated, or drawn by hand) usually cannot
// be played as it is: every pose is a drawing of its own, so the head, the
// body and its details change from frame to frame, the poses are not on one
// ground line, and their order is rarely a cycle. But the poses hold what a
// procedural rig cannot make: legs drawn in different shapes.
//
// stabilizeCycle keeps the drawn legs and takes everything else from one
// pose: the head and upper body of the "master" pose are used in every
// frame, each pose's legs are put under it on one ground line, and the poses
// are ordered into a cycle (neutral, step, neutral, other step). This is how
// classic RPG character sprites are built.

#include "acestudio/core/Image.h"
#include "acestudio/core/Result.h"
#include "acestudio/core/SpriteMotion.h"

#include <string>
#include <vector>

namespace acestudio::motion {

enum class CycleView { Auto, Front, Side };

struct CycleOptions {
    // Seen from the front or back (the feet lift in place) or from the side
    // (the feet stride apart). Auto tells them apart by how far the feet spread.
    CycleView view = CycleView::Auto;
    // The pose (0-based) whose head and upper body every frame uses; -1 = the
    // pose most like the others.
    int master = -1;
    // The poses (0-based) in play order; empty = worked out from the poses.
    std::vector<int> order;
    int frameMs = 150;
    // The upper body rises and sinks by a pixel or two, as the poses were drawn.
    bool bob = true;
};

struct CycleResult {
    Clip clip;                       // name "walk", looping
    int master = 0;
    std::vector<int> order;          // the poses used, in play order
    bool frontView = false;
    // What could not be done well (a single step pose, legs not found, ...).
    std::vector<std::string> notes;
};

// `poses`: two or more poses of one character facing one way, each on a
// transparent background (any sizes). Errors when fewer than two have
// visible pixels, or an option names a pose that does not exist.
Result<CycleResult> stabilizeCycle(const std::vector<Image>& poses, const CycleOptions& opt = {});

} // namespace acestudio::motion
