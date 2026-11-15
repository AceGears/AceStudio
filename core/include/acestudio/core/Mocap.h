#pragma once
// 3D skeletal motion onto a 2D sprite (Motion Studio, ADR-ANIM-002).
//
// A BVH file (the plain-text motion capture format that Blender, Mixamo
// exports via Blender, the CMU library, Rokoko and most mocap tools write)
// is read into a skeleton and its per-frame channels. Forward kinematics
// gives every joint's position in 3D; a camera (turn, tilt, roll; orthographic
// or perspective) projects them onto the screen.
//
// Retargeting does not copy joint rotations. The 2D sprite is cut into the
// same part rig the procedural walk uses (thighs, shins, feet, arms, body
// bands), and each frame's projected joints become targets for it:
//  - the feet go where the projected ankles are, measured from the hips and
//    scaled by leg length (the sprite's proportions are kept, not the
//    source's), and two-bone IK finds the knees: bones keep their length;
//  - a foot the source plants on the floor is held on the sprite's ground
//    row, and the hips sink when a planted leg could not reach otherwise;
//  - the body leans as the projected spine leans; arms turn to point where
//    the projected arms point;
//  - root travel is removed for an in-place cycle (game sprites), or kept.
//
// The camera only changes how the motion is seen. It never turns the
// artwork: a sprite drawn from the side shows side-view art whatever the
// camera. When the camera is far from the view the sprite was drawn in, the
// result says so (a new view needs art for that view).

#include "acestudio/core/Image.h"
#include "acestudio/core/Result.h"
#include "acestudio/core/SpriteMotion.h"

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace acestudio::mocap {

struct Vec3 {
    double x = 0, y = 0, z = 0;
};

enum class Channel { Xposition, Yposition, Zposition, Xrotation, Yrotation, Zrotation };

struct Joint {
    std::string name;            // "End Site" joints are named <parent>_end
    int parent = -1;             // -1 = root
    Vec3 offset;                 // from the parent, in the parent's frame
    std::vector<Channel> channels;
    int firstChannel = 0;        // index of its first value in a frame
    bool endSite = false;
};

struct Motion {
    std::vector<Joint> joints;   // parents come before their children
    int frameCount = 0;
    double frameTime = 1.0 / 30; // seconds
    int channelCount = 0;
    std::vector<double> values;  // frameCount x channelCount

    double duration() const { return frameCount * frameTime; }
    int find(std::string_view name) const;   // -1 if missing
};

// Limits on what is accepted (an imported file is untrusted input).
struct BvhLimits {
    std::size_t maxBytes = 64u << 20;
    int maxJoints = 512;
    int maxDepth = 64;
    int maxFrames = 200000;
};

// Errors (CorruptData) name the line that is wrong. Nothing in the file is
// executed; only HIERARCHY / MOTION with the standard six channel names is read.
Result<Motion> parseBvh(std::string_view text, const BvhLimits& limits = {});
Result<Motion> loadBvh(const std::filesystem::path& path, const BvhLimits& limits = {});

// World positions of every joint (same order as Motion::joints) at a time in
// seconds; between two frames the positions are blended linearly. Times
// outside the clip clamp to it. frame < 0 gives the rest pose (offsets only).
std::vector<Vec3> jointPositions(const Motion& m, double seconds);
std::vector<Vec3> restPositions(const Motion& m);

// ---- Bone mapping --------------------------------------------------------------

// The joints a humanoid retarget needs.
enum class Bone {
    Hips, Spine, Chest, Neck, Head,
    LShoulder, LUpperArm, LForearm, LHand,
    RShoulder, RUpperArm, RForearm, RHand,
    LThigh, LShin, LFoot, LToe,
    RThigh, RShin, RFoot, RToe,
};
inline constexpr int kBoneCount = 21;
std::string_view toString(Bone b) noexcept;
std::optional<Bone> boneFromString(std::string_view s) noexcept;

struct BoneMap {
    std::array<int, kBoneCount> joint{};   // index into Motion::joints, -1 = not mapped
    std::vector<std::string> notes;        // how it was found, what is missing
    int operator[](Bone b) const { return joint[static_cast<std::size_t>(b)]; }
    int& operator[](Bone b) { return joint[static_cast<std::size_t>(b)]; }
    // Hips, both thighs, shins and feet: enough to walk.
    bool hasLegs() const;
};

// By name first (CMU / Mixamo "LeftUpLeg", 3ds Max "Bip01 L Thigh", Blender
// "thigh.L", Daz "lThigh", ...), then by the shape of the hierarchy for
// anything still missing. `overrides` ("Bone=joint name") win.
BoneMap autoMap(const Motion& m, const std::vector<std::pair<Bone, std::string>>& overrides = {});

// ---- Camera ----------------------------------------------------------------------

// Angles in degrees, about the character (not the world):
//  turn (yaw)  0 = the camera in front of the character (it faces the viewer),
//              90 = at its right side (it faces screen right), 180 = behind it;
//  tilt (pitch) the camera rises above it and looks down (90 = top-down);
//  roll        turns the picture clockwise.
struct Camera {
    double turn = 90, tilt = 0, roll = 0;
    bool perspective = false;
    double fov = 35;             // degrees, perspective only
    double distance = 3;         // in character heights, perspective only
};
// front, back, right, left (the way the character faces on screen),
// front-right, front-left, back-right, back-left, three-quarter, top-down,
// isometric.
std::optional<Camera> cameraPreset(std::string_view name);
std::vector<std::string> cameraPresetNames();

struct Projected {
    std::vector<double> x, y;    // screen units (character heights), y down
    std::vector<double> depth;   // larger = nearer the camera
};
// The character's forward direction (horizontal) is taken from the motion:
// the way the hips travel, or else the way the toes point, or else +Z.
Vec3 characterForward(const Motion& m, const BoneMap& map);
Projected project(const std::vector<Vec3>& world, const Camera& cam, Vec3 forward, double height, Vec3 centre);

// ---- Retarget --------------------------------------------------------------------

struct RetargetOptions {
    // Nullopt = chosen from the sprite: front for a front view, the side the
    // sprite faces for a side view.
    std::optional<Camera> camera;
    double start = 0;            // seconds into the motion
    double length = 0;           // seconds; 0 = to the end
    int fps = 12;                // frames sampled per second of motion
    int frames = 0;              // > 0: exactly this many frames over the span
    bool inPlace = true;         // remove the root's travel
    bool loop = true;
    double amount = 1;           // scales the motion's swing (0.25..2)
    motion::Body body = motion::Body::Biped;
    std::vector<std::pair<Bone, std::string>> mapOverrides;
};

struct RetargetResult {
    motion::Clip clip;           // the sprite, posed frame by frame
    motion::Clip source;         // the projected 3D skeleton, same frames
    BoneMap map;
    Camera camera;
    bool approximateView = false;   // camera far from the sprite's drawn view
    std::vector<std::string> notes;
};

Result<RetargetResult> retarget(const Image& sprite, const Motion& m, const RetargetOptions& opt = {});

// The projected skeleton as a picture: left side blue, right side red, spine
// yellow, on transparent, `size` pixels square, for a side-by-side preview.
Image drawSkeleton(const Motion& m, const BoneMap& map, const Camera& cam, double seconds, int size, bool inPlace = true);

} // namespace acestudio::mocap
