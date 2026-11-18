#pragma once

// Direction sets (ADR-ANIM-002 stage 4): a character drawn from several
// views (front, back, side, three-quarter), and a motion played in every
// direction a game needs by picking, for each camera turn, the drawn view
// closest to it.
//
// The art is never turned or re-drawn here: a direction gets the pixels of
// the nearest drawn view (or its left-right mirror), and the motion is seen
// from that direction's camera. A direction far from every drawn view is
// flagged, as retarget does, so the missing view can be drawn.

#include "acestudio/core/Document.h"
#include "acestudio/core/Image.h"
#include "acestudio/core/Mocap.h"
#include "acestudio/core/Result.h"
#include "acestudio/core/SpriteMotion.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace acestudio::directions {

// Turns are in degrees, as mocap::Camera::turn: 0 = the character faces the
// viewer, 90 = it faces screen right, 180 = it faces away, -90 = screen left.
// They are kept in (-180, 180].

// One drawn view of the character.
struct DrawnView {
    std::string name;   // for messages and output (e.g. "s", "front", "se")
    double turn = 0;
    Image art;
};

// View names: compass keys as the Character Pack writes them (s, se, e, ne,
// n, nw, w, sw; "s" faces the viewer, "e" faces screen right) or words
// (front, front-right, right, back-right, back, back-left, left, front-left).
// Case does not matter. Nullopt for any other name.
std::optional<double> viewTurn(std::string_view name);
// The compass key of the nearest of the eight directions.
std::string compassName(double turn);
// The compass key when the turn is within 1 degree of one, else "turnN"
// (N the whole degrees, e.g. "turn30", "turn-60").
std::string directionName(double turn);
// Turns in (-180, 180].
double wrapTurn(double turn);

// Every view has art with visible pixels, a finite turn and a name; no two
// views are within 1 degree of each other; at most kMaxViews views.
inline constexpr int kMaxViews = 16;
Status checkViews(const std::vector<DrawnView>& views);

struct ViewChoice {
    int view = -1;          // index into the views
    bool mirrored = false;  // the view's art flipped left-right
    double turn = 0;        // the turn the chosen art stands for (mirrored: -view turn)
    double offBy = 0;       // degrees between that turn and the one asked for
};
// The drawn view nearest to `turn`. A view's mirror stands for the opposite
// side (a right-facing view mirrored faces left); when a drawn view and a
// mirror are equally near, the drawn view wins. Mirrors swap the character's
// left and right, which a one-handed weapon or a side parting shows: draw
// that side's view to avoid it, or pass allowMirror = false.
ViewChoice chooseView(const std::vector<DrawnView>& views, double turn, bool allowMirror = true);
// The art of a choice (mirrored when it says so).
Image choiceArt(const std::vector<DrawnView>& views, const ViewChoice& choice);

// The eight compass directions (s, se, e, ne, n, nw, w, sw) or the four
// (s, e, n, w), as turns.
std::vector<double> compassTurns(int count);

struct DirectionOptions {
    std::vector<double> turns;              // the directions to make (degrees)
    mocap::RetargetOptions retarget;        // timing, amount, map, tilt...; the camera's
                                            // turn is set per direction
    bool allowMirror = true;
};

struct DirectionClip {
    std::string direction;    // directionName(turn)
    double turn = 0;
    ViewChoice choice;
    std::string viewName;     // the chosen view's name (with " (mirrored)")
    bool approximateView = false;   // more than 30 degrees from the chosen view, or tilted over 40
    mocap::RetargetResult result;
};

// The motion retargeted once per direction onto the chosen view's art. Fails
// when the views are invalid, a turn is not finite or repeats a direction, or
// a retarget fails (the message names the direction and view).
Result<std::vector<DirectionClip>> retargetDirections(const std::vector<DrawnView>& views, const mocap::Motion& motion,
                                                     const DirectionOptions& opt);

// A turn on the spot from one turn to another, the short way round, in
// `stepDeg` steps: each step shows the nearest drawn view, a step that would
// show the same art as the one before is left out, and every frame is held
// `frameMs`. Frames share one size, the art centred and standing on the same
// row. This cuts between drawn views; nothing in between is drawn.
Result<motion::Clip> turnClip(const std::vector<DrawnView>& views, double fromTurn, double toTurn, double stepDeg = 45,
                              int frameMs = 100, bool allowMirror = true);

// The clips on one document: one frame per clip frame on a layer named
// "Directions", the art centred and standing on one row, one tag per clip
// named "<prefix>_<direction>" (or the clip's name when prefix is empty, for
// a turn clip), tags looping or playing once as the clips do.
struct NamedClip {
    std::string tag;
    const motion::Clip* clip = nullptr;
};
Result<Document> clipsDocument(const std::vector<NamedClip>& clips);

} // namespace acestudio::directions
