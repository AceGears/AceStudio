#pragma once

// Exporting frames: selection of frames, sprite sheets with Aseprite-
// compatible JSON metadata, animated files and PNG sequences.

#include "acestudio/core/AnimCodec.h"
#include "acestudio/core/Document.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace acestudio {

// Which frames to export. A named tag selects its range; otherwise
// [from, to] (to = -1 means the last frame).
struct FrameRange {
    std::string tag;
    int from = 0;
    int to = -1;
};

// Frame indices in document order for the range. Fails on unknown tags.
Result<std::vector<int>> resolveFrames(const Document& doc, const FrameRange& range);

// Flattened frames (optionally scaled up by an integer factor with
// nearest-neighbour). When `playbackOrder` is set and a tag is selected,
// frames follow the tag direction (reverse / ping-pong) for animated
// formats.
Result<std::vector<AnimFrame>> renderFrames(const Document& doc, const FrameRange& range, int scale = 1,
                                            bool playbackOrder = false);

enum class SheetLayout { Horizontal, Vertical, Grid, Packed };

struct SheetOptions {
    SheetLayout layout = SheetLayout::Horizontal;
    int columns = 0;              // Grid: 0 = automatic (near-square)
    int borderPadding = 0;        // empty pixels around the whole sheet
    int spacing = 0;              // empty pixels between frames
    int innerPadding = 0;         // empty pixels around each frame (inside its rect)
    bool trim = false;            // crop transparent borders of each frame
    bool mergeDuplicates = false; // identical frames share one rect
    bool powerOfTwo = false;      // round sheet size up to powers of two
    int scale = 1;                // integer nearest-neighbour upscale
    int maxWidth = 8192;          // Packed: target maximum width
};

struct SheetFrame {
    int frame = 0;          // document frame index
    IntRect rect;           // where the (trimmed) frame sits in the sheet
    IntRect source;         // trimmed area within the full frame (scaled)
    bool trimmed = false;
    int durationMs = 100;
};

struct SpriteSheet {
    Image image;
    int frameWidth = 0;     // untrimmed, scaled
    int frameHeight = 0;
    std::vector<SheetFrame> frames;
};

Result<SpriteSheet> buildSpriteSheet(const Document& doc, const FrameRange& range, const SheetOptions& opts);

enum class JsonStyle { Hash, Array };

// Aseprite-compatible JSON (read by Phaser's load.aseprite, many engines and
// texture tools). Frame names are "<baseName> <index>". Tags are remapped to
// exported frame positions; tags outside the exported range are dropped.
std::string spriteSheetJson(const Document& doc, const SpriteSheet& sheet, const std::string& imageFile,
                            const std::string& baseName, JsonStyle style);

// Writes <dir>/<baseName>_<NNN>.png for each frame. Refuses to overwrite
// existing files unless `overwrite`. Returns the written paths.
Result<std::vector<std::filesystem::path>> exportPngSequence(const Document& doc, const FrameRange& range,
                                                             const std::filesystem::path& dir,
                                                             const std::string& baseName, int scale = 1,
                                                             bool overwrite = false);

} // namespace acestudio
