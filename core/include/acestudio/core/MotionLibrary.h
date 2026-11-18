#pragma once
// The motion library (ADR-ANIM-002 stage 3b): ready motions for Motion Studio
// and `retarget`, and the user's own saved motions.
//
// Built-in presets are made by code, not captured: a humanoid skeleton with
// Mixamo-style joint names (so autoMap finds every bone), Y up, facing +Z,
// sampled at 30 frames a second. Nothing is bundled, so no clip carries a
// licence to track. Looping presets (idle, walk, run) end one frame before
// they repeat; one-shot presets (jump, attack, hurt) start and end at rest.
//
// Saved presets are BVH files in a folder (by default "<user data>/motions"),
// one per motion, named after the preset. Saving a glTF/GLB animation there
// converts it to BVH, so every saved preset loads the same way.

#include "acestudio/core/Mocap.h"
#include "acestudio/core/Result.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace acestudio::mocap {

struct PresetInfo {
    std::string name;            // "walk", or a saved preset's name
    std::string description;
    double seconds = 0;
    bool loops = false;          // built-in presets only; saved presets: false
    bool builtIn = false;
    bool fourLegged = false;     // a four-legged skeleton (for four-legged sprites)
    std::filesystem::path file;  // saved presets: the BVH file
    std::string error;           // saved presets that could not be read
};

// idle, walk, run, jump, attack, hurt, flap (humanoid; flap is for winged sprites)
// and quad-walk (four-legged).
std::vector<PresetInfo> builtInPresets();
// NotFound for an unknown name.
Result<Motion> builtInPreset(std::string_view name);

// BVH text for a motion. parseBvh reads it back with the same joints (names
// made BVH-safe and unique), offsets, channels and values (to 9 significant
// digits), listed depth-first as BVH requires (a motion stored in another
// parent-first order comes back reordered, with the same poses). End-site
// joints become "End Site" blocks. Refused: no frames or
// channels, more than one root, values that are not finite or above 1e7.
Result<std::string> writeBvh(const Motion& m);

// "<user data>/motions" (the folder is not created here).
Result<std::filesystem::path> defaultPresetDir();

// A preset name: 1..64 letters, digits, spaces, '-' or '_', not starting or
// ending with a space, and not a built-in name (any case).
Status checkPresetName(std::string_view name);

// The saved presets in `dir` (*.bvh), sorted by name. A missing folder is an
// empty library. Unreadable files are listed with `error` set.
Result<std::vector<PresetInfo>> savedPresets(const std::filesystem::path& dir);

// Writes `m` as "<dir>/<name>.bvh" (creating `dir`). Refuses a bad name and,
// unless `overwrite`, a name already saved.
Status savePreset(const std::filesystem::path& dir, std::string_view name, const Motion& m, bool overwrite = false);

// Deletes a saved preset (NotFound if there is none).
Status removePreset(const std::filesystem::path& dir, std::string_view name);

// A built-in preset, else the saved preset of that name in `dir`.
Result<Motion> loadPreset(std::string_view name, const std::filesystem::path& dir);

} // namespace acestudio::mocap
