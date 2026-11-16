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

Result<AutoRigResult> autoRig(const Document& doc, LayerId layer, int frame) {
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
    const motion::Subject subj = motion::analyzeSubject(sprite);
    auto built = motion::detail::buildGaitRig(sprite, subj, subj.body);
    if (!built)
        return bad(std::string("Auto-rig needs a body with legs; this sprite reads as ") + motion::toString(subj.body) +
                   ". Add bones by hand instead.");
    const motion::rig::Rig& parts = built->rig;
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

    // Bones. The root runs from the hips up through the body.
    Rig rig;
    const IntRect bb = subj.bounds;
    const double hipY = subj.legTop >= 0 ? subj.legTop : bb.y + bb.h;
    auto root = addBone(rig, boneFrom("body", kNoBone, canvas({bb.x + bb.w / 2.0, hipY}), canvas({bb.x + bb.w / 2.0, static_cast<double>(bb.y)})));
    if (!root) return root.error();
    auto bind = [&](int part, BoneId bone) -> Status {
        if (part < 0 || partLayer[static_cast<std::size_t>(part)] == kNoLayer) return {};
        return bindLayer(rig, partLayer[static_cast<std::size_t>(part)], bone);
    };
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
    for (std::size_t a = 0; a < built->armParts.size() && a < subj.arms.size(); ++a) {
        const int part = built->armParts[a];
        const motion::rig::Part& ap = parts.parts[static_cast<std::size_t>(part)];
        const motion::rig::Vec tip = farthest(ap.own, ap.pivot.x, ap.pivot.y);
        auto arm = addBone(rig, boneFrom("arm" + std::to_string(a), root.value(), canvas(ap.pivot), canvas(tip)));
        if (!arm) return arm.error();
        if (auto st = bind(part, arm.value()); !st) return st.error();
    }
    rig.restFrame = frame;
    if (auto st = validate(rig, &d); !st) return st.error();
    d.setRig(std::move(rig));
    if (auto st = d.validate(); !st) return st.error();

    out.notes.push_back(std::string("Body: ") + motion::toString(subj.body) + (built->front ? " seen from the front" : "") +
                        ", " + std::to_string(built->legs.size()) + " legs, " + std::to_string(built->armParts.size()) + " arms");
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
        const Image out = motion::rig::renderPose(one, {pp}, {0, 0, box.w, box.h});
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
