#pragma once

// Texture atlas packing (Phase 6).
//
// MaxRects bin packing (best short side fit), fully deterministic: items are
// placed in a fixed order (larger side, then area, then name, then input
// position) and no hashing or randomness is involved, so the same input gives
// byte-identical pages and JSON on every run and platform.

#include "acestudio/core/Image.h"
#include "acestudio/core/Result.h"

#include <string>
#include <vector>

namespace acestudio {

// Positions for `sizes` (w, h) inside one bin of at most maxW x maxH, each
// with `gap` empty pixels to its right and bottom neighbours. Returns one rect
// per input (in input order) or an empty vector if they do not all fit.
// Deterministic.
std::vector<IntRect> packRects(const std::vector<std::pair<int, int>>& sizes, int maxW, int maxH, int gap = 0);

struct AtlasItem {
    std::string name;
    Image image;
};

struct AtlasOptions {
    int maxWidth = 4096;
    int maxHeight = 4096;
    int padding = 0;          // empty pixels between sprites
    int border = 0;           // empty pixels around each page
    int extrude = 0;          // repeat each sprite's edge pixels this far out (stops filtering seams)
    bool trim = false;        // crop transparent borders (the original size is kept in the JSON)
    bool mergeDuplicates = true;   // identical (trimmed) images share one rect
    bool powerOfTwo = false;  // page sizes rounded up to powers of two
    bool multiplePages = true;     // otherwise fail when one page is not enough
};

struct AtlasSprite {
    std::string name;
    int page = 0;
    IntRect rect;             // where the (trimmed) image sits on its page, extrusion excluded
    IntRect source;           // the trimmed area within the original image
    int sourceWidth = 0, sourceHeight = 0;   // original size
    bool trimmed = false;
    int duplicateOf = -1;     // index of the sprite whose rect it shares, or -1
};

struct Atlas {
    std::vector<Image> pages;
    std::vector<AtlasSprite> sprites;   // input order
};

// Fails on empty input, duplicate names, a sprite larger than a page, or (with
// multiplePages off) items that need more than one page.
Result<Atlas> packAtlas(const std::vector<AtlasItem>& items, const AtlasOptions& opts = {});

// TexturePacker "JSON (Hash)" for one page, or its "multiatlas" form
// ({"textures": [...]}) when there are several pages. `pageFiles[i]` is the
// image file name stored for page i.
std::string atlasJson(const Atlas& atlas, const std::vector<std::string>& pageFiles);

} // namespace acestudio
