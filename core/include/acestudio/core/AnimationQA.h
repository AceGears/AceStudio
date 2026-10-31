#pragma once

// Consistency checks and automatic corrections for generated animation
// frames (Phase 5: generate -> evaluate -> correct -> commit).
//
// Every frame is compared with a reference (the character's look) and its
// neighbours:
//   ground line   the lowest opaque row should stay put (feet don't float)
//   height        the figure's height should match the reference
//   palette drift how far the frame's colours are from the profile palette,
//                 and how different its colour mix is from the reference
//   silhouette    overlap with the neighbouring frames (bottom-centre aligned):
//                 a frame that matches neither neighbour was drawn as something else
// Corrections fix what can be fixed exactly (ground, height, centring,
// palette); frames whose silhouette jumps are flagged for regeneration.

#include "acestudio/core/Image.h"
#include "acestudio/core/Palette.h"
#include "acestudio/core/Pose.h"
#include "acestudio/core/Raster.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace acestudio {

// Makes the background transparent: pixels connected to the image border whose
// colour is within `tolerance` (max channel difference) of the median border
// colour. Interior pixels of the same colour (enclosed by the subject) stay.
Image removeBorderBackground(const Image& src, int tolerance = 24);

// Removes opaque parts that lie entirely below the largest one (a floor line
// or shadow drawn under an airborne character). Parts beside or above it (a
// detached sword) are kept.
Image removeDetachedBelow(const Image& src);

// Alpha > 0 pixels.
Mask silhouette(const Image& img);

// Agreement (-1..1) between where colours sit left/right of the figure's
// centre in `img` and in `reference`: +1 = same facing, -1 = mirror image.
// Returns 1 when the reference is too symmetric to tell.
double facingAgreement(const Image& img, const Image& reference);

// Intersection over union of two silhouettes after aligning their bounding
// boxes' bottom-centre points (so a figure that only moved is unchanged).
// 1.0 when both are empty.
double silhouetteIoU(const Mask& a, const Mask& b);

// Mean distance (0..1, RGB, max-channel metric) from each opaque pixel to its
// nearest palette colour. 0 when every pixel is a palette colour.
double paletteDistance(const Image& img, const Palette& palette);

// Half the L1 distance between the two images' palette-index histograms (each
// opaque pixel counted at its nearest palette colour): 0 = same colour mix, 1 = disjoint.
double colorMixDistance(const Image& a, const Image& b, const Palette& palette);

// What a frame should look like relative to the reference, from its pose: a
// jump leaves the ground, a crouch is shorter, a stride's bounds are lower.
struct FrameExpectation {
    int groundOffset = 0;      // px, + = lower than the reference ground (negative = airborne)
    double heightRatio = 1.0;  // expected height / reference height
    // How much this frame's pose overlaps the previous / next frame's pose
    // (thick skeletons, bottom-centre aligned). Big motions (a jump, a run)
    // overlap less, so their frames are allowed to as well.
    double poseOverlapPrev = 1.0, poseOverlapNext = 1.0;
};

// Expectations for frames generated from `poses` and cropped with
// `box` (normalised x0, y0, x1, y1, as from poseFramingBox) to `spriteHeight`
// px. Frame 0 is the reference.
std::vector<FrameExpectation> expectationsFromPoses(const std::vector<Pose>& poses, const std::array<double, 4>& box,
                                                    int spriteHeight);

// Perceptual colour-profile distance (0 = same, 1 = disjoint): half the L1
// distance between histograms of opaque pixels over fixed bins (12 hues x 3
// lightness levels for coloured pixels, 4 grey levels). Independent of any
// palette and of image size. Calibrated on real SDXL frames of one character
// (<= 0.31 to each other) against other subjects and recoloured copies
// (>= 0.41).
double colourProfileDistance(const Image& a, const Image& b);

struct QAOptions {
    int groundTolerance = 1;          // px
    double heightTolerance = 0.08;    // relative to the reference height
    double paletteTolerance = 0.06;   // paletteDistance
    double colorMixTolerance = 0.36;  // colourProfileDistance vs the reference frame
    double minIoU = 0.55;             // silhouette overlap with the closer-matching neighbour frame...
    double poseOverlapFactor = 0.8;   // ...or this share of the poses' own overlap, if lower
    bool cyclic = true;               // the last frame leads into frame 1 (walk, run, idle; not a jump)
    double flipThreshold = -0.35;     // facing agreement below this = drawn facing the wrong way
    std::vector<FrameExpectation> expected;   // per frame; empty = every frame stands like the reference
    // The character's reference images: every frame (frame 1 included) is
    // compared with them for colour mix, so a frame 1 that is a different
    // character is caught. Empty = no identity check.
    std::vector<Image> identity;
    double identityTolerance = 0.38;          // colourProfileDistance to the closest reference image
};

enum QAFlag : std::uint32_t {
    QAGround = 1u << 0,
    QAHeight = 1u << 1,
    QAPalette = 1u << 2,
    QAColorMix = 1u << 3,
    QASilhouette = 1u << 4,
    QAEmpty = 1u << 5,
    QAFlipped = 1u << 6,   // drawn facing the other way (the mirror image matches better)
    QAOffModel = 1u << 7,  // colours do not match the character's reference images
};

struct FrameQA {
    IntRect bounds;                  // opaque bounds (empty if nothing drawn)
    int groundOffset = 0;            // frame ground - expected ground (px, + = lower)
    double heightRatio = 1.0;        // frame height / expected height
    FrameExpectation expected;       // what the pose asked for
    double identity = 0.0;           // colourProfileDistance to the closest reference image (0 if none)
    double facing = 1.0;             // -1..1: how the left/right placement of colours agrees with the
                                     // reference (+1 same facing, -1 mirrored; 1 when undecidable)
    double paletteDistance = 0.0;
    double colorMix = 0.0;           // colourProfileDistance vs the reference frame
    double iouPrevious = 1.0;        // best overlap with a neighbouring frame (1 for the first)
    std::uint32_t flags = 0;
    double score = 1.0;              // 0..1, 1 = consistent
    bool ok() const noexcept { return flags == 0; }
    // Flags that corrections cannot fix: regenerate the frame.
    bool needsRegeneration() const noexcept { return (flags & (QASilhouette | QAEmpty | QAColorMix | QAOffModel)) != 0; }
};

struct QAReport {
    std::vector<FrameQA> frames;
    int referenceGround = 0;
    int referenceHeight = 0;
    int referenceCentreX = 0;
    double meanScore() const;
    std::vector<int> failing() const;          // frames with any flag
    std::vector<int> toRegenerate() const;     // frames corrections cannot fix
};

// `reference` sets the expected ground line, height, centre and colour mix;
// all frames must have the same size. The colour checks do not need a
// palette; `palette` (may be empty) is only used for the off-palette check.
QAReport evaluateFrames(const std::vector<Image>& frames, const Image& reference, const Palette& palette,
                        const QAOptions& opts = {});
std::string describe(const FrameQA& qa);

// Fixes the ground line and height (to what the frame's pose expects;
// nearest-neighbour scale about the feet), facing (mirrors flipped frames) and
// off-palette colours; each frame keeps its horizontal position. Frames that need regeneration are returned
// unchanged apart from palette mapping.
std::vector<Image> correctFrames(const std::vector<Image>& frames, const QAReport& report, const Palette& palette,
                                 const QAOptions& opts = {});

// Every opaque pixel mapped to its nearest palette colour (alpha kept).
Image mapToPalette(const Image& img, const Palette& palette);

} // namespace acestudio
