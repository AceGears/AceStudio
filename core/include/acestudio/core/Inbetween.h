#pragma once
// In-betweens for pixel art: new frames between two drawn frames.
//
// Ordinary frame interpolation blends colours and blurs outlines, which ruins
// low-resolution art. This works the way an animator's in-between does: every
// pixel of the first frame finds where it went in the second (patch matching,
// both ways, so pixels that appear or vanish are known), and each in-between
// moves the pixels part of the way. Pixels are moved, never mixed: every colour
// of an in-between comes from one of the two frames, so the palette is kept
// and alpha stays binary. The moved pixels are drawn at a finer scale and
// brought back by a majority vote per pixel (like a K-centroid downscale), so
// shapes stay solid instead of tearing.
//
// What it cannot do: invent what neither frame shows (a limb that turns round,
// detail hidden in both). A pose that changes that much needs a drawn or
// generated key, and the area check below says which in-betweens to look at.

#include "acestudio/core/Document.h"
#include "acestudio/core/Image.h"
#include "acestudio/core/Result.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace acestudio::inbetween {

struct Options {
    int intermediates = 1;        // new frames between each pair (1..7)
    int searchRadius = 0;         // furthest a pixel is looked for; 0 = a third of the frame
    int patchRadius = 2;          // pixels matched by their 5x5 neighbourhood
    int iterations = 5;           // matching passes
    int supersample = 3;          // finer drawing scale before the vote (1..4)
    double opaqueShare = 0.5;     // share of a pixel's samples that makes it opaque
    double areaTolerance = 0.25;  // flag an in-between whose area is this far off the expected
    std::uint32_t seed = 1;       // the matcher's random search (results are repeatable)
    // Checked between steps; true stops the work with a Cancelled error.
    std::function<bool()> cancelled;
};

// Where each pixel of `from` went in `to` (whole pixels), and how sure that is.
struct Flow {
    int width = 0, height = 0;
    std::vector<int> dx, dy;      // per pixel; 0 for transparent pixels
    std::vector<float> cost;      // patch difference at the match (0 = identical)
};

// Pixel motion from `from` to `to` (same size).
Result<Flow> estimateFlow(const Image& from, const Image& to, const Options& opt = {});

struct PairResult {
    std::vector<Image> frames;            // opt.intermediates frames, in order
    std::vector<double> areaDeviation;    // per frame: (area - expected) / expected
};

// The in-betweens between `a` and `b` (same size, RGBA with any alpha; output
// alpha is 0 or 255).
Result<PairResult> inbetweenPair(const Image& a, const Image& b, const Options& opt = {});

enum class Timing {
    Split,   // a key's duration is shared between it and the in-betweens after it
    Fixed,   // every frame, key or new, lasts `fixedMs`
};

struct SequenceResult {
    std::vector<Image> frames;
    std::vector<int> durations;           // ms
    std::vector<int> key;                 // per frame: index of the source key, or -1 for an in-between
    std::vector<int> flagged;             // frames (indices into `frames`) the area check flags
    std::vector<std::string> notes;
};

// In-betweens for a run of keys (two or more, one size). With `loop`, the last
// key also gets in-betweens towards the first. Durations: one per key (ms).
Result<SequenceResult> inbetweenSequence(const std::vector<Image>& keys, const std::vector<int>& durations, bool loop,
                                         Timing timing, int fixedMs, const Options& opt = {});

struct DocumentResult {
    Document document;               // the edited copy
    std::vector<int> newFrames;      // indices of the in-betweens in `document`
    std::vector<int> flagged;        // in-betweens the area check doubts (indices in `document`)
    std::vector<std::string> notes;
};

// In-betweens one raster layer of a document across the frames first..last
// (the keys, two or more). The new frames go after each key; every other
// layer holds the key's cel (and mask) there, so only `layer` moves. Tags
// that end on a key grow to keep its in-betweens. Durations follow `timing`.
// The caller applies the result as one undo step (Editor::replaceDocument).
Result<DocumentResult> inbetweenLayer(const Document& doc, LayerId layer, int first, int last, bool loop, Timing timing,
                                      int fixedMs, const Options& opt = {});

} // namespace acestudio::inbetween
