#pragma once
// AI key poses: motion that needs new drawings (a wing seen at another
// angle, the far wing, a turn) cannot be made by moving the pixels of one
// sprite. An image model draws the key poses from the sprite (the reference
// keeps its identity); this module plans those poses, aligns each one to the
// sprite, scores how consistent it is, and assembles the clip.
//
// Every pose is first reduced to the sprite's size and exact palette by the
// AI worker (pixel post-process), so no new colours appear.

#include "acestudio/core/Image.h"
#include "acestudio/core/SpriteMotion.h"

#include <string>
#include <vector>

namespace acestudio::keypose {

struct PoseSpec {
    std::string key;          // "level", "down", ...
    std::string description;  // what the image model is asked to draw (appended to the subject)
    int durationMs = 100;     // how long the pose shows in the clip
    bool original = false;    // the sprite itself (no generation)
};

struct Plan {
    std::string name;             // clip name, e.g. "flap"
    std::vector<PoseSpec> poses;  // in clip order; the cycle loops back to the first
    // Rows (fractions of the height, from the top) that should not move
    // between poses: what each pose is aligned on (a dragon's legs and tail).
    double anchorFrom = 0.6, anchorTo = 1.0;
};

// The plan for an action, or an empty plan when AI poses are not used for it.
Plan planFor(motion::Action action, motion::Body body);

// Inputs for redrawing one pose by inpainting (FLUX.2 klein): the sprite
// scaled up by whole pixels onto a white canvas, and a mask that protects
// the body (white = repaint, black = keep). Only the wings (and the space
// around the body where wings can go, including behind it) are redrawn, so
// the body keeps its exact pixels, place and scale in every pose.
struct InpaintInputs {
    Image init;    // RGB, white background
    Image mask;    // white = repaint
    int scale = 1;
};
InpaintInputs inpaintInputs(const Image& sprite, const Mask& wings, int targetSize = 512);

// A generated pose (the model's full-size output on the inpainting canvas,
// background already cut away) brought back onto the sprite's own pixel
// grid: each sprite pixel takes the colour at the centre of its block, and
// every colour is snapped to the nearest colour of the sprite.
Image reduceToSprite(const Image& raw, const InpaintInputs& in, const Image& sprite);

// The wing pixels of a sprite as the flap uses them: everything above the
// wing line (fraction of the height) between the span fractions, or `mask`.
Mask wingMask(const Image& sprite, double line, double from, double to, const Mask& mask = {});

// The prompt for one pose: the subject (the asset's name or description)
// redrawn in that pose, identical in look to the reference.
std::string posePrompt(const std::string& subject, const PoseSpec& pose);

struct Aligned {
    Image image;          // the pose, moved onto the sprite's canvas
    int dx = 0, dy = 0;   // the move that was applied
    // 0..1: how well the anchor rows match the sprite after the move, and
    // how close the pose's colours and size are to the sprite's (identity).
    double anchorMatch = 0, identity = 0;
    std::vector<std::string> issues;
};

// Moves `pose` (same size as `original`) to the offset where its anchor rows
// best match the sprite's (searching +-`range` px), and scores it.
Aligned align(const Image& pose, const Image& original, const Plan& plan, int range = 8);

// Puts the sprite's own body pixels back into a generated pose (same canvas)
// wherever the pose shows the body re-rendered with a slightly different
// colour or is empty there: the body stays pixel-identical across poses. A
// pixel the pose changed a lot (a wing drawn in front of the body) is kept.
// Returns how many pixels were restored.
int restoreBody(Image& pose, const Image& original, const Mask& wings);

// The better of several variants of one pose (by anchor match and identity).
std::size_t best(const std::vector<Aligned>& variants);

// The clip: the poses in order with their durations, every frame on one
// canvas (the sprite's size plus room for wings that reach outside it).
motion::Clip assemble(const Plan& plan, const Image& original, const std::vector<Aligned>& generated);

} // namespace acestudio::keypose
