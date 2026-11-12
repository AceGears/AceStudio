// Swimming on the part rig (ADR-ANIM-001 step 5c).
//
// A fish seen from the side is cut into the body, the tail fin (everything
// past the narrow tail stalk) and the other fins (thin parts sticking out of
// the body's top and bottom edges). Then:
//  - the body bends with a wave that runs from head to tail. Every column of
//    the body moves up or down by whole pixels and neighbouring columns never
//    part by more than one pixel, so the outline stays closed. No column is
//    dropped or doubled: the body keeps its length (the older renderer
//    showed the sideways bend by dropping columns and shortened the fish by
//    up to a tenth);
//  - the bend grows from the head (nearly still) to the tail stalk, by the
//    swimming mode's envelope (anguilliform: the whole body; carangiform:
//    the rear third; thunniform: only the stalk);
//  - the tail fin trails the stalk by a fraction of a beat, turns about the
//    stalk and narrows as it sweeps towards or away from the viewer
//    (foreshortening), so it reads as beating sideways;
//  - every other fin paddles on its own phase about where it joins the
//    body; the fins are drawn behind the body, so their roots stay hidden.
// A faster swim beats more often per loop, not just with shorter frames.

#include "RigModules.h"

#include "PartRig.h"

#include <algorithm>
#include <cmath>

namespace acestudio::motion::detail {

namespace {

constexpr double kPi = 3.14159265358979323846;

int iround(double v) { return static_cast<int>(std::lround(v)); }

} // namespace

std::optional<RigClip> swimRigRender(const Image& sprite, const Subject& subj, Action action, int frames, double amount,
                                     SwimStyle style) {
    if (action != Action::Idle && action != Action::Swim && action != Action::Dash) return std::nullopt;
    const IntRect bb = subj.bounds;
    if (bb.empty() || frames < 2 || bb.w < 8 || bb.h < 4) return std::nullopt;
    amount = std::clamp(amount, 0.25, 2.0);
    const int sw = sprite.width(), sh = sprite.height();
    const Mask opaque = rig::opaqueMask(sprite);
    const bool headLeft = subj.headLeft;
    const int dir = headLeft ? 1 : -1;                       // head -> tail, in x
    const int headX = headLeft ? bb.x : bb.x + bb.w - 1;
    auto along = [&](int x) { return (x - headX) * dir; };   // 0 at the head

    // Column heights from head to tail.
    std::vector<int> colH(static_cast<std::size_t>(bb.w), 0);
    for (int k = 0; k < bb.w; ++k) {
        const int x = headX + dir * k;
        for (int y = bb.y; y < bb.y + bb.h; ++y) colH[static_cast<std::size_t>(k)] += opaque.get(x, y) ? 1 : 0;
    }
    // The tail stalk: the thinnest column in the rear half (but before the
    // last eighth), when it is clearly thinner than the body.
    int stalk = -1;
    int deepest = 0;
    for (int k = 0; k < bb.w; ++k) deepest = std::max(deepest, colH[static_cast<std::size_t>(k)]);
    for (int k = bb.w / 2; k < bb.w * 7 / 8; ++k)
        if (stalk < 0 || colH[static_cast<std::size_t>(k)] < colH[static_cast<std::size_t>(stalk)]) stalk = k;
    if (stalk < 0 || colH[static_cast<std::size_t>(stalk)] * 2 > deepest) stalk = -1;

    // Mode from the build: length over depth.
    const double aspect = static_cast<double>(bb.w) / std::max(1, bb.h);
    SwimStyle mode = style;
    if (mode == SwimStyle::Auto)
        mode = aspect >= 3.2 ? SwimStyle::Anguilliform : aspect <= 1.6 ? SwimStyle::Carangiform : SwimStyle::Subcarangiform;

    // Parts. The tail fin: past the stalk (an eel has none).
    Mask tail(sw, sh);
    if (stalk >= 0 && mode != SwimStyle::Anguilliform)
        for (int y = bb.y; y < bb.y + bb.h; ++y)
            for (int x = bb.x; x < bb.x + bb.w; ++x)
                if (opaque.get(x, y) && along(x) > stalk) tail.set(x, y, true);
    // Fins: thin parts sticking out of the body (gone after an opening).
    const Mask trunk = rig::minusMask(opaque, tail);
    const int r = std::max(1, iround(deepest / 8.0));
    const Mask core = rig::openMask(trunk, r);
    const Mask coreRim = rig::dilate(core, 1);
    std::vector<Mask> fins;
    {
        const Mask thin = rig::minusMask(trunk, core);
        const int minFin = std::max(6, rig::count(trunk) / 120);
        for (Mask& piece : rig::components(thin)) {
            const IntRect pb = rig::bounds(piece);
            if (rig::count(piece) < minFin || pb.w < 2 || pb.h < 2) continue;
            // A fin sticks out: most of it lies beyond the core's edge.
            int outside = 0;
            for (int y = pb.y; y < pb.y + pb.h; ++y)
                for (int x = pb.x; x < pb.x + pb.w; ++x)
                    if (piece.get(x, y) && !coreRim.get(x, y)) ++outside;
            if (outside * 3 < rig::count(piece)) continue;
            fins.push_back(std::move(piece));
            if (fins.size() >= 6) break;
        }
    }

    rig::Rig rig;
    const int body = rig::addPart(rig, sprite, "body", trunk, {headX + 0.5, bb.y + bb.h / 2.0}, 0);
    int tailPart = -1;
    rig::Vec stalkPt{headX + dir * (stalk + 0.5) + 0.5, bb.y + bb.h / 2.0};
    if (rig::count(tail) > 0) {
        // The stalk's middle row.
        const int sx = headX + dir * stalk;
        int y0 = -1, y1 = -1;
        for (int y = bb.y; y < bb.y + bb.h; ++y)
            if (opaque.get(sx, y)) {
                if (y0 < 0) y0 = y;
                y1 = y;
            }
        if (y0 >= 0) stalkPt = {sx + 0.5 + dir * 0.5, (y0 + y1 + 1) / 2.0};
        tailPart = rig::addPart(rig, sprite, "tail_fin", tail, stalkPt, -1);
    }
    struct Fin {
        int part = -1;
        rig::Vec root;
        double u = 0;      // along the body, 0 head .. 1 tail
        bool top = false;
    };
    std::vector<Fin> finParts;
    double bodyMidY = 0;
    {
        int n = 0;
        for (int y = bb.y; y < bb.y + bb.h; ++y)
            for (int x = bb.x; x < bb.x + bb.w; ++x)
                if (core.get(x, y)) {
                    bodyMidY += y + 0.5;
                    ++n;
                }
        bodyMidY = n ? bodyMidY / n : bb.y + bb.h / 2.0;
    }
    for (const Mask& fin : fins) {
        double sx = 0, sy = 0;
        int n = 0;
        const IntRect fb = rig::bounds(fin);
        for (int y = fb.y; y < fb.y + fb.h; ++y)
            for (int x = fb.x; x < fb.x + fb.w; ++x)
                if (fin.get(x, y) && coreRim.get(x, y)) {
                    sx += x + 0.5;
                    sy += y + 0.5;
                    ++n;
                }
        if (n == 0) {
            sx = fb.x + fb.w / 2.0;
            sy = fb.y + fb.h / 2.0;
            n = 1;
        }
        Fin f;
        f.root = {sx / n, sy / n};
        f.top = f.root.y < bodyMidY;
        f.u = std::clamp(along(iround(f.root.x)) / std::max(1.0, bb.w - 1.0), 0.0, 1.0);
        f.part = rig::addPart(rig, sprite, f.top ? "dorsal_fin" : "ventral_fin", fin, f.root, -1);
        finParts.push_back(f);
    }
    rig::inpaintHidden(rig, std::max(3, deepest / 6));
    rig::prepare(rig);

    // Motion.
    const bool dash = action == Action::Dash, idle = action == Action::Idle;
    const int beats = (dash ? 2 : 1) * (mode == SwimStyle::Thunniform ? 2 : 1);
    const double amp = std::max(1.0, bb.h * (dash ? 0.12 : idle ? 0.035 : 0.08) * amount);
    const double wavelength = mode == SwimStyle::Anguilliform ? 0.7 : mode == SwimStyle::Carangiform ? 1.25 : mode == SwimStyle::Thunniform ? 1.6 : 1.0;
    auto envelope = [&](double u) {   // 0 at the head, 1 at the tail stalk
        switch (mode) {
        case SwimStyle::Anguilliform: return 0.15 + 0.85 * u;
        case SwimStyle::Carangiform: return std::pow(std::max(0.0, (u - 0.45) / 0.55), 2.0);
        case SwimStyle::Thunniform: return std::pow(std::max(0.0, (u - 0.72) / 0.28), 1.5);
        case SwimStyle::Subcarangiform:
        case SwimStyle::Auto: break;
        }
        return std::pow(std::max(0.0, (u - 0.2) / 0.8), 1.5);
    };
    const int bodyLen = stalk >= 0 && tailPart >= 0 ? stalk + 1 : bb.w;
    // The bend at body position u (0 head, 1 stalk), in pixels, at time t.
    auto bend = [&](double u, double t) {
        const double phase = 2 * kPi * (beats * t) - 2 * kPi * u / wavelength;
        return amp * envelope(std::min(1.0, u)) * std::sin(phase);
    };
    const double hoverAmp = idle ? std::max(1.0, bb.h / 40.0) : 0.0;
    const int m = static_cast<int>(std::ceil(amp)) + std::max(4, bb.h / 4) + 2;
    const IntRect big{bb.x - m, bb.y - m, bb.w + 2 * m, bb.h + 2 * m};
    const rig::Part& bodyP = rig.parts[static_cast<std::size_t>(body)];
    const IntRect pb{bodyP.hiBox.x + 1, bodyP.hiBox.y + 1, bodyP.hiBox.w - 2, bodyP.hiBox.h - 2};
    std::vector<Image> frameImgs;
    std::vector<std::vector<rig::PartPose>> allPoses;
    for (int f = 0; f < frames; ++f) {
        const double t = static_cast<double>(f) / frames;
        std::vector<rig::PartPose> pose = rig.rest();
        const int hover = iround(hoverAmp * std::sin(2 * kPi * t));
        // A dash surges forward and glides back.
        const int surge = dash ? iround(bb.w * 0.06 * amount * std::sin(kPi * t)) * -dir : 0;
        // Body columns: whole pixels, neighbours at most one apart.
        std::vector<int> colDy(static_cast<std::size_t>(std::max(0, pb.w)), 0);
        {
            std::vector<int> byK(static_cast<std::size_t>(bb.w), 0);
            int prev = iround(bend(0.0, t));
            for (int k = 0; k < bb.w; ++k) {
                const int want = iround(bend(static_cast<double>(k) / std::max(1, bodyLen - 1), t));
                const int v = std::clamp(want, prev - 1, prev + 1);
                byK[static_cast<std::size_t>(k)] = v;
                prev = v;
            }
            for (int c = 0; c < pb.w; ++c) {
                const int k = std::clamp(along(pb.x + c), 0, bb.w - 1);
                colDy[static_cast<std::size_t>(c)] = byK[static_cast<std::size_t>(k)];
            }
        }
        auto dyAt = [&](double x) {
            if (colDy.empty()) return 0;
            const int c = std::clamp(static_cast<int>(std::floor(x)) - pb.x, 0, pb.w - 1);
            return colDy[static_cast<std::size_t>(c)];
        };
        rig::PartPose& bq = pose[static_cast<std::size_t>(body)];
        bq.colDy = colDy;
        bq.dy = hover;
        bq.dx = surge;
        if (tailPart >= 0) {
            // The fin trails the stalk by 0.15 of a beat; it turns about the
            // stalk and narrows as it sweeps across the line of sight.
            const double lagT = t - 0.15 / beats;
            const double ph = 2 * kPi * beats * lagT - 2 * kPi / wavelength;
            rig::PartPose& tq = pose[static_cast<std::size_t>(tailPart)];
            tq.dy = hover + dyAt(stalkPt.x - dir * 1.0);
            tq.dx = surge;
            const double turnAmp = (dash ? 0.32 : idle ? 0.1 : 0.22) * amount;
            tq.angle = -dir * turnAmp * std::sin(ph);
            tq.sx = 1.0 - (idle ? 0.15 : 0.4) * std::abs(std::cos(ph));
        }
        for (std::size_t i = 0; i < finParts.size(); ++i) {
            const Fin& fn = finParts[i];
            rig::PartPose& fq = pose[static_cast<std::size_t>(fn.part)];
            fq.dy = hover + dyAt(fn.root.x);
            fq.dx = surge;
            // Fins paddle on their own phases; lower fins more than the
            // dorsal fin, which mostly follows the body.
            const double finPhase = 2 * kPi * (t * (idle ? 1 : beats) + 0.25 * static_cast<double>(i) + fn.u * 0.5);
            const double finAmp = (fn.top ? 0.08 : 0.16) * amount * (idle ? 1.2 : 1.0);
            fq.angle = finAmp * std::sin(finPhase) * (fn.top ? -dir : dir);
        }
        allPoses.push_back(pose);
        frameImgs.push_back(rig::renderPose(rig, pose, big));
    }
    int x0 = big.w, y0 = big.h, x1 = -1, y1 = -1;
    for (const Image& im : frameImgs) {
        const IntRect rr = im.opaqueBounds();
        if (rr.empty()) continue;
        x0 = std::min(x0, rr.x);
        y0 = std::min(y0, rr.y);
        x1 = std::max(x1, rr.x + rr.w);
        y1 = std::max(y1, rr.y + rr.h);
    }
    if (x1 < 0) return std::nullopt;
    RigClip out;
    out.depthPops = static_cast<int>(rig::zPops(rig, allPoses, big).size());
    out.box = {big.x + x0, big.y + y0, x1 - x0, y1 - y0};
    for (const Image& im : frameImgs) out.frames.push_back(im.copy({x0, y0, x1 - x0, y1 - y0}));
    return out;
}

} // namespace acestudio::motion::detail
