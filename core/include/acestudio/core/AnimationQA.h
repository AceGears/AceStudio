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
//   silhouette    overlap with the previous frame (bottom-centre aligned):
//                 sudden shape changes mean the model drew something else
// Corrections fix what can be fixed exactly (ground, height, centring,
// palette); frames whose silhouette jumps are flagged for regeneration.

#include "acestudio/core/Image.h"
#include "acestudio/core/Palette.h"
#include "acestudio/core/Raster.h"

#include <cstdint>
#include <string>
#include <vector>

namespace acestudio {

// Makes the background transparent: pixels connected to the image border whose
// colour is within `tolerance` (max channel difference) of the median border
// colour. Interior pixels of the same colour (enclosed by the subject) stay.
Image removeBorderBackground(const Image& src, int tolerance = 24);

// Alpha > 0 pixels.
Mask silhouette(const Image& img);

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

struct QAOptions {
    int groundTolerance = 1;          // px
    double heightTolerance = 0.08;    // relative to the reference height
    double paletteTolerance = 0.06;   // paletteDistance
    double colorMixTolerance = 0.35;  // colorMixDistance vs the reference
    double minIoU = 0.55;             // silhouette overlap with the previous frame
};

enum QAFlag : std::uint32_t {
    QAGround = 1u << 0,
    QAHeight = 1u << 1,
    QAPalette = 1u << 2,
    QAColorMix = 1u << 3,
    QASilhouette = 1u << 4,
    QAEmpty = 1u << 5,
};

struct FrameQA {
    IntRect bounds;                  // opaque bounds (empty if nothing drawn)
    int groundOffset = 0;            // frame ground - reference ground (px, + = lower)
    double heightRatio = 1.0;        // frame height / reference height
    double paletteDistance = 0.0;
    double colorMix = 0.0;           // vs the reference
    double iouPrevious = 1.0;        // vs the previous frame (1 for the first)
    std::uint32_t flags = 0;
    double score = 1.0;              // 0..1, 1 = consistent
    bool ok() const noexcept { return flags == 0; }
    // Flags that corrections cannot fix: regenerate the frame.
    bool needsRegeneration() const noexcept { return (flags & (QASilhouette | QAEmpty | QAColorMix)) != 0; }
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
// all images must have the same size.
QAReport evaluateFrames(const std::vector<Image>& frames, const Image& reference, const Palette& palette,
                        const QAOptions& opts = {});
std::string describe(const FrameQA& qa);

// Fixes the ground line, height (nearest-neighbour scale about the feet) and
// off-palette colours; each frame keeps its horizontal position. Frames that need regeneration are returned
// unchanged apart from palette mapping.
std::vector<Image> correctFrames(const std::vector<Image>& frames, const QAReport& report, const Palette& palette,
                                 const QAOptions& opts = {});

// Every opaque pixel mapped to its nearest palette colour (alpha kept).
Image mapToPalette(const Image& img, const Palette& palette);

} // namespace acestudio
