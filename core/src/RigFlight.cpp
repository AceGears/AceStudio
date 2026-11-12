// Wing flap and flight on the part rig (ADR-ANIM-001 step 5b).
//
// The wings are cut from the sprite (the wing line or a painted mask, then
// split into the two wings), each becomes a part that turns about its
// shoulder, and the body (head, neck, legs, tail) is a third part:
//  - the near wing is drawn in front of the body, the far wing behind it, so
//    the head is never covered by the far wing and the near wing sweeps down
//    away from the head;
//  - the far wing trails the near wing by a fraction of a beat and sweeps
//    less (it is seen at an angle);
//  - a wing curls: the outer wing (the hand, the long fingers) turns later
//    than the arm, trailing it on the downstroke and folding in on the
//    recovery, so the tip draws a loop instead of a rigid paddle. The curl is
//    one smooth deformation of the wing part, so no seam opens at the wrist;
//  - fast downstroke, slow recovery; the body rises as the wings push down;
//  - the tail (the thin end of the body away from the head) waves in whole
//    pixels, each column later than the one before it, so the tip lags the root.
// Pixels are only moved or turned with RotSprite sampling (PartRig.h).

#include "RigModules.h"

#include "PartRig.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace acestudio::motion::detail {

namespace {

constexpr double kPi = 3.14159265358979323846;

double smoothstep(double q) {
    q = std::clamp(q, 0.0, 1.0);
    return q * q * (3 - 2 * q);
}

int iround(double v) { return static_cast<int>(std::lround(v)); }

// How far the wing is raised: 1 = as drawn (up), 0 = the bottom of the
// downstroke. The downstroke takes 3 of 8 keys, the recovery 5.
double openness(double t) {
    static const double k[] = {1.0, 0.74, 0.23, 0.0, 0.06, 0.26, 0.63, 0.86, 1.0};
    const double x = (t - std::floor(t)) * 8.0;
    const int i = std::min(7, static_cast<int>(x));
    const double u = smoothstep(x - i);
    return k[i] + (k[i + 1] - k[i]) * u;
}

struct WingCut {
    Mask mask;
    rig::Vec shoulder;
    double reach = 0;     // shoulder to the farthest pixel
    int sign = 1;         // +1: the tip sweeps clockwise on the downstroke
    int area = 0;
};

WingCut cutWing(const Mask& wing, rig::Vec shoulder) {
    WingCut c;
    c.mask = wing;
    c.shoulder = shoulder;
    c.area = rig::count(wing);
    double cx = 0;
    for (int y = 0; y < wing.height(); ++y)
        for (int x = 0; x < wing.width(); ++x)
            if (wing.get(x, y)) {
                c.reach = std::max(c.reach, std::hypot(x + 0.5 - shoulder.x, y + 0.5 - shoulder.y));
                cx += x + 0.5;
            }
    // The wing sweeps outwards: away from the body, on the side its mass lies.
    c.sign = cx / std::max(1, c.area) < shoulder.x ? -1 : 1;
    return c;
}

// Two wings that touch near their roots: erode the wings until they fall
// apart into two big pieces, then grow the pieces back over the wing pixels
// at the same pace (a watershed), so the cut follows the narrow join.
std::vector<Mask> splitByErosion(const Mask& wings) {
    const int total = rig::count(wings);
    for (int r = 1; r <= 8; ++r) {
        auto pieces = rig::components(rig::erode(wings, r));
        if (pieces.size() < 2) {
            if (pieces.empty()) break;
            continue;
        }
        const int a = rig::count(pieces[0]), b = rig::count(pieces[1]);
        if (b * 5 < a || b * 40 < total) continue;
        const int w = wings.width(), h = wings.height();
        std::vector<int> label(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), -1);
        std::vector<std::pair<int, int>> front;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                for (int k = 0; k < 2; ++k)
                    if (pieces[static_cast<std::size_t>(k)].get(x, y)) {
                        label[static_cast<std::size_t>(y * w + x)] = k;
                        front.emplace_back(x, y);
                    }
        while (!front.empty()) {
            std::vector<std::pair<int, int>> next;
            for (const auto& [x, y] : front) {
                const int l = label[static_cast<std::size_t>(y * w + x)];
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = x + dx, ny = y + dy;
                        if (!wings.get(nx, ny) || label[static_cast<std::size_t>(ny * w + nx)] >= 0) continue;
                        label[static_cast<std::size_t>(ny * w + nx)] = l;
                        next.emplace_back(nx, ny);
                    }
            }
            front = std::move(next);
        }
        Mask m0(w, h), m1(w, h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const int l = label[static_cast<std::size_t>(y * w + x)];
                if (l == 0) m0.set(x, y, true);
                else if (l == 1) m1.set(x, y, true);
                else if (wings.get(x, y)) m0.set(x, y, true);   // cut off by the erosion: crumbs
            }
        return {m0, m1};
    }
    return {};
}

} // namespace

std::optional<RigClip> flightRender(const Image& sprite, const Subject& subj, Action action, int frames, double amount,
                                    const WingSpec& spec) {
    if (action != Action::Flap && action != Action::Fly) return std::nullopt;
    const IntRect bb = subj.bounds;
    if (bb.empty() || frames < 2) return std::nullopt;
    amount = std::clamp(amount, 0.25, 2.0);
    const int sw = sprite.width(), sh = sprite.height();
    const Mask opaque = rig::opaqueMask(sprite);

    // The wings: above the wing line (between `from` and `to`), or the mask.
    const bool painted = spec.mask && !spec.mask->isNull();
    const int rootRow = bb.y + iround(std::clamp(spec.line, 0.05, 0.95) * bb.h);
    const int wx0 = bb.x + static_cast<int>(std::floor(std::clamp(std::min(spec.from, spec.to), 0.0, 1.0) * bb.w));
    const int wx1 = bb.x + static_cast<int>(std::ceil(std::clamp(std::max(spec.from, spec.to), 0.0, 1.0) * bb.w));
    Mask wings(sw, sh);
    for (int y = bb.y; y < bb.y + bb.h; ++y)
        for (int x = bb.x; x < bb.x + bb.w; ++x)
            if (opaque.get(x, y) && (painted ? spec.mask->get(x, y) : (y < rootRow && x >= wx0 && x < wx1))) wings.set(x, y, true);
    // A wing's lower part often hangs below the line, over the back. Below
    // the line, the wing goes on through pixels of the wing's own colours
    // (common in the wing, rare in the lower body), connected to the wing,
    // down to a quarter of the height; and a column that continues a wing
    // column and ends in air within a few rows (a fringe) is wing too.
    if (!painted) {
        std::map<std::uint32_t, int> inWing, inBody;
        int nw = 0, nb = 0;
        auto key = [](Rgba8 c) { return (static_cast<std::uint32_t>(c.r) << 16) | (static_cast<std::uint32_t>(c.g) << 8) | c.b; };
        for (int y = bb.y; y < bb.y + bb.h; ++y)
            for (int x = bb.x; x < bb.x + bb.w; ++x) {
                if (!opaque.get(x, y)) continue;
                if (wings.get(x, y) && y < rootRow - 2) { ++inWing[key(sprite.pixel(x, y))]; ++nw; }
                else if (y >= bb.y + bb.h * 55 / 100) { ++inBody[key(sprite.pixel(x, y))]; ++nb; }
            }
        auto wingish = [&](int x, int y) {
            const std::uint32_t k = key(sprite.pixel(x, y));
            const double fw = inWing.count(k) ? inWing[k] / static_cast<double>(std::max(1, nw)) : 0.0;
            const double fb = inBody.count(k) ? inBody[k] / static_cast<double>(std::max(1, nb)) : 0.0;
            return fw > 0 && fw > 2.0 * fb;
        };
        const int deepest = rootRow + bb.h / 4;
        std::vector<std::pair<int, int>> stack;
        for (int x = wx0; x < wx1; ++x)
            if (wings.get(x, rootRow - 1)) stack.emplace_back(x, rootRow - 1);
        while (!stack.empty()) {
            const auto [x, y] = stack.back();
            stack.pop_back();
            const int nx[4] = {x + 1, x - 1, x, x}, ny[4] = {y, y, y + 1, y - 1};
            for (int k = 0; k < 4; ++k) {
                if (ny[k] < rootRow || ny[k] > deepest || nx[k] < wx0 || nx[k] >= wx1) continue;
                if (!opaque.get(nx[k], ny[k]) || wings.get(nx[k], ny[k]) || !wingish(nx[k], ny[k])) continue;
                wings.set(nx[k], ny[k], true);
                stack.emplace_back(nx[k], ny[k]);
            }
        }
        const int fringe = std::max(3, bb.h / 8);
        for (int x = wx0; x < wx1; ++x) {
            int y = rootRow;
            while (y < bb.y + bb.h && wings.get(x, y)) ++y;
            if (!wings.get(x, y - 1)) continue;
            int len = 0;
            while (y + len < bb.y + bb.h && opaque.get(x, y + len) && !wings.get(x, y + len)) ++len;
            if (len > 0 && len <= fringe && (y + len >= bb.y + bb.h || !opaque.get(x, y + len)))
                for (int k = 0; k < len; ++k) wings.set(x, y + k, true);
        }
        // Lone body pixels left inside the wing (an outline between two
        // membrane panels) go with the wing.
        for (int y = rootRow; y <= deepest; ++y)
            for (int x = wx0; x < wx1; ++x) {
                if (!opaque.get(x, y) || wings.get(x, y)) continue;
                int around = 0, wingAround = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                        if ((dx || dy) && opaque.get(x + dx, y + dy)) {
                            ++around;
                            wingAround += wings.get(x + dx, y + dy) ? 1 : 0;
                        }
                if (around > 0 && wingAround * 4 >= around * 3) wings.set(x, y, true);
            }
    }
    const int wingArea = rig::count(wings);
    if (wingArea < 12) return std::nullopt;
    const Mask body = rig::minusMask(opaque, wings);
    if (rig::count(body) < 4) return std::nullopt;

    // Two wings: two big pieces, or else the emptiest column in the middle of
    // the wings splits them. One wing (a side view) is the near wing.
    std::vector<Mask> parts;
    {
        auto pieces = rig::components(wings);
        if (pieces.size() >= 2 && rig::count(pieces[1]) * 4 >= rig::count(pieces[0])) {
            Mask a = pieces[0], b = pieces[1];
            const IntRect ra = rig::bounds(a), rb = rig::bounds(b);
            for (std::size_t i = 2; i < pieces.size(); ++i) {   // crumbs join the nearer wing
                const IntRect r = rig::bounds(pieces[i]);
                const double ca = std::abs(r.x + r.w / 2.0 - (ra.x + ra.w / 2.0)), cb = std::abs(r.x + r.w / 2.0 - (rb.x + rb.w / 2.0));
                (ca <= cb ? a : b) = rig::orMask(ca <= cb ? a : b, pieces[i]);
            }
            parts = {a, b};
        } else if (auto halves = splitByErosion(wings); !halves.empty()) {
            parts = std::move(halves);
        } else {
            const IntRect wb = rig::bounds(wings);
            std::vector<int> col(static_cast<std::size_t>(wb.w), 0);
            for (int y = wb.y; y < wb.y + wb.h; ++y)
                for (int x = wb.x; x < wb.x + wb.w; ++x) col[static_cast<std::size_t>(x - wb.x)] += wings.get(x, y) ? 1 : 0;
            int split = -1, low = 1 << 30;
            for (int k = wb.w * 3 / 10; k < wb.w * 7 / 10; ++k)
                if (col[static_cast<std::size_t>(k)] < low) { low = col[static_cast<std::size_t>(k)]; split = k; }
            int peakL = 0, peakR = 0;
            for (int k = 0; k < wb.w; ++k) (k < split ? peakL : peakR) = std::max(k < split ? peakL : peakR, col[static_cast<std::size_t>(k)]);
            if (split > 0 && low * 3 <= std::min(peakL, peakR) * 2) {
                Mask a(sw, sh), b(sw, sh);
                for (int y = wb.y; y < wb.y + wb.h; ++y)
                    for (int x = wb.x; x < wb.x + wb.w; ++x)
                        if (wings.get(x, y)) (x - wb.x < split ? a : b).set(x, y, true);
                parts = {a, b};
            } else {
                parts = {wings};
            }
        }
    }

    // Shoulders: where each wing meets the body, nearest the body's middle
    // (the torso); else the middle of the wing's root.
    const Mask bodyRim = rig::dilate(body, 1);
    rig::Vec bodyMid;
    {
        double mx = 0, my = 0;
        int mn = 0;
        for (int y = bb.y; y < bb.y + bb.h; ++y)
            for (int x = bb.x; x < bb.x + bb.w; ++x)
                if (body.get(x, y)) { mx += x + 0.5; my += y + 0.5; ++mn; }
        bodyMid = {mx / std::max(1, mn), my / std::max(1, mn)};
    }
    std::vector<WingCut> cuts;
    for (const Mask& w : parts) {
        double sx = 0, sy = 0;
        int n = 0;
        const IntRect r = rig::bounds(w);
        double nearest = 1e18;
        for (int y = r.y; y < r.y + r.h; ++y)
            for (int x = r.x; x < r.x + r.w; ++x)
                if (w.get(x, y) && bodyRim.get(x, y)) nearest = std::min(nearest, std::hypot(x + 0.5 - bodyMid.x, y + 0.5 - bodyMid.y));
        const double reachRoot = std::max(3.0, r.w / 10.0);
        for (int y = r.y; y < r.y + r.h; ++y)
            for (int x = r.x; x < r.x + r.w; ++x)
                if (w.get(x, y) && bodyRim.get(x, y) && std::hypot(x + 0.5 - bodyMid.x, y + 0.5 - bodyMid.y) <= nearest + reachRoot) {
                    sx += x + 0.5;
                    sy += y + 0.5;
                    ++n;
                }
        if (n < 2) {
            sx = sy = 0;
            n = 0;
            const int rows = std::max(1, r.h / 8);
            for (int y = r.y + r.h - rows; y < r.y + r.h; ++y)
                for (int x = r.x; x < r.x + r.w; ++x)
                    if (w.get(x, y)) { sx += x + 0.5; sy += y + 0.5; ++n; }
        }
        rig::Vec sh2{sx / std::max(1, n), sy / std::max(1, n)};
        cuts.push_back(cutWing(w, sh2));
    }
    // Flight of a sprite that is not known to have wings needs a clear pair.
    if (action == Action::Fly && cuts.size() < 2 && !painted) return std::nullopt;
    // Where the head is: the side the creature faces, else the side of the
    // body's highest pixels (head, horns, ears) away from the body's middle.
    int headSide = subj.facing;
    if (headSide == 0) {
        const IntRect b = rig::bounds(body);
        double sx = 0;
        int n = 0;
        for (int y = b.y; y < b.y + std::max(1, b.h / 8); ++y)
            for (int x = b.x; x < b.x + b.w; ++x)
                if (body.get(x, y)) { sx += x + 0.5; ++n; }
        if (n > 0 && std::abs(sx / n - bodyMid.x) > b.w * 0.08) headSide = sx / n > bodyMid.x ? 1 : -1;
    }
    // Near and far: the far wing is on the head's side (its root is behind
    // the neck); without a head side, the smaller wing.
    std::size_t nearIdx = 0;
    if (cuts.size() == 2) {
        const std::size_t right = cuts[0].shoulder.x > cuts[1].shoulder.x ? 0 : 1;
        if (headSide != 0) nearIdx = headSide > 0 ? 1 - right : right;
        else nearIdx = cuts[0].area >= cuts[1].area ? 0 : 1;
    }
    // The near wing, drawn in front of the body, sweeps towards the tail: it
    // never passes over the head.
    if (!cuts.empty() && headSide != 0) cuts[nearIdx].sign = -headSide;

    // The near wing's root: below the line, over the back, within the wing's
    // columns. It is skinned: it turns with the wing at the line and less
    // and less below it, so the wing never leaves a piece of itself behind.
    const int skinDepth = std::max(3, bb.h / 6);
    double skinX0 = 0, skinX1 = 0;
    if (!painted && !cuts.empty()) {
        WingCut& nw = cuts[nearIdx];
        IntRect r = rig::bounds(nw.mask);
        // Inwards up to the far wing's shoulder: the membrane between the two
        // roots goes with the near wing, its turn fading out sideways.
        skinX0 = nw.sign < 0 ? r.x : nw.shoulder.x;
        skinX1 = nw.sign < 0 ? nw.shoulder.x : r.x + r.w;
        if (cuts.size() == 2) {
            const double farX = cuts[1 - nearIdx].shoulder.x;
            const int lo = static_cast<int>(std::floor(std::min(farX, static_cast<double>(r.x)))), hi = static_cast<int>(std::ceil(std::max(farX, static_cast<double>(r.x + r.w))));
            r.x = lo;
            r.w = hi - lo;
        }
        for (int y = rootRow; y < rootRow + skinDepth; ++y)
            for (int x = r.x; x < r.x + r.w; ++x)
                if (opaque.get(x, y) && !nw.mask.get(x, y)) {
                    bool otherWing = false;
                    for (std::size_t i = 0; i < cuts.size(); ++i) otherWing = otherWing || (i != nearIdx && cuts[i].mask.get(x, y));
                    if (!otherWing) nw.mask.set(x, y, true);
                }
    }
    Mask bodyOnly = body;
    for (const WingCut& c : cuts) bodyOnly = rig::minusMask(bodyOnly, c.mask);
    // Crumbs of the body that touch nothing of it (wing pixels cut off by
    // the line) go with the nearest wing.
    {
        auto pieces = rig::components(bodyOnly);
        const int mainArea = pieces.empty() ? 0 : rig::count(pieces.front());
        for (std::size_t i = 1; i < pieces.size(); ++i) {
            if (rig::count(pieces[i]) * 20 > mainArea || cuts.empty()) continue;
            const IntRect r = rig::bounds(pieces[i]);
            std::size_t best = 0;
            double bestD = 1e18;
            for (std::size_t k = 0; k < cuts.size(); ++k) {
                const IntRect wbx = rig::bounds(cuts[k].mask);
                const double dx = std::max({0, wbx.x - (r.x + r.w), r.x - (wbx.x + wbx.w)});
                const double dy = std::max({0, wbx.y - (r.y + r.h), r.y - (wbx.y + wbx.h)});
                if (dx + dy < bestD) { bestD = dx + dy; best = k; }
            }
            cuts[best].mask = rig::orMask(cuts[best].mask, pieces[i]);
            bodyOnly = rig::minusMask(bodyOnly, pieces[i]);
        }
    }

    rig::Rig rig;
    const double bodyCx = rig::bounds(body).x + rig::bounds(body).w / 2.0;
    // The head's end of the body (from just behind the near shoulder on) is
    // drawn over the near wing: the wing's root tucks in behind the chest
    // and the head, and the near wing only ever covers the back and tail.
    Mask chest(sw, sh);
    if (!cuts.empty() && headSide != 0) {
        const WingCut& nw = cuts[nearIdx];
        const double split = nw.shoulder.x - headSide * std::max(2.0, nw.reach * 0.15);
        for (int y = bb.y; y < bb.y + bb.h; ++y)
            for (int x = bb.x; x < bb.x + bb.w; ++x)
                if (bodyOnly.get(x, y) && (x + 0.5 - split) * headSide > 0) chest.set(x, y, true);
    }
    const int bodyPart = rig::addPart(rig, sprite, "body", rig::minusMask(bodyOnly, chest), {bodyCx, static_cast<double>(rootRow)}, 0);
    const int chestPart = rig::count(chest) > 0 ? rig::addPart(rig, sprite, "head_end", chest, {bodyCx, static_cast<double>(rootRow)}, 5) : -1;
    struct WingParts {
        int part = -1;
        bool nearWing = true;
        WingCut cut;
    };
    std::vector<WingParts> wp;
    for (std::size_t i = 0; i < cuts.size(); ++i) {
        WingParts p;
        p.cut = cuts[i];
        p.nearWing = i == nearIdx;
        p.part = rig::addPart(rig, sprite, p.nearWing ? "near_wing" : "far_wing", cuts[i].mask, cuts[i].shoulder, p.nearWing ? 2 : -2,
                              p.nearWing ? rig::Side::Near : rig::Side::Far);
        wp.push_back(p);
    }
    rig::inpaintHidden(rig, std::max(4, bb.h / 6));
    rig::prepare(rig);

    // Motion.
    const bool fly = action == Action::Fly;
    const double sweep = std::min(2.0, 1.6 * amount * (fly ? 0.7 : 1.0));   // radians at the bottom
    const double farLag = 0.045, handLag = 0.09;
    const int lift = std::max(1, iround(bb.h / 64.0 * amount));
    // Render into a generous box; the clip is cropped to what is used.
    double reach = 0;
    for (const WingCut& c : cuts) reach = std::max(reach, c.reach);
    const int m = static_cast<int>(std::ceil(reach)) + lift + 4;
    const IntRect big{bb.x - m, bb.y - m, bb.w + 2 * m, bb.h + 2 * m};
    // The tail: columns of the body at the end away from the head, from its
    // tip until the body gets thick (half its greatest depth). Listed root
    // first. None when the head side is unknown or the end is too short.
    std::vector<int> tailColumns;
    if (headSide != 0) {
        const Mask& own = rig.parts[static_cast<std::size_t>(bodyPart)].own;
        const IntRect ob = rig::bounds(own);
        std::map<int, int> depth;
        for (int x = ob.x; x < ob.x + ob.w; ++x) {
            int n = 0;
            for (int y = ob.y; y < ob.y + ob.h; ++y) n += own.get(x, y) ? 1 : 0;
            depth[x] = n;
        }
        // Measured on the whole body: the head end is a part of its own, so
        // this part alone may be little more than the tail.
        int deepest = 0;
        for (int x = bb.x; x < bb.x + bb.w; ++x) {
            int n = 0;
            for (int y = bb.y; y < bb.y + bb.h; ++y) n += bodyOnly.get(x, y) ? 1 : 0;
            deepest = std::max(deepest, n);
        }
        const int tip = headSide > 0 ? ob.x : ob.x + ob.w - 1;
        for (int x = tip; x >= ob.x && x < ob.x + ob.w && (x + 0.5 - bodyCx) * headSide < 0; x += headSide) {
            if (depth[x] * 2 >= deepest) break;
            tailColumns.insert(tailColumns.begin(), x);
        }
        if (tailColumns.size() < 4) tailColumns.clear();
    }
    const int tailAmp = std::max(1, iround(static_cast<double>(tailColumns.size()) * 0.15 * amount));
    std::vector<Image> frameImgs;
    std::vector<std::vector<rig::PartPose>> allPoses;
    for (int f = 0; f < frames; ++f) {
        const double t = static_cast<double>(f) / frames;
        std::vector<rig::PartPose> pose = rig.rest();
        // The body rises while the wings are low, a frame behind the stroke.
        const double low = 1.0 - openness(t - 1.0 / 16.0);
        const double bob = fly ? 0.5 * (1 - std::cos(2 * kPi * t)) : 0.0;
        const int dy = -iround(lift * smoothstep((low - 0.2) / 0.6)) + iround(bob * lift);
        pose[static_cast<std::size_t>(bodyPart)].dy = dy;
        if (chestPart >= 0) pose[static_cast<std::size_t>(chestPart)].dy = dy;
        if (!tailColumns.empty()) {
            // A wave running from the root to the tip, 0.3 of a beat late at
            // the tip, growing along the tail; neighbours differ by at most a
            // pixel so the tail stays one piece.
            const rig::Part& bp = rig.parts[static_cast<std::size_t>(bodyPart)];
            const int left = bp.hiBox.x + 1;
            std::vector<int> column(static_cast<std::size_t>(std::max(0, bp.hiBox.w - 2)), 0);
            int previous = 0;
            for (std::size_t i = 0; i < tailColumns.size(); ++i) {
                const double u = static_cast<double>(i + 1) / static_cast<double>(tailColumns.size());
                const int wanted = iround(tailAmp * u * std::sin(2 * kPi * (t - 0.3 * u)));
                previous = std::clamp(wanted, previous - 1, previous + 1);
                const int k = tailColumns[i] - left;
                if (k >= 0 && k < static_cast<int>(column.size())) column[static_cast<std::size_t>(k)] = previous;
            }
            pose[static_cast<std::size_t>(bodyPart)].colDy = std::move(column);
        }
        for (const WingParts& w : wp) {
            const double lag = w.nearWing ? 0.0 : farLag;
            const double scale = w.nearWing ? 1.0 : 0.8;
            const double down = 1.0 - openness(t - lag);
            // The hand trails the arm on the way down and folds in (back
            // towards the body) on the way up.
            const double handDown = 1.0 - openness(t - lag - handLag);
            rig::PartPose& q = pose[static_cast<std::size_t>(w.part)];
            q.angle = w.cut.sign * sweep * scale * down;
            q.bend = w.cut.sign * sweep * scale * (handDown - down) * 0.9;
            q.bendRadius = w.cut.reach;
            q.dy = dy;
            if (w.nearWing && !painted) {
                q.skinTop = rootRow;
                q.skinDepth = skinDepth;
                q.skinX0 = skinX0;
                q.skinX1 = skinX1;
                q.skinFade = std::max(3.0, std::abs(cuts.size() == 2 ? cuts[1 - nearIdx].shoulder.x - w.cut.shoulder.x : skinDepth / 2.0));
            }
            // The far wing is foreshortened across the stroke (seen at an angle).
            if (!w.nearWing) q.sx = 1.0 - 0.15 * down;
        }
        allPoses.push_back(pose);
        frameImgs.push_back(rig::renderPose(rig, pose, big));
    }
    // Crop to the area used by any frame.
    int x0 = big.w, y0 = big.h, x1 = -1, y1 = -1;
    for (const Image& im : frameImgs) {
        const IntRect r = im.opaqueBounds();
        if (r.empty()) continue;
        x0 = std::min(x0, r.x);
        y0 = std::min(y0, r.y);
        x1 = std::max(x1, r.x + r.w);
        y1 = std::max(y1, r.y + r.h);
    }
    if (x1 < 0) return std::nullopt;
    RigClip out;
    out.depthPops = static_cast<int>(rig::zPops(rig, allPoses, big).size());
    out.box = {big.x + x0, big.y + y0, x1 - x0, y1 - y0};
    for (const Image& im : frameImgs) out.frames.push_back(im.copy({x0, y0, x1 - x0, y1 - y0}));
    return out;
}

} // namespace acestudio::motion::detail
