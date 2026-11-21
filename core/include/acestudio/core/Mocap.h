#pragma once
// 3D skeletal motion onto a 2D sprite (Motion Studio, ADR-ANIM-002).
//
// A BVH file (the plain-text motion capture format that Blender, Mixamo
// exports via Blender, the CMU library, Rokoko and most mocap tools write)
// or one animation of a glTF/GLB file is read into a skeleton and its
// per-frame channels. Forward kinematics
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
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
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

// ---- glTF 2.0 / GLB (MocapGltf.cpp) ---------------------------------------------
//
// One animation of a .gltf (JSON) or .glb (binary) file becomes a Motion:
//  - the skeleton is the first skin that the animation moves (else the first
//    skin); with no skin, the node subtree under the animated nodes. Parents
//    come before children; node names are the joint names;
//  - every translation/rotation/scale channel (LINEAR with slerp, STEP,
//    CUBICSPLINE Hermite) is sampled at `fps` over the animation's time range
//    (time 0 of the Motion = the first keyframe). Unanimated nodes keep their
//    rest TRS. Morph weights are ignored;
//  - the result is BVH-like: offsets are the rest pose in world axes (so
//    restPositions() is the glTF rest pose), the root has 3 position + 3
//    rotation channels (Z, X, Y), other joints 3 rotation channels, plus 3
//    position channels only where translation or scale animation moves them.
//    jointPositions() reproduces the glTF world positions, including the
//    transforms of nodes above the skeleton (an Armature's rotation and scale).
// Only what the motion needs is read (no meshes or images). No extension is
// followed; a file that requires one is refused. External buffers must be
// relative file names inside `baseDir` (no "..", no absolute paths, no URL
// schemes); data: URIs must be base64. Sparse accessors are refused. Errors
// (CorruptData / UnsupportedVersion) name the accessor or channel at fault.
struct GltfOptions {
    int animationIndex = -1;        // >= 0: this animation (wins over `animation`)
    std::string animation;          // a name, or an index as text; empty = the first
    double fps = 30;                // sampling rate, 1..240
    std::size_t maxBytes = 64u << 20;   // the file plus external buffers it loads
    int maxJoints = 512;
    int maxNodes = 16384;
    int maxChannels = 4096;         // per animation
    std::size_t maxKeyframes = 1u << 20;   // per channel
    int maxFrames = 200000;         // sampled frames
};

Result<Motion> parseGltf(std::span<const std::uint8_t> bytes, const std::filesystem::path& baseDir, const GltfOptions& opt = {});
// The animations' names, in file order (an unnamed one is "Animation <i>").
Result<std::vector<std::string>> gltfAnimationNames(std::span<const std::uint8_t> bytes, const GltfOptions& opt = {});

// By extension: .bvh, .glb or .gltf (anything else is refused with a hint).
Result<Motion> loadMotion(const std::filesystem::path& path, const GltfOptions& opt = {});
// The animations a motion file holds: glTF names; a BVH holds one (empty list).
Result<std::vector<std::string>> motionAnimationNames(const std::filesystem::path& path, const GltfOptions& opt = {});

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

// ---- Four-legged skeletons (ADR-ANIM-002 stage 3c) -------------------------------

// A leg found by shape: a chain of joints from the body down to a joint that
// touches the floor in the rest pose.
struct CreatureLeg {
    std::vector<int> joints;     // from its top (the joint below the body) to the lowest joint
    bool hind = false;
    double length = 0;           // rest length along the chain
};
struct CreatureMap {
    int root = -1;
    std::vector<CreatureLeg> legs;   // hind legs first, then front legs
    // A six- or eight-legged skeleton's other legs: the pairs between the
    // hind and front ones, from the back forward, two per pair.
    std::vector<CreatureLeg> middle;
    Vec3 forward{0, 0, 1};           // horizontal, from the hind legs to the front legs
    // The other ends, found by shape too: the head is the end furthest
    // forward (ahead of the middle), the tail the end furthest back (behind
    // the hind legs). -1 when there is none.
    int head = -1, tail = -1;
    std::vector<std::string> notes;
    bool hasFourLegs() const { return legs.size() == 4; }   // a hind and a front pair (and maybe middle ones)
    // Leg `i` of all of them: 0-3 are `legs`, 4 and up are `middle`.
    std::size_t legCount() const { return legs.size() + middle.size(); }
    const CreatureLeg& leg(std::size_t i) const { return i < legs.size() ? legs[i] : middle[i - legs.size()]; }
};
// Names are not used: the legs are the chains ending near the floor (a toe
// and heel under one foot count once), the front pair is the one the root
// travels towards, or else the one nearer the highest other end (the head).
// Four, six or eight legs are taken (the hind and front pairs go to `legs`,
// the rest to `middle`); any other count is reported in `notes` and gives
// no legs.
CreatureMap creatureMap(const Motion& m);

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
    // The turn the sprite is drawn at (degrees, as Camera::turn), when it is
    // known (a direction set's view). Nullopt = read from the sprite (front
    // or side). It is the camera's turn when no camera is given, and the view
    // the "approximate view" check measures from.
    std::optional<double> drawnTurn;
    double start = 0;            // seconds into the motion
    double length = 0;           // seconds; 0 = to the end
    int fps = 12;                // frames sampled per second of motion
    int frames = 0;              // > 0: exactly this many frames over the span
    bool inPlace = true;         // remove the root's travel
    bool loop = true;
    double amount = 1;           // scales the motion's swing (0.25..2)
    // Biped: a humanoid motion on a two-legged sprite. Quadruped: a
    // four-legged motion (creatureMap) on a four-legged sprite; its legs
    // follow the motion's legs and the body rises and sinks with it.
    // Flyer: a humanoid motion on a winged sprite; each wing turns about its
    // shoulder as the motion's arm rises and falls, the hand end trails as
    // the elbow bends, and the body rises and sinks with the hips.
    // Swimmer: any motion with a spine (a chain from a joint named head to
    // one named tail, else the skeleton's longest chain) on a fish-like
    // sprite; the spine's wave bends the body and turns the tail fin.
    motion::Body body = motion::Body::Biped;
    // Flyer only: which pixels are wings, as motion::Options (the part above
    // `wingLine` of the opaque height, or exactly `wingMask` when set).
    double wingLine = 0.45;
    Mask wingMask;
    std::vector<std::pair<Bone, std::string>> mapOverrides;
};

struct RetargetResult {
    motion::Clip clip;           // the sprite, posed frame by frame
    motion::Clip source;         // the projected 3D skeleton, same frames
    BoneMap map;                 // humanoid bones (a four-legged retarget maps only Hips)
    CreatureMap creature;        // four-legged retargets only
    Camera camera;
    bool approximateView = false;   // camera far from the sprite's drawn view
    std::vector<std::string> notes;
};

Result<RetargetResult> retarget(const Image& sprite, const Motion& m, const RetargetOptions& opt = {});

// A swimming motion read for a fish drawn from the side. The spine is the
// chain from the joint named head to the one named tail (the skeleton's
// longest chain without such names). `wave` is its offset across the body
// (in the direction it moves in more), from the head and from its neutral
// shape (a loop's mean, a one-shot's first frame), as a fraction of the
// chain's length. A fin is a branch off the spine named fin, flipper,
// pectoral, pelvic or dorsal that moves.
struct SwimFinSource {
    std::string name;
    bool top = false;          // points up from the spine (a dorsal fin)
    double u = 0;              // where it joins the spine, 0 head .. 1 tail
    std::vector<double> turn;  // per frame, radians; toward the tail is positive
    // Per frame, its length as seen from the side over its longest in the
    // span (a loop) or its first frame. Empty when it does not change.
    std::vector<double> shown;
};
struct SwimSource {
    int frames = 0;
    double step = 0;                        // seconds between frames
    std::vector<int> chain;                 // joints, head first
    std::vector<double> along;              // per chain joint, 0 head .. 1 tail
    bool byName = false;
    double chainLen = 0;
    std::vector<std::vector<double>> wave;  // [frame][chain joint]
    std::vector<SwimFinSource> fins;
    std::vector<std::vector<Vec3>> world;   // [frame][joint]
    double floorY = 0;
    Vec3 axis;                              // head to tail at rest
    std::vector<std::string> notes;
    double waveAt(int frame, double u) const;
    // The chain's end against the body before `stalkU` (radians, down positive).
    double tailTurn(int frame, double stalkU = 0.8) const;
};
// Uses start, length, fps, frames and loop of `opt`. Fails when the
// skeleton has no spine of three joints.
Result<SwimSource> swimSource(const Motion& m, const RetargetOptions& opt = {});

// What can drive a pair of wings: the skeleton's own wing bones (the two
// longest chains of joints named "wing": the chain's first joint and its
// farthest joint), else the humanoid arms of `map` (upper arm and hand,
// [0] left, [1] right). -1 when there is neither. Own wings are ordered
// left first by their rest position (+X is left for a skeleton facing +Z).
struct WingSource {
    int shoulder[2] = {-1, -1}, tip[2] = {-1, -1};
    bool own = false;
    bool found() const { return shoulder[0] >= 0 && shoulder[1] >= 0 && tip[0] >= 0 && tip[1] >= 0; }
};
WingSource wingSource(const Motion& m, const BoneMap& map);

// The body kind "auto" stands for, from the motion first and the sprite
// second: Flyer when the skeleton has two chains of joints named wing
// (unless both it and the sprite stand on four legs), Swimmer when the
// skeleton has no legs at all (no four legs by shape, no humanoid legs),
// else Quadruped for a four-legged sprite and Biped for the rest. A
// humanoid motion never picks Flyer or Swimmer: ask for those by name.
// `why`, when given, gets a sentence for the user if the motion decided.
motion::Body autoBody(const Image& sprite, const Motion& m, std::string* why = nullptr);

// The projected skeleton as a picture: left side blue, right side red, spine
// yellow, on transparent, `size` pixels square, for a side-by-side preview.
Image drawSkeleton(const Motion& m, const BoneMap& map, const Camera& cam, double seconds, int size, bool inPlace = true);

} // namespace acestudio::mocap
