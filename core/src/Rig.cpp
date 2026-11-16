#include "acestudio/core/Rig.h"
#include "acestudio/core/Document.h"

#include <algorithm>
#include <cmath>
#include <set>

using nlohmann::json;

namespace acestudio {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMaxCoord = 1e6;
constexpr std::size_t kMaxName = 128;

Error bad(std::string m) { return Error{ErrorCode::InvalidArgument, std::move(m)}; }

bool finite(double v) { return std::isfinite(v); }

// Parents before children, keeping the existing order where it already
// holds. Fails on a cycle or a missing parent.
Status sortBones(std::vector<Bone>& bones) {
    std::vector<Bone> out;
    out.reserve(bones.size());
    std::set<BoneId> placed;
    std::vector<bool> done(bones.size(), false);
    for (std::size_t pass = 0; pass <= bones.size(); ++pass) {
        bool progress = false;
        for (std::size_t i = 0; i < bones.size(); ++i) {
            if (done[i]) continue;
            if (bones[i].parent != kNoBone && !placed.count(bones[i].parent)) continue;
            out.push_back(bones[i]);
            placed.insert(bones[i].id);
            done[i] = true;
            progress = true;
        }
        if (out.size() == bones.size()) {
            bones = std::move(out);
            return {};
        }
        if (!progress) break;
    }
    return bad("The bones have a missing parent or form a loop");
}

void turnAbout(double cx, double cy, double a, double& x, double& y) {
    const double c = std::cos(a), s = std::sin(a);
    const double ux = x - cx, uy = y - cy;
    x = cx + c * ux - s * uy;
    y = cy + s * ux + c * uy;
}

double wrap(double a) {
    while (a > kPi) a -= 2 * kPi;
    while (a < -kPi) a += 2 * kPi;
    return a;
}

} // namespace

double Bone::tailX() const { return x + length * std::cos(angle); }
double Bone::tailY() const { return y + length * std::sin(angle); }

const Bone* Rig::bone(BoneId id) const noexcept {
    for (const Bone& b : bones)
        if (b.id == id) return &b;
    return nullptr;
}

Bone* Rig::bone(BoneId id) noexcept {
    for (Bone& b : bones)
        if (b.id == id) return &b;
    return nullptr;
}

const Bone* Rig::bone(const std::string& name) const noexcept {
    for (const Bone& b : bones)
        if (b.name == name) return &b;
    return nullptr;
}

std::vector<BoneId> Rig::children(BoneId id) const {
    std::vector<BoneId> out;
    for (const Bone& b : bones)
        if (b.parent == id && b.id != kNoBone) out.push_back(b.id);
    return out;
}

std::vector<BoneId> Rig::subtree(BoneId id) const {
    std::vector<BoneId> out{id};
    // Bones are sorted parents first, so one pass collects every descendant.
    std::set<BoneId> in{id};
    for (const Bone& b : bones)
        if (in.count(b.parent) && in.insert(b.id).second) out.push_back(b.id);
    return out;
}

std::vector<LayerId> Rig::parts(BoneId id) const {
    std::vector<LayerId> out;
    for (const auto& [layer, b] : bindings)
        if (b == id) out.push_back(layer);
    return out;
}

namespace rigs {

Status validate(const Rig& rig, const Document* doc) {
    if (rig.bones.size() > static_cast<std::size_t>(Rig::kMaxBones))
        return bad("A rig holds at most " + std::to_string(Rig::kMaxBones) + " bones");
    if (rig.chains.size() > static_cast<std::size_t>(Rig::kMaxChains))
        return bad("A rig holds at most " + std::to_string(Rig::kMaxChains) + " IK chains");
    std::set<BoneId> seen;
    std::set<std::string> names;
    for (const Bone& b : rig.bones) {
        if (b.id == kNoBone || !seen.insert(b.id).second) return bad("Bone ids must be unique and non-zero");
        if (b.id >= rig.nextId) return bad("Bone id " + std::to_string(b.id) + " is not below the next id");
        if (b.name.empty() || b.name.size() > kMaxName) return bad("Bone names must have 1 to 128 characters");
        if (!names.insert(b.name).second) return bad("Two bones are named '" + b.name + "'");
        if (b.parent != kNoBone && !seen.count(b.parent))
            return bad("Bone '" + b.name + "' comes before its parent, or its parent is missing");
        for (double v : {b.x, b.y, b.length, b.angle, b.minAngle, b.maxAngle})
            if (!finite(v)) return bad("Bone '" + b.name + "' has a value that is not a number");
        if (std::abs(b.x) > kMaxCoord || std::abs(b.y) > kMaxCoord || b.length < 0 || b.length > kMaxCoord)
            return bad("Bone '" + b.name + "' is out of range");
        if (std::abs(b.angle) > 64 * kPi) return bad("Bone '" + b.name + "' has an out-of-range angle");
        if (b.limited && (b.minAngle > b.maxAngle || b.minAngle < -2 * kPi || b.maxAngle > 2 * kPi))
            return bad("Bone '" + b.name + "' has limits out of order or beyond a full turn");
    }
    for (const auto& [layer, b] : rig.bindings) {
        if (!seen.count(b)) return bad("A part is bound to a missing bone");
        if (doc) {
            const Layer* l = doc->layer(layer);
            if (!l || l->kind != LayerKind::Raster) return bad("A part names a missing or non-raster layer");
        }
    }
    std::set<BoneId> tips;
    for (const IkChain& c : rig.chains) {
        const Bone* t = rig.bone(c.tip);
        if (!t || t->parent == kNoBone) return bad("IK chain '" + c.name + "' needs a tip bone that has a parent");
        if (c.bend != 1 && c.bend != -1) return bad("IK chain '" + c.name + "' bends +1 or -1");
        if (c.name.empty() || c.name.size() > kMaxName) return bad("IK chain names must have 1 to 128 characters");
        if (!tips.insert(c.tip).second) return bad("Two IK chains end on bone '" + t->name + "'");
    }
    return {};
}

namespace {
// Applies `edit` to a copy and keeps it only if the result is valid.
template <typename Fn> Status edited(Rig& rig, Fn edit) {
    Rig next = rig;
    if (auto st = edit(next); !st) return st;
    if (auto st = validate(next); !st) return st;
    rig = std::move(next);
    return {};
}
} // namespace

Result<BoneId> addBone(Rig& rig, Bone bone) {
    if (rig.bones.size() >= static_cast<std::size_t>(Rig::kMaxBones))
        return bad("A rig holds at most " + std::to_string(Rig::kMaxBones) + " bones");
    if (bone.parent != kNoBone && !rig.bone(bone.parent)) return bad("The parent bone does not exist");
    const BoneId id = rig.nextId;
    bone.id = id;
    auto st = edited(rig, [&](Rig& r) -> Status {
        r.bones.push_back(bone);
        r.nextId = id + 1;
        return {};
    });
    if (!st) return st.error();
    return id;
}

Status renameBone(Rig& rig, BoneId id, std::string name) {
    if (!rig.bone(id)) return bad("No such bone");
    return edited(rig, [&](Rig& r) -> Status {
        r.bone(id)->name = std::move(name);
        return {};
    });
}

Status setBoneGeometry(Rig& rig, BoneId id, double x, double y, double length, double angle, bool children) {
    const Bone* b = rig.bone(id);
    if (!b) return bad("No such bone");
    const double ox = b->x, oy = b->y, turn = angle - b->angle, mx = x - b->x, my = y - b->y;
    return edited(rig, [&](Rig& r) -> Status {
        if (children)
            for (BoneId c : r.subtree(id)) {
                if (c == id) continue;
                Bone* cb = r.bone(c);
                turnAbout(ox, oy, turn, cb->x, cb->y);
                cb->x += mx;
                cb->y += my;
                cb->angle += turn;
            }
        Bone* me = r.bone(id);
        me->x = x;
        me->y = y;
        me->length = length;
        me->angle = angle;
        return {};
    });
}

Status reparentBone(Rig& rig, BoneId id, BoneId newParent) {
    if (!rig.bone(id)) return bad("No such bone");
    if (newParent != kNoBone && !rig.bone(newParent)) return bad("The new parent bone does not exist");
    for (BoneId d : rig.subtree(id))
        if (d == newParent) return bad("A bone cannot become the child of itself or of a bone below it");
    return edited(rig, [&](Rig& r) -> Status {
        r.bone(id)->parent = newParent;
        // A chain whose tip loses its parent cannot be solved any more.
        if (newParent == kNoBone) std::erase_if(r.chains, [&](const IkChain& c) { return c.tip == id; });
        return sortBones(r.bones);
    });
}

Status removeBone(Rig& rig, BoneId id) {
    const Bone* b = rig.bone(id);
    if (!b) return bad("No such bone");
    const BoneId parent = b->parent;
    return edited(rig, [&](Rig& r) -> Status {
        for (Bone& c : r.bones)
            if (c.parent == id) c.parent = parent;
        std::erase_if(r.bones, [&](const Bone& x) { return x.id == id; });
        for (auto it = r.bindings.begin(); it != r.bindings.end();) {
            if (it->second != id) { ++it; continue; }
            if (parent != kNoBone) { it->second = parent; ++it; }
            else it = r.bindings.erase(it);
        }
        std::erase_if(r.chains, [&](const IkChain& c) {
            const Bone* t = r.bone(c.tip);
            return c.tip == id || !t || t->parent == kNoBone;
        });
        return sortBones(r.bones);
    });
}

Status setLimits(Rig& rig, BoneId id, bool limited, double minAngle, double maxAngle) {
    if (!rig.bone(id)) return bad("No such bone");
    return edited(rig, [&](Rig& r) -> Status {
        Bone* b = r.bone(id);
        b->limited = limited;
        b->minAngle = minAngle;
        b->maxAngle = maxAngle;
        return {};
    });
}

Status setLocked(Rig& rig, BoneId id, bool locked) {
    if (!rig.bone(id)) return bad("No such bone");
    rig.bone(id)->locked = locked;
    return {};
}

Status bindLayer(Rig& rig, LayerId layer, BoneId bone) {
    if (layer == 0) return bad("No layer");
    if (bone == kNoBone) {
        rig.bindings.erase(layer);
        return {};
    }
    if (!rig.bone(bone)) return bad("No such bone");
    rig.bindings[layer] = bone;
    return {};
}

Status addChain(Rig& rig, IkChain chain) {
    return edited(rig, [&](Rig& r) -> Status {
        r.chains.push_back(std::move(chain));
        return {};
    });
}

Status removeChain(Rig& rig, std::size_t index) {
    if (index >= rig.chains.size()) return bad("No such IK chain");
    rig.chains.erase(rig.chains.begin() + static_cast<std::ptrdiff_t>(index));
    return {};
}

double clampTurn(const Bone& bone, double turn) {
    if (bone.locked || !std::isfinite(turn)) return 0;
    if (bone.limited) return std::clamp(turn, bone.minAngle, bone.maxAngle);
    return turn;
}

std::vector<BoneWorld> solve(const Rig& rig, const RigPose& pose) {
    std::vector<BoneWorld> out(rig.bones.size());
    std::map<BoneId, std::size_t> at;
    for (std::size_t i = 0; i < rig.bones.size(); ++i) {
        const Bone& b = rig.bones[i];
        at[b.id] = i;
        const auto t = pose.turn.find(b.id);
        const double own = clampTurn(b, t == pose.turn.end() ? 0.0 : t->second);
        BoneWorld& w = out[i];
        if (b.parent == kNoBone || !at.count(b.parent)) {
            w.headX = b.x + pose.dx;
            w.headY = b.y + pose.dy;
            w.turn = own;
        } else {
            const Bone& p = rig.bones[at[b.parent]];
            const BoneWorld& pw = out[at[b.parent]];
            double hx = b.x, hy = b.y;
            turnAbout(p.x, p.y, pw.turn, hx, hy);
            w.headX = hx + (pw.headX - p.x);
            w.headY = hy + (pw.headY - p.y);
            w.turn = pw.turn + own;
        }
        w.tailX = w.headX + b.length * std::cos(b.angle + w.turn);
        w.tailY = w.headY + b.length * std::sin(b.angle + w.turn);
    }
    return out;
}

Status reach(const Rig& rig, RigPose& pose, std::size_t chain, double targetX, double targetY) {
    if (chain >= rig.chains.size()) return bad("No such IK chain");
    if (!std::isfinite(targetX) || !std::isfinite(targetY)) return bad("The target is not a number");
    const IkChain& c = rig.chains[chain];
    const Bone* tip = rig.bone(c.tip);
    const Bone* upper = tip ? rig.bone(tip->parent) : nullptr;
    if (!tip || !upper) return bad("IK chain '" + c.name + "' is broken");
    // The parent of the upper bone stays as posed.
    double baseTurn = 0;
    const std::vector<BoneWorld> world = solve(rig, pose);
    std::size_t ui = 0;
    for (std::size_t i = 0; i < rig.bones.size(); ++i)
        if (rig.bones[i].id == upper->id) ui = i;
    if (upper->parent != kNoBone)
        for (std::size_t i = 0; i < rig.bones.size(); ++i)
            if (rig.bones[i].id == upper->parent) baseTurn = world[i].turn;
    // Segment 1: upper head to tip head (they need not touch); segment 2: the tip.
    const double r1 = std::atan2(tip->y - upper->y, tip->x - upper->x);
    const double l1 = std::hypot(tip->x - upper->x, tip->y - upper->y);
    const double l2 = tip->length;
    const double r2 = tip->angle;
    const double hx = world[ui].headX, hy = world[ui].headY;
    const double tx = targetX - hx, ty = targetY - hy;
    const double base = std::atan2(ty, tx);
    double a1 = base;
    if (l1 > 1e-9 && l2 > 1e-9) {
        const double d = std::clamp(std::hypot(tx, ty), std::abs(l1 - l2) + 1e-9, l1 + l2 - 1e-9);
        const double cosA = std::clamp((l1 * l1 + d * d - l2 * l2) / (2 * l1 * d), -1.0, 1.0);
        a1 = base + c.bend * std::acos(cosA);
    }
    const double upperTurn = clampTurn(*upper, wrap(a1 - r1 - baseTurn));
    pose.turn[upper->id] = upperTurn;
    // Where the joint really is after the (possibly clamped) upper turn.
    const double ja = r1 + baseTurn + upperTurn;
    const double jx = hx + l1 * std::cos(ja), jy = hy + l1 * std::sin(ja);
    const double a2 = std::atan2(targetY - jy, targetX - jx);
    pose.turn[tip->id] = clampTurn(*tip, wrap(a2 - r2 - (baseTurn + upperTurn)));
    return {};
}

json toJson(const Rig& rig) {
    json bones = json::array();
    for (const Bone& b : rig.bones) {
        json jb = {{"id", b.id}, {"parent", b.parent}, {"name", b.name}, {"x", b.x}, {"y", b.y}, {"length", b.length}, {"angle", b.angle}};
        if (b.limited) jb["limits"] = {b.minAngle, b.maxAngle};
        if (b.locked) jb["locked"] = true;
        bones.push_back(std::move(jb));
    }
    json bindings = json::array();
    for (const auto& [layer, bone] : rig.bindings) bindings.push_back({{"layer", layer}, {"bone", bone}});
    json chains = json::array();
    for (const IkChain& c : rig.chains) chains.push_back({{"name", c.name}, {"tip", c.tip}, {"bend", c.bend}});
    return {{"version", Rig::kVersion}, {"nextId", rig.nextId}, {"bones", bones}, {"bindings", bindings}, {"chains", chains}};
}

namespace {
Error corrupt(std::string m) { return Error{ErrorCode::CorruptData, "Rig: " + std::move(m)}; }

template <typename T> bool getNumber(const json& j, const char* key, T& out) {
    if (!j.is_object() || !j.contains(key) || !j[key].is_number()) return false;
    out = j[key].get<T>();
    return true;
}
bool getId(const json& j, const char* key, std::uint32_t& out) {
    if (!j.is_object() || !j.contains(key) || !j[key].is_number_unsigned()) return false;
    const auto v = j[key].get<std::uint64_t>();
    if (v > 0xFFFFFFFFull) return false;
    out = static_cast<std::uint32_t>(v);
    return true;
}
} // namespace

Result<Rig> fromJson(const json& j) {
    if (!j.is_object()) return corrupt("not an object");
    int version = 0;
    if (!getNumber(j, "version", version)) return corrupt("no version");
    if (version < 1) return corrupt("invalid version");
    if (version > Rig::kVersion) return Error{ErrorCode::UnsupportedVersion, "This rig was saved by a newer AceStudio. Please update AceStudio."};
    Rig rig;
    if (!getId(j, "nextId", rig.nextId)) return corrupt("no nextId");
    for (const char* key : {"bones", "bindings", "chains"})
        if (!j.contains(key) || !j[key].is_array()) return corrupt(std::string("no ") + key + " list");
    if (j["bones"].size() > static_cast<std::size_t>(Rig::kMaxBones)) return corrupt("too many bones");
    if (j["chains"].size() > static_cast<std::size_t>(Rig::kMaxChains)) return corrupt("too many IK chains");
    if (j["bindings"].size() > static_cast<std::size_t>(Document::kMaxLayers)) return corrupt("too many parts");
    for (const json& jb : j["bones"]) {
        Bone b;
        if (!getId(jb, "id", b.id) || !getId(jb, "parent", b.parent)) return corrupt("a bone has no id or parent");
        if (!jb.contains("name") || !jb["name"].is_string()) return corrupt("a bone has no name");
        b.name = jb["name"].get<std::string>();
        if (!getNumber(jb, "x", b.x) || !getNumber(jb, "y", b.y) || !getNumber(jb, "length", b.length) || !getNumber(jb, "angle", b.angle))
            return corrupt("bone '" + b.name + "' lacks its geometry");
        if (jb.contains("limits")) {
            const json& l = jb["limits"];
            if (!l.is_array() || l.size() != 2 || !l[0].is_number() || !l[1].is_number()) return corrupt("bone '" + b.name + "' has bad limits");
            b.limited = true;
            b.minAngle = l[0].get<double>();
            b.maxAngle = l[1].get<double>();
        }
        if (jb.contains("locked")) {
            if (!jb["locked"].is_boolean()) return corrupt("bone '" + b.name + "' has a bad lock");
            b.locked = jb["locked"].get<bool>();
        }
        rig.bones.push_back(std::move(b));
    }
    for (const json& jb : j["bindings"]) {
        LayerId layer = 0;
        BoneId bone = 0;
        if (!getId(jb, "layer", layer) || !getId(jb, "bone", bone)) return corrupt("a part has no layer or bone");
        if (!rig.bindings.emplace(layer, bone).second) return corrupt("a layer is bound twice");
    }
    for (const json& jc : j["chains"]) {
        IkChain c;
        if (!jc.is_object() || !jc.contains("name") || !jc["name"].is_string() || !getId(jc, "tip", c.tip) || !getNumber(jc, "bend", c.bend))
            return corrupt("an IK chain is incomplete");
        c.name = jc["name"].get<std::string>();
        rig.chains.push_back(std::move(c));
    }
    if (auto st = validate(rig); !st) return corrupt(st.error().message);
    return rig;
}

} // namespace rigs
} // namespace acestudio
