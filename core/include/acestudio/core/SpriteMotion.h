#pragma once
// Animating an existing sprite without AI.
//
// The sprite is split into parts measured from its silhouette (body, head,
// and the leg groups found at its feet), and each frame moves the parts with
// rigid transforms: leg swing about the hip, body bob, squash and stretch,
// lean, rotation. Pixels are sampled nearest-neighbour, so a frame contains
// only the sprite's own colours (the hurt flash is the one exception), and
// every frame of a clip is cropped to one shared box so the ground line stays
// put.
//
// The motion adapts to the subject: two feet walk, four feet trot in diagonal
// pairs, a footless blob hops, a flyer hovers, an object rocks. It is a quick
// and predictable starting point; for new poses (a turn, an arm swing drawn
// differently) the AI pipeline generates the frames instead.

#include "acestudio/core/Image.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace acestudio::motion {

enum class Action {
    Idle, Walk, Run, Sprint, Jump, Fall, Land, Attack1, Attack2, Attack3, Dodge, Roll, Dash, Block, Hurt, Death,
    Cast, Swim, Fly, Flap, Climb, Sit, Sleep, Custom
};

struct ActionInfo {
    Action action;
    const char* key;
    const char* label;
    int frames;       // default frame count
    int frameMs;      // default frame duration
    bool loops;
};
const std::vector<ActionInfo>& actions();
const ActionInfo& actionInfo(Action a);
std::optional<Action> actionFromKey(std::string_view key);

enum class Body { Biped, Quadruped, Blob, Flyer, Object };
const char* toString(Body b);
std::optional<Body> bodyFromString(std::string_view s);

// What the silhouette says about the subject.
struct Subject {
    Body body = Body::Object;
    IntRect bounds;              // opaque bounds in the sprite
    int legTop = -1;             // first row of the legs (sprite coordinates), -1 = no legs
    std::vector<std::pair<int, int>> feet;   // [x0, x1] column runs of the feet, left to right
    int footRow = -1;            // lowest row of the feet (above any ground shadow)
    int groundTop = -1;          // first row of a ground shadow / base line, -1 = none
};
Subject analyzeSubject(const Image& sprite);

// Custom motion: each value is the peak of a sine over the cycle.
struct CustomMotion {
    double bob = 0.05;          // up/down, fraction of the height
    double sway = 0.0;          // side to side, fraction of the width
    double squash = 0.05;       // vertical squash and stretch
    double tilt = 0.0;          // rotation, radians
    double legSwing = 0.3;      // radians
};

struct Options {
    Action action = Action::Idle;
    int frames = 0;             // 0 = the action's default
    double amount = 1.0;        // scales every movement (0.25..2)
    std::optional<Body> body;   // overrides the measured body
    bool faceLeft = false;      // mirror the result
    // Wing flap: everything above this line (fraction of the sprite's height
    // from the top) is a wing that folds about the line.
    double wingLine = 0.45;
    CustomMotion custom;
};

struct Clip {
    std::string name;
    std::vector<Image> frames;
    std::vector<int> durations;  // ms
    bool loops = true;
    Body body = Body::Object;
};

// Animates a sprite (transparent background expected). Errors when the
// sprite has no opaque pixels.
Result<Clip> animate(const Image& sprite, const Options& opt);

// Several actions at once, all cropped to one shared frame size so the clips
// line up in a sprite sheet.
Result<std::vector<Clip>> animatePack(const Image& sprite, const std::vector<Action>& actions, const Options& base);

// Automatic checks of a clip: pixel-count drift (parts lost or doubled),
// frame-to-frame jumps, and the loop seam.
struct ClipCheck {
    double maxAreaChange = 0;   // largest relative change of opaque area vs frame 0
    double minNeighbourIoU = 1; // lowest silhouette overlap of consecutive frames
    double loopIoU = 1;         // last frame vs first (looping clips)
    std::vector<std::string> issues;
};
ClipCheck checkClip(const Clip& clip);

} // namespace acestudio::motion
