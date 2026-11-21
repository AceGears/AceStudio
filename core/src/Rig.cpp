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

namespace {
// Skins whose part lost its binding or whose bone is no longer a child of
// the part's bone (after a bone is removed, reparented or the part rebound).
void dropBrokenSkins(Rig& r) {
    std::erase_if(r.skins, [&](const auto& kv) {
        const auto bound = r.bindings.find(kv.first);
        const Bone* c = r.bone(kv.second.bone);
        return bound == r.bindings.end() || !c || c->parent != bound->second;
    });
}
} // namespace

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
    for (const auto& [layer, sk] : rig.skins) {
        const auto bound = rig.bindings.find(layer);
        if (bound == rig.bindings.end()) return bad("A bent part is not bound to a bone");
        const Bone* child = rig.bone(sk.bone);
        if (!child || child->parent != bound->second) return bad("A bent part must bend with a child of its own bone");
        if (!finite(sk.blend) || sk.blend < 0 || sk.blend > kMaxCoord) return bad("A bent part's blend width is out of range");
    }
    if (rig.restFrame < 0 || rig.restFrame >= Document::kMaxFrames || (doc && rig.restFrame >= doc->frameCount()))
        return bad("The rest frame is not a frame of the document");
    if (rig.keys.size() > static_cast<std::size_t>(Rig::kMaxKeys)) return bad("A rig holds at most " + std::to_string(Rig::kMaxKeys) + " keys");
    for (std::size_t i = 0; i < rig.keys.size(); ++i) {
        const RigKey& k = rig.keys[i];
        if (k.frame < 0 || k.frame >= Document::kMaxFrames || (doc && k.frame >= doc->frameCount()))
            return bad("A key is on a frame the document does not have");
        if (i > 0 && rig.keys[i - 1].frame >= k.frame) return bad("Keys must be in frame order, one per frame");
        if (!finite(k.pose.dx) || !finite(k.pose.dy) || std::abs(k.pose.dx) > kMaxCoord || std::abs(k.pose.dy) > kMaxCoord)
            return bad("The key on frame " + std::to_string(k.frame) + " moves out of range");
        for (const auto& [b, t] : k.pose.turn)
            if (!seen.count(b) || !finite(t) || std::abs(t) > 64 * kPi)
                return bad("The key on frame " + std::to_string(k.frame) + " turns a missing bone or by an invalid angle");
        for (const auto& [b, s] : k.pose.stretch)
            if (!seen.count(b) || !finite(s) || s < Rig::kMinStretch || s > Rig::kMaxStretch)
                return bad("The key on frame " + std::to_string(k.frame) + " stretches a missing bone or by an invalid factor");
        const Curve& c = k.curve;
        for (double v : {c.x1, c.y1, c.x2, c.y2})
            if (!finite(v)) return bad("A key's curve is not a number");
        if (c.x1 < 0 || c.x1 > 1 || c.x2 < 0 || c.x2 > 1 || std::abs(c.y1) > 10 || std::abs(c.y2) > 10)
            return bad("A key's Bezier curve needs x in 0..1");
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
        dropBrokenSkins(r);
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
        for (RigKey& k : r.keys) {
            k.pose.turn.erase(id);
            k.pose.stretch.erase(id);
        }
        dropBrokenSkins(r);
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
        rig.skins.erase(layer);
        return {};
    }
    if (!rig.bone(bone)) return bad("No such bone");
    rig.bindings[layer] = bone;
    dropBrokenSkins(rig);
    return {};
}

Status skinLayer(Rig& rig, LayerId layer, BoneId child, double blend) {
    const auto bound = rig.bindings.find(layer);
    if (bound == rig.bindings.end()) return bad("Bind the part to a bone before bending it across a joint");
    if (child == kNoBone) {
        rig.skins.erase(layer);
        return {};
    }
    const Bone* c = rig.bone(child);
    if (!c) return bad("No such bone");
    if (c->parent != bound->second) return bad("A part bends with a child of its own bone ('" + c->name + "' is not one)");
    if (!finite(blend) || blend < 0 || blend > kMaxCoord) return bad("The blend width must be 0 (automatic) or more pixels");
    rig.skins[layer] = Skin{child, blend};
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

double clampStretch(double stretch) {
    if (!std::isfinite(stretch)) return 1;
    return std::clamp(stretch, Rig::kMinStretch, Rig::kMaxStretch);
}

std::vector<BoneWorld> solve(const Rig& rig, const RigPose& pose) {
    std::vector<BoneWorld> out(rig.bones.size());
    std::map<BoneId, std::size_t> at;
    for (std::size_t i = 0; i < rig.bones.size(); ++i) {
        const Bone& b = rig.bones[i];
        at[b.id] = i;
        const auto t = pose.turn.find(b.id);
        const double own = clampTurn(b, t == pose.turn.end() ? 0.0 : t->second);
        const auto st = pose.stretch.find(b.id);
        const double s = st == pose.stretch.end() ? 1.0 : clampStretch(st->second);
        BoneWorld& w = out[i];
        const BoneWorld* pw = nullptr;
        if (b.parent == kNoBone || !at.count(b.parent)) {
            w.headX = b.x + pose.dx;
            w.headY = b.y + pose.dy;
            w.turn = own;
        } else {
            const Bone& p = rig.bones[at[b.parent]];
            pw = &out[at[b.parent]];
            if (pw->stretched) {
                const double qx = b.x - p.x, qy = b.y - p.y;
                w.headX = pw->headX + pw->xx * qx + pw->xy * qy;
                w.headY = pw->headY + pw->yx * qx + pw->yy * qy;
            } else {
                double hx = b.x, hy = b.y;
                turnAbout(p.x, p.y, pw->turn, hx, hy);
                w.headX = hx + (pw->headX - p.x);
                w.headY = hy + (pw->headY - p.y);
            }
            w.turn = pw->turn + own;
        }
        w.stretched = s != 1.0 || (pw && pw->stretched);
        if (!w.stretched) {
            w.xx = std::cos(w.turn);
            w.yx = std::sin(w.turn);
            w.xy = -w.yx;
            w.yy = w.xx;
            w.tailX = w.headX + b.length * std::cos(b.angle + w.turn);
            w.tailY = w.headY + b.length * std::sin(b.angle + w.turn);
            continue;
        }
        // Its own map: longer along its rest direction, then its own turn;
        // after that, whatever its parent does.
        const double ca = std::cos(b.angle), sa = std::sin(b.angle);
        const double a00 = ca * ca * s + sa * sa, a01 = ca * sa * (s - 1), a11 = sa * sa * s + ca * ca;
        const double co = std::cos(own), so = std::sin(own);
        const double t00 = co * a00 - so * a01, t01 = co * a01 - so * a11;
        const double t10 = so * a00 + co * a01, t11 = so * a01 + co * a11;
        const double p00 = pw ? pw->xx : 1, p01 = pw ? pw->xy : 0, p10 = pw ? pw->yx : 0, p11 = pw ? pw->yy : 1;
        w.xx = p00 * t00 + p01 * t10;
        w.xy = p00 * t01 + p01 * t11;
        w.yx = p10 * t00 + p11 * t10;
        w.yy = p10 * t01 + p11 * t11;
        const double lx = b.length * ca, ly = b.length * sa;
        w.tailX = w.headX + w.xx * lx + w.xy * ly;
        w.tailY = w.headY + w.yx * lx + w.yy * ly;
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
    double l1 = std::hypot(tip->x - upper->x, tip->y - upper->y);
    double l2 = tip->length;
    const double r2 = tip->angle;
    const double hx = world[ui].headX, hy = world[ui].headY;
    // Stretched bones (RigPose::stretch, here or above): the lengths as
    // posed start the solve, and the turns are then refined against the
    // real tip below, since a stretched parent maps its children unevenly.
    std::size_t ti = 0;
    for (std::size_t i = 0; i < rig.bones.size(); ++i)
        if (rig.bones[i].id == tip->id) ti = i;
    const bool stretched = world[ti].stretched;
    if (stretched) {
        l1 = std::hypot(world[ti].headX - hx, world[ti].headY - hy);
        l2 = std::hypot(world[ti].tailX - world[ti].headX, world[ti].tailY - world[ti].headY);
    }
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
    if (!stretched) return {};
    // Damped least squares on the two turns: the tip's tail to the target
    // (or as near as the chain gets). A step is kept only if it comes nearer.
    auto miss = [&](double tu, double tt, double& ex, double& ey) {
        RigPose p = pose;
        p.turn[upper->id] = tu;
        p.turn[tip->id] = tt;
        const std::vector<BoneWorld> w = solve(rig, p);
        ex = w[ti].tailX - targetX;
        ey = w[ti].tailY - targetY;
        return ex * ex + ey * ey;
    };
    double tu = pose.turn[upper->id], tt = pose.turn[tip->id];
    double ex = 0, ey = 0;
    double err = miss(tu, tt, ex, ey);
    double damp = 1e-3;
    for (int it = 0; it < 40 && err > 1e-10; ++it) {
        constexpr double h = 1e-5;
        double ax = 0, ay = 0, bx = 0, by = 0;
        miss(tu + h, tt, ax, ay);
        miss(tu, tt + h, bx, by);
        const double j00 = (ax - ex) / h, j10 = (ay - ey) / h, j01 = (bx - ex) / h, j11 = (by - ey) / h;
        // (JtJ + damp) d = -Jt e
        const double m00 = j00 * j00 + j10 * j10 + damp, m01 = j00 * j01 + j10 * j11, m11 = j01 * j01 + j11 * j11 + damp;
        const double g0 = -(j00 * ex + j10 * ey), g1 = -(j01 * ex + j11 * ey);
        const double det = m00 * m11 - m01 * m01;
        if (!(std::abs(det) > 1e-18)) break;
        const double nu = clampTurn(*upper, tu + (g0 * m11 - g1 * m01) / det), nt = clampTurn(*tip, tt + (g1 * m00 - g0 * m01) / det);
        double nx = 0, ny = 0;
        const double nerr = miss(nu, nt, nx, ny);
        if (std::isfinite(nerr) && nerr < err) {
            const bool small = err - nerr < 1e-12;
            tu = nu;
            tt = nt;
            ex = nx;
            ey = ny;
            err = nerr;
            damp = std::max(1e-9, damp * 0.3);
            if (small) break;
        } else {
            damp *= 8;
            if (damp > 1e6) break;
        }
    }
    pose.turn[upper->id] = wrap(tu);
    pose.turn[tip->id] = wrap(tt);
    return {};
}

Status setKey(Rig& rig, int frame, RigPose pose, Curve curve) {
    return edited(rig, [&](Rig& r) -> Status {
        // Turns of 0 are the rest turn: kept out so keys stay small.
        std::erase_if(pose.turn, [](const auto& kv) { return kv.second == 0.0; });
        auto it = std::lower_bound(r.keys.begin(), r.keys.end(), frame, [](const RigKey& k, int f) { return k.frame < f; });
        if (it != r.keys.end() && it->frame == frame) *it = RigKey{frame, std::move(pose), curve};
        else r.keys.insert(it, RigKey{frame, std::move(pose), curve});
        return {};
    });
}

Status removeKey(Rig& rig, int frame) {
    const auto before = rig.keys.size();
    std::erase_if(rig.keys, [&](const RigKey& k) { return k.frame == frame; });
    if (rig.keys.size() == before) return bad("There is no key on frame " + std::to_string(frame));
    return {};
}

Status setRestFrame(Rig& rig, int frame) {
    return edited(rig, [&](Rig& r) -> Status {
        r.restFrame = frame;
        return {};
    });
}

double ease(const Curve& c, double t) {
    t = std::clamp(t, 0.0, 1.0);
    switch (c.type) {
    case CurveType::Linear: return t;
    case CurveType::EaseIn: return t * t * t;
    case CurveType::EaseOut: return 1 - (1 - t) * (1 - t) * (1 - t);
    case CurveType::EaseInOut: return t < 0.5 ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3) / 2;
    case CurveType::Stepped: return t >= 1 ? 1 : 0;
    case CurveType::Bezier: {
        // x(s) is monotonic for x1, x2 in [0, 1]: find s by bisection.
        auto bez = [](double p1, double p2, double s) {
            const double u = 1 - s;
            return 3 * u * u * s * p1 + 3 * u * s * s * p2 + s * s * s;
        };
        double lo = 0, hi = 1;
        for (int i = 0; i < 60; ++i) {
            const double mid = (lo + hi) / 2;
            if (bez(c.x1, c.x2, mid) < t) lo = mid;
            else hi = mid;
        }
        return bez(c.y1, c.y2, (lo + hi) / 2);
    }
    }
    return t;
}

RigPose poseAt(const Rig& rig, int frame) {
    if (rig.keys.empty()) return {};
    if (frame <= rig.keys.front().frame) return rig.keys.front().pose;
    if (frame >= rig.keys.back().frame) return rig.keys.back().pose;
    const auto next = std::upper_bound(rig.keys.begin(), rig.keys.end(), frame, [](int f, const RigKey& k) { return f < k.frame; });
    const RigKey& b = *next;
    const RigKey& a = *(next - 1);
    if (a.frame == frame) return a.pose;
    const double e = ease(a.curve, static_cast<double>(frame - a.frame) / (b.frame - a.frame));
    std::set<BoneId> ids;
    for (const auto& kv : a.pose.turn) ids.insert(kv.first);
    for (const auto& kv : b.pose.turn) ids.insert(kv.first);
    auto turnOf = [](const RigPose& p, BoneId id) {
        const auto it = p.turn.find(id);
        return it == p.turn.end() ? 0.0 : it->second;
    };
    RigPose out;
    for (BoneId id : ids) {
        const double ta = turnOf(a.pose, id), tb = turnOf(b.pose, id);
        out.turn[id] = ta + (tb - ta) * e;
    }
    ids.clear();
    for (const auto& kv : a.pose.stretch) ids.insert(kv.first);
    for (const auto& kv : b.pose.stretch) ids.insert(kv.first);
    auto stretchOf = [](const RigPose& p, BoneId id) {
        const auto it = p.stretch.find(id);
        return it == p.stretch.end() ? 1.0 : it->second;
    };
    for (BoneId id : ids) {
        const double sa = stretchOf(a.pose, id), sb = stretchOf(b.pose, id);
        out.stretch[id] = sa + (sb - sa) * e;
    }
    out.dx = a.pose.dx + (b.pose.dx - a.pose.dx) * e;
    out.dy = a.pose.dy + (b.pose.dy - a.pose.dy) * e;
    return out;
}

const char* toString(CurveType t) {
    switch (t) {
    case CurveType::Linear: return "linear";
    case CurveType::EaseIn: return "ease-in";
    case CurveType::EaseOut: return "ease-out";
    case CurveType::EaseInOut: return "ease-in-out";
    case CurveType::Stepped: return "stepped";
    case CurveType::Bezier: return "bezier";
    }
    return "linear";
}

std::optional<CurveType> curveFromString(const std::string& s) {
    for (CurveType t : {CurveType::Linear, CurveType::EaseIn, CurveType::EaseOut, CurveType::EaseInOut, CurveType::Stepped, CurveType::Bezier})
        if (s == toString(t)) return t;
    return std::nullopt;
}

namespace {
json poseJson(const RigPose& p) {
    json turns = json::array();
    for (const auto& [b, t] : p.turn) turns.push_back({{"bone", b}, {"angle", t}});
    json j = {{"turn", turns}, {"dx", p.dx}, {"dy", p.dy}};
    if (!p.stretch.empty()) {
        json stretches = json::array();
        for (const auto& [b, s] : p.stretch) stretches.push_back({{"bone", b}, {"factor", s}});
        j["stretch"] = stretches;
    }
    return j;
}
} // namespace

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
    json j = {{"version", 1}, {"nextId", rig.nextId}, {"bones", bones}, {"bindings", bindings}, {"chains", chains}};
    if (rig.restFrame != 0 || !rig.keys.empty()) {
        j["version"] = 2;
        j["restFrame"] = rig.restFrame;
        json keys = json::array();
        for (const RigKey& k : rig.keys) {
            json jc = {{"type", toString(k.curve.type)}};
            if (k.curve.type == CurveType::Bezier) jc["points"] = {k.curve.x1, k.curve.y1, k.curve.x2, k.curve.y2};
            keys.push_back({{"frame", k.frame}, {"pose", poseJson(k.pose)}, {"curve", jc}});
        }
        j["keys"] = keys;
    }
    if (!rig.skins.empty()) {
        j["version"] = 3;
        if (!j.contains("restFrame")) j["restFrame"] = rig.restFrame;
        json skins = json::array();
        for (const auto& [layer, sk] : rig.skins) skins.push_back({{"layer", layer}, {"bone", sk.bone}, {"blend", sk.blend}});
        j["skins"] = skins;
    }
    for (const RigKey& k : rig.keys)
        if (!k.pose.stretch.empty()) j["version"] = 4;
    return j;
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
    if (version >= 2) {
        if (j.contains("restFrame") && !getNumber(j, "restFrame", rig.restFrame)) return corrupt("bad rest frame");
        if (j.contains("keys")) {
            if (!j["keys"].is_array() || j["keys"].size() > static_cast<std::size_t>(Rig::kMaxKeys)) return corrupt("bad key list");
            for (const json& jk : j["keys"]) {
                RigKey k;
                if (!getNumber(jk, "frame", k.frame) || !jk.contains("pose") || !jk["pose"].is_object()) return corrupt("a key has no frame or pose");
                const json& jp = jk["pose"];
                if (!getNumber(jp, "dx", k.pose.dx) || !getNumber(jp, "dy", k.pose.dy) || !jp.contains("turn") || !jp["turn"].is_array())
                    return corrupt("a key's pose is incomplete");
                if (jp["turn"].size() > static_cast<std::size_t>(Rig::kMaxBones)) return corrupt("a key turns too many bones");
                for (const json& jt : jp["turn"]) {
                    BoneId b = 0;
                    double a = 0;
                    if (!getId(jt, "bone", b) || !getNumber(jt, "angle", a)) return corrupt("a key's turn is incomplete");
                    k.pose.turn[b] = a;
                }
                if (jp.contains("stretch")) {
                    if (!jp["stretch"].is_array() || jp["stretch"].size() > static_cast<std::size_t>(Rig::kMaxBones))
                        return corrupt("a key's stretch list is not a list");
                    for (const json& js : jp["stretch"]) {
                        BoneId b = 0;
                        double s = 1;
                        if (!getId(js, "bone", b) || !getNumber(js, "factor", s)) return corrupt("a key's stretch is incomplete");
                        k.pose.stretch[b] = s;
                    }
                }
                if (jk.contains("curve")) {
                    const json& jc = jk["curve"];
                    if (!jc.is_object() || !jc.contains("type") || !jc["type"].is_string()) return corrupt("a key's curve has no type");
                    const auto type = curveFromString(jc["type"].get<std::string>());
                    if (!type) return corrupt("unknown curve '" + jc["type"].get<std::string>() + "'");
                    k.curve.type = *type;
                    if (*type == CurveType::Bezier) {
                        if (!jc.contains("points") || !jc["points"].is_array() || jc["points"].size() != 4) return corrupt("a Bezier curve needs four numbers");
                        const json& pts = jc["points"];
                        for (const json& v : pts)
                            if (!v.is_number()) return corrupt("a Bezier curve needs four numbers");
                        k.curve.x1 = pts[0].get<double>();
                        k.curve.y1 = pts[1].get<double>();
                        k.curve.x2 = pts[2].get<double>();
                        k.curve.y2 = pts[3].get<double>();
                    }
                }
                rig.keys.push_back(std::move(k));
            }
        }
    }
    if (version >= 3 && j.contains("skins")) {
        if (!j["skins"].is_array() || j["skins"].size() > static_cast<std::size_t>(Rig::kMaxBones)) return corrupt("bad list of bent parts");
        for (const json& js : j["skins"]) {
            LayerId layer = 0;
            Skin sk;
            if (!getId(js, "layer", layer) || !getId(js, "bone", sk.bone) || !getNumber(js, "blend", sk.blend))
                return corrupt("a bent part has no layer, bone or blend");
            if (!rig.skins.emplace(layer, sk).second) return corrupt("a part is bent twice");
        }
    }
    if (auto st = validate(rig); !st) return corrupt(st.error().message);
    return rig;
}

} // namespace rigs
} // namespace acestudio
