#include "acestudio/core/RigTools.h"
#include "acestudio/core/SpriteMotion.h"

#include "PartRig.h"
#include "RigModules.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace acestudio::rigs {

namespace {

constexpr double kPi = 3.14159265358979323846;

Error bad(std::string m) { return Error{ErrorCode::InvalidArgument, std::move(m)}; }

// A bone's move from rest: turn about its rest head, then put the head at
// `hx, hy`. Clockwise on screen (y down), as the rest of the rig.
struct BoneMove {
    // [xx xy; yx yy]: the turn, with any stretch (Rig.h BoneWorld).
    double xx = 1, xy = 0, yx = 0, yy = 1, px = 0, py = 0, hx = 0, hy = 0;
    BoneMove(const BoneWorld& w, double restX, double restY, double headX, double headY)
        : xx(w.xx), xy(w.xy), yx(w.yx), yy(w.yy), px(restX), py(restY), hx(headX), hy(headY) {}
    motion::rig::Vec apply(double x, double y) const {
        x -= px;
        y -= py;
        return {xx * x + xy * y + hx, yx * x + yy * y + hy};
    }
    motion::rig::Vec invert(double x, double y) const {
        x -= hx;
        y -= hy;
        const double det = xx * yy - xy * yx;   // 1 for a turn; a stretch is never 0
        return {(yy * x - xy * y) / det + px, (-yx * x + xx * y) / det + py};
    }
};

// A joint a skinned part bends at (in the part's own coordinates): the
// weight of the bone past it rises smoothly from 0 to 1 across a band
// `blend` wide either side of (x, y), measured along (nx, ny), the
// direction halfway between the two bones at rest.
struct SkinJoint {
    double x = 0, y = 0, nx = 1, ny = 0, blend = 1.5;
};

// A part bent along a chain of bones: `moves[0]` is its own bone, the
// others the bones below it in turn, `joints[i]` the joint between
// moves[i] and moves[i + 1]. With w_i the weight of joint i, a source point
// p goes to the sum of c_k M_k(p), c_0 = 1 - w_0, c_k = w_(k-1) - w_k, the
// last c = the last w: across one joint that is (1 - w) A(p) + w B(p).
// Drawn by inverse mapping: each output pixel solves for its source point
// by Newton's method, started from where each bone alone would put it (and
// half-way between neighbours); where the bent part folds over itself the
// point further down the chain (drawn on top) wins. Colours come from the
// part's 8x Scale2x copy, as rotated parts do.
Image renderSkinned(const motion::rig::Part& p, const std::vector<BoneMove>& moves, const std::vector<SkinJoint>& joints, int w, int h) {
    Image out(w, h);
    if (p.hiBox.empty() || moves.size() < 2 || joints.size() + 1 != moves.size()) return out;
    constexpr int kUp = 8;
    const std::size_t n = joints.size();
    auto weight = [&](const SkinJoint& j, double x, double y, double& dw) {
        const double k = ((x - j.x) * j.nx + (y - j.y) * j.ny + j.blend) / (2 * j.blend);
        if (k <= 0 || k >= 1) {
            dw = 0;
            return k <= 0 ? 0.0 : 1.0;
        }
        dw = 6 * k * (1 - k) / (2 * j.blend);
        return k * k * (3 - 2 * k);
    };
    // The output reaches no further than where the bones put the part.
    double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
    for (const BoneMove& m : moves)
        for (int k = 0; k < 4; ++k) {
            const motion::rig::Vec q = m.apply(k & 1 ? p.hiBox.x + p.hiBox.w : p.hiBox.x, k & 2 ? p.hiBox.y + p.hiBox.h : p.hiBox.y);
            x0 = std::min(x0, q.x);
            y0 = std::min(y0, q.y);
            x1 = std::max(x1, q.x);
            y1 = std::max(y1, q.y);
        }
    const int ix0 = std::max(0, static_cast<int>(std::floor(x0)) - 1), iy0 = std::max(0, static_cast<int>(std::floor(y0)) - 1);
    const int ix1 = std::min(w - 1, static_cast<int>(std::ceil(x1)) + 1), iy1 = std::min(h - 1, static_cast<int>(std::ceil(y1)) + 1);
    auto colourAt = [&](double x, double y) {
        const int hx = static_cast<int>(std::floor((x - p.hiBox.x) * kUp));
        const int hy = static_cast<int>(std::floor((y - p.hiBox.y) * kUp));
        return p.hi.contains(hx, hy) ? p.hi.pixel(hx, hy) : Rgba8{};
    };
    std::vector<double> wj(n), dwj(n);
    std::vector<motion::rig::Vec> at(n + 1), starts;
    for (int oy = iy0; oy <= iy1; ++oy)
        for (int ox = ix0; ox <= ix1; ++ox) {
            const double tx = ox + 0.5, ty = oy + 0.5;
            Rgba8 best{};
            double bestW = -1;
            // Each bone's own answer, then half-way between neighbours.
            starts.clear();
            for (const BoneMove& m : moves) starts.push_back(m.invert(tx, ty));
            for (std::size_t k = 0; k < n; ++k) starts.push_back({0.5 * (starts[k].x + starts[k + 1].x), 0.5 * (starts[k].y + starts[k + 1].y)});
            for (motion::rig::Vec q : starts) {
                bool ok = false;
                double wq = 0;
                for (int it = 0; it < 24; ++it) {
                    for (std::size_t k = 0; k < n; ++k) wj[k] = weight(joints[k], q.x, q.y, dwj[k]);
                    for (std::size_t k = 0; k <= n; ++k) at[k] = moves[k].apply(q.x, q.y);
                    // The blend and its Jacobian: the mixed maps, plus each
                    // change of weight carrying the point between two bones.
                    double px = 0, py = 0, j00 = 0, j01 = 0, j10 = 0, j11 = 0;
                    for (std::size_t k = 0; k <= n; ++k) {
                        const double c = (k == 0 ? 1.0 : wj[k - 1]) - (k == n ? 0.0 : wj[k]);
                        px += c * at[k].x;
                        py += c * at[k].y;
                        j00 += c * moves[k].xx;
                        j01 += c * moves[k].xy;
                        j10 += c * moves[k].yx;
                        j11 += c * moves[k].yy;
                    }
                    wq = 0;
                    for (std::size_t k = 0; k < n; ++k) {
                        const double ex = at[k + 1].x - at[k].x, ey = at[k + 1].y - at[k].y;
                        j00 += ex * dwj[k] * joints[k].nx;
                        j01 += ex * dwj[k] * joints[k].ny;
                        j10 += ey * dwj[k] * joints[k].nx;
                        j11 += ey * dwj[k] * joints[k].ny;
                        wq += wj[k];
                    }
                    const double gx = px - tx, gy = py - ty;
                    if (gx * gx + gy * gy < 1e-8) {
                        ok = true;
                        break;
                    }
                    const double det = j00 * j11 - j01 * j10;
                    if (std::abs(det) < 1e-9) break;
                    double sx = (j11 * gx - j01 * gy) / det, sy = (-j10 * gx + j00 * gy) / det;
                    const double len = std::hypot(sx, sy);
                    if (len > 4) {
                        sx *= 4 / len;
                        sy *= 4 / len;
                    }
                    q.x -= sx;
                    q.y -= sy;
                }
                if (!ok || wq <= bestW) continue;
                const Rgba8 c = colourAt(q.x, q.y);
                if (!c.a) continue;
                best = c;
                bestW = wq;
            }
            if (bestW >= 0) out.setPixel(ox, oy, best);
        }
    return out;
}

// The part's farthest pixel centre from (x, y), in canvas coordinates.
motion::rig::Vec farthest(const Mask& m, double x, double y) {
    motion::rig::Vec best{x, y};
    double bestD = -1;
    const IntRect b = m.bounds();
    for (int py = b.y; py < b.y + b.h; ++py)
        for (int px = b.x; px < b.x + b.w; ++px) {
            if (!m.get(px, py)) continue;
            const double d = std::hypot(px + 0.5 - x, py + 0.5 - y);
            if (d > bestD) {
                bestD = d;
                best = {px + 0.5, py + 0.5};
            }
        }
    return best;
}

// The centre of a mask's pixels.
motion::rig::Vec centroid(const Mask& m) {
    double sx = 0, sy = 0;
    double n = 0;   // double: a 64-bit long to double is a -Wconversion error on GCC
    const IntRect b = m.bounds();
    for (int y = b.y; y < b.y + b.h; ++y)
        for (int x = b.x; x < b.x + b.w; ++x)
            if (m.get(x, y)) {
                sx += x + 0.5;
                sy += y + 0.5;
                ++n;
            }
    return n > 0 ? motion::rig::Vec{sx / n, sy / n} : motion::rig::Vec{b.x + b.w / 2.0, b.y + b.h / 2.0};
}

Bone boneFrom(std::string name, BoneId parent, motion::rig::Vec head, motion::rig::Vec tail) {
    Bone b;
    b.name = std::move(name);
    b.parent = parent;
    b.x = head.x;
    b.y = head.y;
    b.length = std::hypot(tail.x - head.x, tail.y - head.y);
    b.angle = b.length > 0 ? std::atan2(tail.y - head.y, tail.x - head.x) : -kPi / 2;
    return b;
}

} // namespace

const char* toString(AutoRigBody b) {
    switch (b) {
    case AutoRigBody::Auto: return "auto";
    case AutoRigBody::Legs: return "legs";
    case AutoRigBody::Wings: return "wings";
    case AutoRigBody::Fins: return "fins";
    }
    return "auto";
}

std::optional<AutoRigBody> autoRigBodyFromString(const std::string& s) {
    for (AutoRigBody b : {AutoRigBody::Auto, AutoRigBody::Legs, AutoRigBody::Wings, AutoRigBody::Fins})
        if (s == toString(b)) return b;
    return std::nullopt;
}

Result<AutoRigResult> autoRig(const Document& doc, LayerId layer, int frame, AutoRigBody kind) {
    AutoRigOptions o;
    o.body = kind;
    return autoRig(doc, layer, frame, o);
}

Result<AutoRigResult> autoRig(const Document& doc, LayerId layer, int frame, const AutoRigOptions& opt) {
    const AutoRigBody kind = opt.body;
    if (doc.rig()) return bad("This document already has a rig. Remove it first to rig again.");
    const Layer* src = doc.layer(layer);
    if (!src || src->kind != LayerKind::Raster) return bad("Choose a raster layer to rig");
    if (frame < 0 || frame >= doc.frameCount()) return bad("Frame out of range");
    const Image whole = doc.renderLayer(layer, frame);
    const IntRect opaque = whole.opaqueBounds();
    if (opaque.empty()) return bad("The layer has no pixels on this frame");

    // Work on the sprite cropped with a margin, then move back to the canvas.
    const int pad = 2;
    const IntRect box{opaque.x - pad, opaque.y - pad, opaque.w + 2 * pad, opaque.h + 2 * pad};
    Image sprite(box.w, box.h);
    for (int y = 0; y < box.h; ++y)
        for (int x = 0; x < box.w; ++x) sprite.setPixel(x, y, whole.pixel(box.x + x, box.y + y));
    motion::Subject subj = motion::analyzeSubject(sprite);
    if (!opt.arms.empty()) {
        if (opt.arms.size() > 4) return bad("At most 4 arm boxes");
        if (kind == AutoRigBody::Wings || kind == AutoRigBody::Fins) return bad("Arm boxes are for legged rigs");
        subj.arms.clear();
        subj.armShoulders.clear();
        for (std::size_t i = 0; i < opt.arms.size(); ++i) {
            const IntRect& c = opt.arms[i];
            const std::string which = "Arm box " + std::to_string(i + 1);
            if (c.empty()) return bad(which + " is empty");
            for (std::size_t j = 0; j < i; ++j)
                if (!c.intersected(opt.arms[j]).empty()) return bad(which + " overlaps arm box " + std::to_string(j + 1));
            // In the cropped sprite's coordinates.
            const IntRect r = IntRect{c.x - box.x, c.y - box.y, c.w, c.h}.intersected({0, 0, box.w, box.h});
            int count = 0;
            double sx = 0, sy = 0;
            int touch = 0;
            for (int y = r.y; y < r.y + r.h; ++y)
                for (int x = r.x; x < r.x + r.w; ++x) {
                    if (sprite.pixel(x, y).a == 0) continue;
                    ++count;
                    // A pixel next to an opaque pixel outside the box joins the body.
                    bool joins = false;
                    for (const auto& [dx, dy] : {std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}}) {
                        const int nx = x + dx, ny = y + dy;
                        const bool inside = nx >= r.x && ny >= r.y && nx < r.x + r.w && ny < r.y + r.h;
                        if (!inside && nx >= 0 && ny >= 0 && nx < box.w && ny < box.h && sprite.pixel(nx, ny).a > 0) joins = true;
                    }
                    if (joins) {
                        sx += x + 0.5;
                        sy += y + 0.5;
                        ++touch;
                    }
                }
            if (count == 0) return bad(which + " holds no pixels of the layer");
            subj.arms.push_back(r);
            subj.armShoulders.emplace_back(touch ? sx / touch : r.x + r.w / 2.0, touch ? sy / touch : r.y + 0.5);
        }
    }
    // Legs first (the gait rig); else wings for a flyer, fins for a swimmer:
    // the same cuts the animator's part rig uses (ADR-ANIM-001).
    std::optional<motion::detail::GaitRig> built;
    std::optional<motion::detail::FlightRig> flight;
    std::optional<motion::detail::SwimRig> swim;
    const bool any = kind == AutoRigBody::Auto;
    if (any || kind == AutoRigBody::Legs) built = motion::detail::buildGaitRig(sprite, subj, subj.body);
    if (!built && (kind == AutoRigBody::Wings || (any && subj.body == motion::Body::Flyer)))
        flight = motion::detail::buildFlightRig(sprite, subj, motion::detail::WingSpec{}, false);
    if (!built && !flight && (kind == AutoRigBody::Fins || (any && subj.body == motion::Body::Swimmer)))
        swim = motion::detail::buildSwimRig(sprite, subj, motion::SwimStyle::Auto);
    if (!built && !opt.arms.empty()) return bad("Arm boxes need legs, and no legs were found; add the bones by hand instead.");
    if (!built && !flight && !swim) {
        const std::string reads = std::string("this sprite reads as ") + motion::toString(subj.body);
        switch (kind) {
        case AutoRigBody::Legs: return bad("No legs were found; " + reads + ". Add bones by hand instead.");
        case AutoRigBody::Wings: return bad("No wings were found above the wing line. Add bones by hand instead.");
        case AutoRigBody::Fins: return bad("The sprite is too small to cut into fins. Add bones by hand instead.");
        case AutoRigBody::Auto: break;
        }
        return bad("Auto-rig needs a body with legs, wings or fins; " + reads + ". Choose wings or fins, or add bones by hand.");
    }
    const motion::rig::Rig& parts = built ? built->rig : flight ? flight->rig : swim->rig;
    const double ox = box.x, oy = box.y;
    auto canvas = [&](motion::rig::Vec v) { return motion::rig::Vec{v.x + ox, v.y + oy}; };

    AutoRigResult out{doc, kNoLayer, {}};
    Document& d = out.document;
    // The group goes right above the source layer, in the same parent.
    auto group = d.addLayer(src->name + " rig", LayerKind::Group, src->parent);
    if (!group) return group.error();
    if (auto st = d.moveLayer(group.value(), *d.layerIndex(layer) + 1); !st) return st.error();
    out.group = group.value();

    // Part layers, back to front (the part rig's draw order).
    std::vector<std::size_t> order(parts.parts.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return parts.parts[a].z < parts.parts[b].z; });
    std::vector<LayerId> partLayer(parts.parts.size(), kNoLayer);
    for (std::size_t i : order) {
        const motion::rig::Part& p = parts.parts[i];
        const IntRect pb = p.pixels.opaqueBounds();
        if (pb.empty()) continue;
        std::string name = p.name;
        std::replace(name.begin(), name.end(), '_', ' ');
        auto id = d.addLayer(name, LayerKind::Raster, out.group);
        if (!id) return id.error();
        if (auto st = d.setCel(id.value(), frame, Cel{p.pixels.copy(pb), pb.x + box.x, pb.y + box.y}); !st) return st.error();
        partLayer[i] = id.value();
    }
    d.layer(layer)->visible = false;

    Rig rig;
    auto bind = [&](int part, BoneId bone) -> Status {
        if (part < 0 || partLayer[static_cast<std::size_t>(part)] == kNoLayer) return {};
        return bindLayer(rig, partLayer[static_cast<std::size_t>(part)], bone);
    };
    // A bone from a part's pivot to its farthest pixel, bound to the part.
    auto limbBone = [&](const std::string& name, int part, BoneId parent) -> Status {
        if (part < 0) return {};
        const motion::rig::Part& p = parts.parts[static_cast<std::size_t>(part)];
        auto id = addBone(rig, boneFrom(name, parent, canvas(p.pivot), canvas(farthest(p.own, p.pivot.x, p.pivot.y))));
        if (!id) return id.error();
        return bind(part, id.value());
    };
    std::string summary;
    if (built) {
        // The root runs from the hips up through the body.
        const IntRect bb = subj.bounds;
        const double hipY = subj.legTop >= 0 ? subj.legTop : bb.y + bb.h;
        auto root = addBone(rig, boneFrom("body", kNoBone, canvas({bb.x + bb.w / 2.0, hipY}), canvas({bb.x + bb.w / 2.0, static_cast<double>(bb.y)})));
        if (!root) return root.error();
        for (int part : {built->bodyPart, built->shoulderPart, built->pelvisPart})
            if (auto st = bind(part, root.value()); !st) return st.error();

        const int dir = built->dir;
        for (std::size_t i = 0; i < built->legs.size(); ++i) {
            const motion::detail::GaitLeg& L = built->legs[i];
            const std::string nm = "leg" + std::to_string(i);
            const bool hasShin = L.shin >= 0 && partLayer[static_cast<std::size_t>(L.shin)] != kNoLayer && !built->stubby;
            auto thigh = addBone(rig, boneFrom(nm + " thigh", root.value(), canvas(L.hip), canvas(L.knee)));
            if (!thigh) return thigh.error();
            if (auto st = bind(L.thigh, thigh.value()); !st) return st.error();
            BoneId lower = thigh.value();
            if (hasShin) {
                auto shin = addBone(rig, boneFrom(nm + " shin", thigh.value(), canvas(L.knee), canvas(L.ankle)));
                if (!shin) return shin.error();
                if (auto st = bind(L.shin, shin.value()); !st) return st.error();
                lower = shin.value();
            }
            // The foot runs from the ankle to its farthest pixel (the toe).
            if (L.foot >= 0) {
                const motion::rig::Part& fp = parts.parts[static_cast<std::size_t>(L.foot)];
                const motion::rig::Vec toe = farthest(fp.own, L.ankle.x, L.ankle.y);
                auto foot = addBone(rig, boneFrom(nm + " foot", lower, canvas(L.ankle), canvas(toe)));
                if (!foot) return foot.error();
                if (auto st = bind(L.foot, foot.value()); !st) return st.error();
                if (!hasShin) lower = foot.value();
            }
            // The knee bends to the side it is drawn on (straight legs: forward).
            if (lower != thigh.value()) {
                const Bone* tip = rig.bone(lower);
                const double ax = tip->tailX() - (L.hip.x + ox), ay = tip->tailY() - (L.hip.y + oy);
                const double kx = L.knee.x - L.hip.x, ky = L.knee.y - L.hip.y;
                const double cross = ax * ky - ay * kx;
                const int bend = std::abs(cross) > 1e-6 ? (cross > 0 ? 1 : -1) : -dir;
                if (auto st = addChain(rig, IkChain{nm + " IK", lower, bend}); !st) return st.error();
            }
        }
        for (std::size_t a = 0; a < built->armParts.size() && a < subj.arms.size(); ++a)
            if (auto st = limbBone("arm" + std::to_string(a), built->armParts[a], root.value()); !st) return st.error();
        // A four-legged side view: the body beyond the front legs (the way it
        // faces) is the head, beyond the hind legs the tail, each a part of
        // its own on a bone from where it leaves the body, so a motion can
        // nod the head and swing the tail. Too small an end stays on the body.
        int ends = 0, bent = 0;
        if (subj.body == motion::Body::Quadruped && !built->front && built->bodyPart >= 0 &&
            partLayer[static_cast<std::size_t>(built->bodyPart)] != kNoLayer) {
            const LayerId bodyLayer = partLayer[static_cast<std::size_t>(built->bodyPart)];
            int legMin = 1 << 30, legMax = -(1 << 30);
            for (const auto& L : built->legs) {
                if (L.thigh < 0 || partLayer[static_cast<std::size_t>(L.thigh)] == kNoLayer) continue;
                const Cel* c = d.cel(partLayer[static_cast<std::size_t>(L.thigh)], frame);
                if (!c) continue;
                legMin = std::min(legMin, c->x);
                legMax = std::max(legMax, c->x + c->image.width());
            }
            // The body's outline per column: its top and bottom rows. The
            // trunk is the run of columns over the legs; its back is the top
            // row and its belly the bottom row most of those columns share.
            std::map<int, std::pair<int, int>> col;   // x -> (top, bottom)
            if (const Cel* bc = d.cel(bodyLayer, frame))
                for (int y = 0; y < bc->image.height(); ++y)
                    for (int x = 0; x < bc->image.width(); ++x)
                        if (bc->image.pixel(x, y).a) {
                            auto [it, fresh] = col.try_emplace(bc->x + x, bc->y + y, bc->y + y);
                            if (!fresh) {
                                it->second.first = std::min(it->second.first, bc->y + y);
                                it->second.second = std::max(it->second.second, bc->y + y);
                            }
                        }
            std::map<int, int> tops, bottoms;
            for (int x = legMin; x < legMax; ++x)
                if (auto c = col.find(x); c != col.end()) {
                    ++tops[c->second.first];
                    ++bottoms[c->second.second];
                }
            auto mode = [](const std::map<int, int>& m) {
                int best = 0, at = 0;
                for (const auto& [v, n] : m)
                    if (n > best) best = n, at = v;
                return at;
            };
            const int backTop = mode(tops), belly = mode(bottoms);
            for (int end = 0; end < 2 && legMin < legMax && !col.empty(); ++end) {
                const bool head = end == 0;
                const int outward = head ? dir : -dir;   // the way outwards from the legs
                // E: the first column outwards from the legs whose bottom has
                // left the belly line (the neck, or the tail's root).
                int scan = outward > 0 ? legMax : legMin - 1;
                while (col.count(scan) && col[scan].second >= belly - 1) scan += outward;
                if (!col.count(scan)) continue;
                const int edgeX = scan;
                // Inwards from E, the columns rising above the back (the top
                // of a head, a raised tail) belong to the end above the back line.
                int innerX = edgeX;
                while (col.count(innerX - outward) && col[innerX - outward].first < backTop) innerX -= outward;
                auto inEnd = [&](int gx, int gy) {
                    if ((gx - edgeX) * outward >= 0) return true;
                    return (gx - innerX) * outward >= 0 && gy < backTop;
                };
                Cel* bc = d.cel(bodyLayer, frame);
                if (!bc) break;
                int total = 0;
                IntRect r{1 << 30, 1 << 30, 0, 0};
                int x1 = -(1 << 30), y1 = -(1 << 30);
                double rowSum = 0;
                int rowCount = 0;
                const int edge = edgeX;
                for (int y = 0; y < bc->image.height(); ++y)
                    for (int x = 0; x < bc->image.width(); ++x) {
                        if (bc->image.pixel(x, y).a == 0) continue;
                        ++total;
                        const int gx = bc->x + x, gy = bc->y + y;
                        if (!inEnd(gx, gy)) continue;
                        r.x = std::min(r.x, gx);
                        r.y = std::min(r.y, gy);
                        x1 = std::max(x1, gx + 1);
                        y1 = std::max(y1, gy + 1);
                        if (gx == edge) {
                            rowSum += gy + 0.5;
                            ++rowCount;
                        }
                    }
                r.w = x1 - r.x;
                r.h = y1 - r.y;
                if (r.w < 2 || rowCount == 0) continue;
                Image piece(r.w, r.h);
                int n = 0;
                for (int y = 0; y < r.h; ++y)
                    for (int x = 0; x < r.w; ++x) {
                        const int bx = r.x + x - bc->x, by = r.y + y - bc->y;
                        const Rgba8 px = bc->image.pixel(bx, by);
                        if (px.a == 0) continue;
                        piece.setPixel(x, y, px);
                        ++n;
                    }
                if (n < 6 || n * 50 < total) continue;   // under 2% of the body: leave it
                for (int y = 0; y < r.h; ++y)
                    for (int x = 0; x < r.w; ++x)
                        if (piece.pixel(x, y).a) bc->image.setPixel(r.x + x - bc->x, r.y + y - bc->y, Rgba8::transparent());
                const std::string nm = head ? "head" : "tail";
                auto id = d.addLayer(nm, LayerKind::Raster, out.group);
                if (!id) return id.error();
                if (auto st = d.moveLayer(id.value(), *d.layerIndex(bodyLayer) + 1); !st) return st.error();
                if (auto st = d.setCel(id.value(), frame, Cel{piece, r.x, r.y}); !st) return st.error();
                // The bone: from where the end leaves the body to its farthest pixel.
                // The pivot: where the end leaves the body, at the middle of its first column.
                const motion::rig::Vec pivot{outward > 0 ? static_cast<double>(edgeX) : static_cast<double>(edgeX + 1), rowSum / rowCount};
                motion::rig::Vec tip = pivot;
                double best = -1;
                for (int y = 0; y < r.h; ++y)
                    for (int x = 0; x < r.w; ++x)
                        if (piece.pixel(x, y).a) {
                            const double dx = r.x + x + 0.5 - pivot.x, dy = r.y + y + 0.5 - pivot.y;
                            if (dx * dx + dy * dy > best) {
                                best = dx * dx + dy * dy;
                                tip = {r.x + x + 0.5, r.y + y + 0.5};
                            }
                        }
                // A long end (a neck, a long tail: at least 8 px and 2.5 times
                // its mean thickness) gets a second bone from its middle on,
                // and the part bends across that joint: "neck" then "head",
                // "tail" then "tail tip".
                const double reachLen = std::sqrt(std::max(best, 0.0));
                const bool bends = reachLen >= 8 && reachLen * reachLen >= 2.5 * n;
                motion::rig::Vec mid{(pivot.x + tip.x) / 2, (pivot.y + tip.y) / 2};
                if (bends) {
                    // The joint: the middle of the art half-way along, not of the chord (a raised tail curves).
                    const double ux = (tip.x - pivot.x) / reachLen, uy = (tip.y - pivot.y) / reachLen;
                    double sx = 0, sy = 0;
                    int cnt = 0;
                    for (int y = 0; y < r.h; ++y)
                        for (int x = 0; x < r.w; ++x)
                            if (piece.pixel(x, y).a) {
                                const double px = r.x + x + 0.5, py = r.y + y + 0.5;
                                if (std::abs((px - pivot.x) * ux + (py - pivot.y) * uy - reachLen / 2) > 1.0) continue;
                                sx += px;
                                sy += py;
                                ++cnt;
                            }
                    if (cnt) mid = {sx / cnt, sy / cnt};
                }
                auto bone = addBone(rig, boneFrom(bends && head ? "neck" : nm, root.value(), pivot, bends ? mid : tip));
                if (!bone) return bone.error();
                if (auto st = bindLayer(rig, id.value(), bone.value()); !st) return st.error();
                if (bends) {
                    auto tipBone = addBone(rig, boneFrom(head ? "head" : "tail tip", bone.value(), mid, tip));
                    if (!tipBone) return tipBone.error();
                    // A wide band (a quarter of the length either side), so the bend reads as a curve.
                    if (auto st = skinLayer(rig, id.value(), tipBone.value(), reachLen / 4); !st) return st.error();
                    ++bent;
                }
                ++ends;
            }
        }
        summary = std::string("Body: ") + motion::toString(subj.body) + (built->front ? " seen from the front" : "") + ", " +
                  std::to_string(built->legs.size()) + " legs, " + std::to_string(built->armParts.size()) + " arms" +
                  (ends == 2 ? ", a head and a tail" : ends == 1 ? ", one end (head or tail)" : "") +
                  (bent ? " (" + std::to_string(bent) + " long, bending in the middle)" : "");
    } else if (flight) {
        // The root runs from the body's middle towards the head (up when the
        // head side is unknown); the wings turn about their shoulders.
        const motion::rig::Vec mid = centroid(flight->bodyOnly);
        const IntRect bb = subj.bounds;
        const motion::rig::Vec toward = flight->headSide > 0   ? motion::rig::Vec{static_cast<double>(bb.x + bb.w), mid.y}
                                        : flight->headSide < 0 ? motion::rig::Vec{static_cast<double>(bb.x), mid.y}
                                                               : motion::rig::Vec{mid.x, static_cast<double>(bb.y)};
        auto root = addBone(rig, boneFrom("body", kNoBone, canvas(mid), canvas(toward)));
        if (!root) return root.error();
        for (int part : {flight->bodyPart, flight->chestPart})
            if (auto st = bind(part, root.value()); !st) return st.error();
        for (const motion::detail::FlightWing& w : flight->wings)
            if (auto st = limbBone(flight->wings.size() == 1 ? std::string("wing") : w.nearWing ? "near wing" : "far wing", w.part, root.value()); !st)
                return st.error();
        summary = "Body: flyer, " + std::to_string(flight->wings.size()) + (flight->wings.size() == 1 ? " wing" : " wings");
    } else {
        // The root runs from the tail stalk to the head; the tail fin turns
        // at the stalk, every other fin at its root.
        const IntRect bb = subj.bounds;
        const motion::rig::Vec mid = centroid(parts.parts[static_cast<std::size_t>(swim->bodyPart)].own);
        const motion::rig::Vec head{swim->dir > 0 ? static_cast<double>(bb.x) : static_cast<double>(bb.x + bb.w), mid.y};
        const motion::rig::Vec back = swim->tailPart >= 0 ? swim->stalkPt : motion::rig::Vec{swim->dir > 0 ? static_cast<double>(bb.x + bb.w) : static_cast<double>(bb.x), mid.y};
        auto root = addBone(rig, boneFrom("body", kNoBone, canvas(back), canvas(head)));
        if (!root) return root.error();
        if (auto st = bind(swim->bodyPart, root.value()); !st) return st.error();
        if (auto st = limbBone("tail fin", swim->tailPart, root.value()); !st) return st.error();
        int top = 0, bottom = 0;
        for (const motion::detail::SwimFin& f : swim->fins) {
            const int n = f.top ? ++top : ++bottom;
            const std::string name = std::string(f.top ? "dorsal fin" : "ventral fin") + (n > 1 ? " " + std::to_string(n) : std::string());
            if (auto st = limbBone(name, f.part, root.value()); !st) return st.error();
        }
        summary = std::string("Body: swimmer, ") + (swim->tailPart >= 0 ? "a tail fin, " : "no tail fin, ") + std::to_string(swim->fins.size()) +
                  " other fins (the body is one rigid part)";
    }
    rig.restFrame = frame;
    if (auto st = validate(rig, &d); !st) return st.error();
    d.setRig(std::move(rig));
    if (auto st = d.validate(); !st) return st.error();

    out.notes.push_back(summary);
    out.notes.push_back(std::to_string(d.rig()->bones.size()) + " bones, " + std::to_string(d.rig()->bindings.size()) +
                        " part layers, " + std::to_string(d.rig()->chains.size()) + " IK chains");
    out.notes.push_back("Parts were cut on frame " + std::to_string(frame) + " only; '" + src->name + "' is hidden, not deleted");
    if (src->opacity != 255 || src->blend != BlendMode::Normal || src->hasMask)
        out.notes.push_back("The source layer's opacity, blend mode and mask were not carried to the parts");
    return out;
}

Result<Document> bakePose(const Document& doc, int restFrame, int frame, const RigPose& pose) {
    if (!doc.rig()) return bad("The document has no rig");
    if (frame < 0 || frame >= doc.frameCount() || restFrame < 0 || restFrame >= doc.frameCount()) return bad("Frame out of range");
    const Rig& rig = *doc.rig();
    const std::vector<BoneWorld> world = solve(rig, pose);
    Document d = doc;
    const int w = doc.width(), h = doc.height();
    // Parts may move off the canvas; cels can hold that.
    const int m = std::min(std::max(w, h), 256);
    const IntRect box{-m, -m, w + 2 * m, h + 2 * m};
    for (const auto& [layerId, boneId] : rig.bindings) {
        const Cel* c = doc.cel(layerId, restFrame);
        if (d.linkedFrames(layerId, frame).size() > 1) d.unlinkCels(layerId, {frame});
        if (!c) {
            d.clearCel(layerId, frame);
            continue;
        }
        std::size_t bi = 0;
        while (bi < rig.bones.size() && rig.bones[bi].id != boneId) ++bi;
        if (bi == rig.bones.size()) continue;
        const Bone& b = rig.bones[bi];
        const BoneWorld& bw = world[bi];
        // One part in the box's coordinates.
        motion::rig::Rig one;
        one.width = box.w;
        one.height = box.h;
        motion::rig::Part p;
        p.name = "part";
        p.pixels = Image(box.w, box.h);
        p.pixels.blit(c->image, c->x - box.x, c->y - box.y);
        p.own = Mask(box.w, box.h);
        const IntRect cb = c->image.opaqueBounds();
        for (int y = cb.y; y < cb.y + cb.h; ++y)
            for (int x = cb.x; x < cb.x + cb.w; ++x)
                if (c->image.pixel(x, y).a) p.own.set(x + c->x - box.x, y + c->y - box.y, true);
        p.pivot = {b.x - box.x, b.y - box.y};
        one.parts.push_back(std::move(p));
        motion::rig::prepare(one);
        motion::rig::PartPose pp;
        pp.dx = bw.headX - b.x;
        pp.dy = bw.headY - b.y;
        pp.angle = bw.turn;
        if (bw.stretched) {
            // The bone's map as a part pose: wider and taller with a lean, then a turn.
            pp.angle = std::atan2(bw.yx, bw.xx);
            const double ca = std::cos(pp.angle), sa = std::sin(pp.angle);
            pp.sx = std::hypot(bw.xx, bw.yx);
            pp.sy = -sa * bw.xy + ca * bw.yy;
            pp.shear = std::abs(pp.sy) > 1e-9 ? (ca * bw.xy + sa * bw.yy) / pp.sy : 0;
        }
        // A part bent across a joint (or along a chain of them), unless
        // its bones turn together (then it is rigid, drawn exactly as other parts).
        std::vector<std::size_t> chain;   // the bones below its own that it follows
        if (const auto sk = rig.skins.find(layerId); sk != rig.skins.end())
            for (BoneId id : skinChain(rig, b.id, sk->second.bone))
                for (std::size_t k = 0; k < rig.bones.size(); ++k)
                    if (rig.bones[k].id == id) chain.push_back(k);
        bool bent = false;
        {
            const BoneWorld* before = &bw;
            for (std::size_t ci : chain) {
                const BoneWorld& cw = world[ci];
                bent = bent || std::abs(std::remainder(cw.turn - before->turn, 2 * kPi)) > 1e-9;
                if (before->stretched || cw.stretched)
                    bent = bent || std::max({std::abs(cw.xx - before->xx), std::abs(cw.xy - before->xy), std::abs(cw.yx - before->yx),
                                             std::abs(cw.yy - before->yy)}) > 1e-9;
                before = &cw;
            }
        }
        Image out;
        if (bent) {
            // Whole-pixel moves as rigid parts make, the bones below keeping
            // their exact places relative to the part's own.
            const double rx = std::round(pp.dx) - pp.dx, ry = std::round(pp.dy) - pp.dy;
            std::vector<BoneMove> moves;
            std::vector<SkinJoint> joints;
            moves.emplace_back(bw, b.x - box.x, b.y - box.y, bw.headX + rx - box.x, bw.headY + ry - box.y);
            const Bone* above = &b;
            for (std::size_t ci : chain) {
                const Bone& kid = rig.bones[ci];
                const BoneWorld& cw = world[ci];
                moves.emplace_back(cw, kid.x - box.x, kid.y - box.y, cw.headX + rx - box.x, cw.headY + ry - box.y);
                double nx = std::cos(above->angle) + std::cos(kid.angle), ny = std::sin(above->angle) + std::sin(kid.angle);
                if (std::hypot(nx, ny) < 1e-6) {
                    nx = std::cos(above->angle);
                    ny = std::sin(above->angle);
                }
                const double nl = std::hypot(nx, ny);
                double blend = rig.skins.at(layerId).blend;
                if (blend <= 0) blend = std::max(1.5, 0.2 * std::min(above->length, kid.length));
                joints.push_back({kid.x - box.x, kid.y - box.y, nx / nl, ny / nl, blend});
                above = &kid;
            }
            out = renderSkinned(one.parts[0], moves, joints, box.w, box.h);
        } else {
            out = motion::rig::renderPose(one, {pp}, {0, 0, box.w, box.h});
        }
        const IntRect ob = out.opaqueBounds();
        if (ob.empty()) {
            d.clearCel(layerId, frame);
            continue;
        }
        if (auto st = d.setCel(layerId, frame, Cel{out.copy(ob), ob.x + box.x, ob.y + box.y}); !st) return st.error();
    }
    return d;
}

Result<Document> bakeKeys(const Document& doc, int first, int last) {
    if (!doc.rig()) return bad("The document has no rig");
    const Rig& rig = *doc.rig();
    if (rig.keys.empty()) return bad("The rig has no keys: set a key on a frame first");
    if (first > last) {
        first = rig.keys.front().frame;
        last = rig.keys.back().frame;
    }
    if (first < 0 || last >= doc.frameCount()) return bad("Frame range out of range");
    Document d = doc;
    for (int f = first; f <= last; ++f) {
        if (f == rig.restFrame) continue;
        auto baked = bakePose(d, rig.restFrame, f, poseAt(rig, f));
        if (!baked) return baked.error();
        d = std::move(baked).value();
    }
    return d;
}

} // namespace acestudio::rigs
