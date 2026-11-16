#pragma once
// A document's rig: a tree of bones, the raster layers each bone moves (its
// parts), and two-bone IK chains (ADR-ANIM-004).
//
// Parts are ordinary raster layers. They are painted with the usual tools,
// stacked like any layer, and exported as they are; the rig only says which
// bone moves which layer. A pose turns bones; baking a pose (RigTools.h)
// writes the turned parts into a frame's cels, pixel-art safe.
//
// Geometry is in canvas pixels, y down. Angles are radians, clockwise on
// screen from +x (the convention of the part rig the animator uses). A
// bone's rest geometry is absolute (head, length, direction), so reparenting
// a bone never moves it.
//
// Everything here is pure data and functions on it. Undo goes through
// Editor::setRig, which replaces the whole rig (rigs are small).

#include "acestudio/core/Result.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace acestudio {

using LayerId = std::uint32_t;   // as in Document.h
class Document;

using BoneId = std::uint32_t;
inline constexpr BoneId kNoBone = 0;

struct Bone {
    BoneId id = kNoBone;
    BoneId parent = kNoBone;     // kNoBone = a root
    std::string name;
    double x = 0, y = 0;         // head (the joint it turns about), at rest
    double length = 0;           // head to tail, >= 0
    double angle = 0;            // rest direction, head to tail
    // Pose limits: how far the bone may turn from its rest direction,
    // relative to its parent (minAngle <= 0 <= maxAngle not required).
    bool limited = false;
    double minAngle = 0, maxAngle = 0;
    bool locked = false;         // posing and IK leave it at rest
    bool operator==(const Bone&) const = default;

    double tailX() const;
    double tailY() const;
};

// Two bones ending at `tip` (the tip and its parent) solved together so the
// tip's tail reaches a target; the middle joint bends to the side `bend`
// (+1 or -1, as drawn).
struct IkChain {
    std::string name;
    BoneId tip = kNoBone;
    int bend = 1;
    bool operator==(const IkChain&) const = default;
};

struct Rig {
    static constexpr int kMaxBones = 256;
    static constexpr int kMaxChains = 64;
    static constexpr int kVersion = 1;

    std::vector<Bone> bones;              // parents before children
    std::map<LayerId, BoneId> bindings;   // part layer -> the bone that moves it
    std::vector<IkChain> chains;
    BoneId nextId = 1;
    bool operator==(const Rig&) const = default;

    const Bone* bone(BoneId id) const noexcept;
    Bone* bone(BoneId id) noexcept;
    const Bone* bone(const std::string& name) const noexcept;
    std::vector<BoneId> children(BoneId id) const;
    // `id` and every bone below it.
    std::vector<BoneId> subtree(BoneId id) const;
    // Layers moved by `id` (not its children).
    std::vector<LayerId> parts(BoneId id) const;
};

// A pose: per bone, its turn from rest (relative to the parent's turn, so
// children follow), plus a move of every root.
struct RigPose {
    std::map<BoneId, double> turn;
    double dx = 0, dy = 0;
    bool operator==(const RigPose&) const = default;
};

// Where a bone is under a pose.
struct BoneWorld {
    double headX = 0, headY = 0, tailX = 0, tailY = 0;
    double turn = 0;   // total turn from rest (its own plus its parents')
};

namespace rigs {

// Structure and values: ids unique and non-zero, parents present and before
// their children (no cycles), names unique and non-empty, numbers finite,
// lengths >= 0, limits ordered, chains on bones that have a parent, counts
// within limits. With a document, bindings must name its raster layers.
Status validate(const Rig& rig, const Document* doc = nullptr);

// ---- Edits (each validates and leaves the rig unchanged on error) --------------
Result<BoneId> addBone(Rig& rig, Bone bone);   // id assigned; parent may be kNoBone
Status renameBone(Rig& rig, BoneId id, std::string name);
// Rest geometry. With `children`, bones below move along (turning about the
// old head when the angle changes); otherwise they stay where they are.
Status setBoneGeometry(Rig& rig, BoneId id, double x, double y, double length, double angle, bool children);
Status reparentBone(Rig& rig, BoneId id, BoneId newParent);   // no cycles; nothing moves
// The bone's children go to its parent, its parts to its parent (or are
// unbound for a root), chains on it go.
Status removeBone(Rig& rig, BoneId id);
Status setLimits(Rig& rig, BoneId id, bool limited, double minAngle, double maxAngle);
Status setLocked(Rig& rig, BoneId id, bool locked);
Status bindLayer(Rig& rig, LayerId layer, BoneId bone);   // kNoBone unbinds
Status addChain(Rig& rig, IkChain chain);
Status removeChain(Rig& rig, std::size_t index);

// ---- Posing ---------------------------------------------------------------------
// The turn a bone may take: 0 when locked, clamped to its limits.
double clampTurn(const Bone& bone, double turn);
// Forward kinematics, in rig.bones order.
std::vector<BoneWorld> solve(const Rig& rig, const RigPose& pose);
// Turns the chain's two bones so the tip's tail gets as close to the target
// as their lengths, limits and locks allow. Other bones keep their turns.
Status reach(const Rig& rig, RigPose& pose, std::size_t chain, double targetX, double targetY);

// ---- File form (document.json "rig", schema 4) ------------------------------
nlohmann::json toJson(const Rig& rig);
Result<Rig> fromJson(const nlohmann::json& j);

} // namespace rigs
} // namespace acestudio
