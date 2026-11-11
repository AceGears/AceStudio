// Walk, run and sprint on the part rig (ADR-ANIM-001 step 5a).
//
// Each leg is three parts: the thigh (hip to knee), the shin (knee to ankle)
// and the foot. A frame places the foot, then finds the knee with two-bone
// IK, and turns the thigh and the shin to meet it (RotSprite sampling). The
// bones keep their length; the knee bends instead of the leg dropping rows
// (the older renderer shortened legs by dropping and doubling rows).
//  - The planted foot is locked to the ground and moves back at exactly the
//    same number of pixels every frame (an in-place cycle: no sliding); its
//    sole stays on its row.
//  - The swinging foot travels forward on an eased arc, the knee leading.
//  - The body is lowest just after contact and highest at the up pose; it
//    also sinks whenever a planted leg could not reach the ground otherwise,
//    so a straight-legged sprite still plants both feet in the contact pose.
//  - Seen from the side, a biped's pelvis and shoulders shift against each
//    other by whole pixels, the pelvis towards the near leg's stride; the
//    head only moves with the body.
//  - Arms (and anything they hold) are rigid parts that swing from the
//    shoulder against the legs; a shield or sword is turned, never sheared.
//  - Seen from the front, the legs step in place: the foot lifts, the thigh
//    foreshortens as the knee comes towards the viewer; nothing crosses.
// The legs are drawn behind the body (a coat or cloak hangs over them), the
// far leg behind the near one; arms in front.

#include "RigModules.h"

#include "PartRig.h"

#include <algorithm>
#include <cmath>

namespace acestudio::motion::detail {

namespace {

constexpr double kPi = 3.14159265358979323846;

int iround(double v) { return static_cast<int>(std::lround(v)); }

double smoothstep(double q) {
    q = std::clamp(q, 0.0, 1.0);
    return q * q * (3 - 2 * q);
}

struct LegRig {
    int thigh = -1, shin = -1, foot = -1;
    rig::Vec hip, knee, ankle;   // rest joints
    double l1 = 1, l2 = 1;
    double phase = 0;
    int ground = 0;              // the row its foot stands on (lowest row of the foot)
    bool hind = false;           // a four-legged animal's hind leg: the hock bends back
    bool nearLeg = false;
};

} // namespace

std::optional<RigClip> gaitRender(const Image& sprite, const Subject& subj, Body body, Action action, int frames, double amount) {
    const bool footed = (body == Body::Biped || body == Body::Quadruped) && subj.legTop >= 0 && subj.feet.size() >= 2;
    const bool locomotion = action == Action::Walk || action == Action::Run || action == Action::Sprint;
    if (!footed || !locomotion || frames < 4) return std::nullopt;
    const IntRect bb = subj.bounds;
    amount = std::clamp(amount, 0.25, 2.0);
    const int sw = sprite.width(), sh = sprite.height();
    const int dir = subj.facing < 0 ? -1 : 1;
    const bool front = seenFromFront(sprite, subj, body);
    const bool run = action != Action::Walk;

    // Which part each pixel belongs to: ground, arm a, leg i, or the body.
    const bool hasGround = subj.groundTop >= 0;
    const std::size_t nLegs = subj.feet.size();
    Mask ground(sw, sh);
    std::vector<Mask> armMask(subj.arms.size(), Mask(sw, sh));
    std::vector<Mask> legMask(nLegs, Mask(sw, sh));
    Mask bodyMask(sw, sh);
    for (int y = bb.y; y < bb.y + bb.h; ++y)
        for (int x = bb.x; x < bb.x + bb.w; ++x) {
            if (sprite.pixel(x, y).a == 0) continue;
            if (hasGround && y >= subj.groundTop) { ground.set(x, y, true); continue; }
            bool arm = false;
            for (std::size_t a = 0; a < subj.arms.size() && !arm; ++a) {
                const IntRect& r = subj.arms[a];
                if (x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h) { armMask[a].set(x, y, true); arm = true; }
            }
            if (arm) continue;
            int best = -1, bestD = 2;
            if (y >= subj.legTop)
                for (std::size_t i = 0; i < nLegs; ++i) {
                    const int d = x < subj.feet[i].first ? subj.feet[i].first - x : x > subj.feet[i].second ? x - subj.feet[i].second : 0;
                    if (d < bestD) { bestD = d; best = static_cast<int>(i); }
                }
            if (best >= 0) legMask[static_cast<std::size_t>(best)].set(x, y, true);
            else bodyMask.set(x, y, true);
        }

    // Leg joints: the middle of the leg's pixels on the hip, knee and ankle rows.
    auto midX = [&](const Mask& m, int y, double fallback) {
        double s = 0;
        int n = 0;
        for (int x = bb.x; x < bb.x + bb.w; ++x)
            if (m.get(x, y)) { s += x + 0.5; ++n; }
        return n ? s / n : fallback;
    };
    rig::Rig rig;
    if (hasGround) rig::addPart(rig, sprite, "ground", ground, {bb.x + bb.w / 2.0, static_cast<double>(subj.groundTop)}, -10);
    const int bodyPart = rig::addPart(rig, sprite, "body", bodyMask, {bb.x + bb.w / 2.0, static_cast<double>(subj.legTop)}, 0);
    // A side-on biped's torso twists: the shoulder band and the pelvis band
    // are parts of their own (the head and waist stay in "body").
    const int bodyRows = subj.legTop - bb.y;
    const bool twist = body == Body::Biped && !front && bodyRows >= 8;
    int shoulderPart = -1, pelvisPart = -1;
    if (twist) {
        const int shoulderTop = bb.y + iround(bodyRows * 0.45);
        const int span = subj.legTop - shoulderTop;
        const int shoulderEnd = shoulderTop + std::max(1, iround(span * 0.4));
        const int pelvisTop = subj.legTop - std::max(1, iround(span * 0.3));
        Mask shoulders(sw, sh), pelvis(sw, sh);
        for (int y = shoulderTop; y < subj.legTop; ++y)
            for (int x = bb.x; x < bb.x + bb.w; ++x) {
                if (!bodyMask.get(x, y)) continue;
                if (y < shoulderEnd) shoulders.set(x, y, true);
                else if (y >= pelvisTop) pelvis.set(x, y, true);
            }
        const rig::Vec pivot{bb.x + bb.w / 2.0, static_cast<double>(subj.legTop)};
        shoulderPart = rig::addPart(rig, sprite, "shoulders", shoulders, pivot, 0);
        pelvisPart = rig::addPart(rig, sprite, "pelvis", pelvis, pivot, 0);
    }
    // The near leg (more of it drawn) in front of the far one.
    std::vector<int> legArea(nLegs);
    for (std::size_t i = 0; i < nLegs; ++i) legArea[i] = rig::count(legMask[i]);
    std::vector<LegRig> legs(nLegs);
    for (std::size_t i = 0; i < nLegs; ++i) {
        LegRig& L = legs[i];
        const int hip = subj.legTop;
        const int bottom = i < subj.footBottoms.size() ? subj.footBottoms[i] : subj.footRow;
        const int len = bottom - hip + 1;
        const int footTop = bottom - std::max(1, iround(len * 0.22)) + 1;
        const int knee = hip + std::max(1, (footTop - hip) / 2);
        const double fx = (subj.feet[i].first + subj.feet[i].second + 1) / 2.0;
        L.hip = {midX(legMask[i], hip, fx), static_cast<double>(hip)};
        L.knee = {midX(legMask[i], knee, fx), static_cast<double>(knee)};
        L.ankle = {midX(legMask[i], footTop, fx), static_cast<double>(footTop)};
        L.ground = bottom;
        L.l1 = std::max(1.0, std::hypot(L.knee.x - L.hip.x, L.knee.y - L.hip.y));
        L.l2 = std::max(1.0, std::hypot(L.ankle.x - L.knee.x, L.ankle.y - L.knee.y));
        if (nLegs == 4) L.phase = (i == 0 || i == 3) ? 0.0 : 0.5;
        else L.phase = (i % 2 == 0) ? 0.0 : 0.5;
        L.hind = body == Body::Quadruped && (fx - (bb.x + bb.w / 2.0)) * dir < 0;
        int nearer = 0;
        for (std::size_t j = 0; j < nLegs; ++j) nearer += legArea[j] > legArea[i] || (legArea[j] == legArea[i] && j > i) ? 1 : 0;
        const bool nearLeg = nearer < static_cast<int>(nLegs) / 2;
        L.nearLeg = nearLeg;
        const int z = nearLeg ? -2 : -4;
        Mask thigh(sw, sh), shin(sw, sh), foot(sw, sh);
        for (int y = hip; y <= bottom; ++y)
            for (int x = bb.x; x < bb.x + bb.w; ++x)
                if (legMask[i].get(x, y)) (y < knee ? thigh : y < footTop ? shin : foot).set(x, y, true);
        const std::string nm = "leg" + std::to_string(i);
        // Drawn shin, then foot over the ankle, then thigh over the knee.
        L.shin = rig::addPart(rig, sprite, nm + "_shin", shin, L.knee, z);
        L.foot = rig::addPart(rig, sprite, nm + "_foot", foot, L.ankle, z);
        L.thigh = rig::addPart(rig, sprite, nm + "_thigh", thigh, L.hip, z + 1);
    }
    // A biped starts from its drawn pose: the foot drawn further forward is
    // forward in the first frame.
    if (nLegs == 2 && dir > 0) std::swap(legs[0].phase, legs[1].phase);
    std::vector<int> armParts;
    for (std::size_t a = 0; a < subj.arms.size(); ++a) {
        const IntRect& r = subj.arms[a];
        armParts.push_back(rig::addPart(rig, sprite, "arm" + std::to_string(a), armMask[a], {r.x + r.w / 2.0, r.y + 0.5}, 2));
    }
    rig::inpaintHidden(rig, std::max(3, bb.h / 10));
    rig::prepare(rig);

    // Amplitudes in whole pixels. The planted foot moves back `speed` pixels
    // a frame: the stride is a whole number of pixels per frame.
    const int legLen = subj.footRow - subj.legTop + 1;
    const int half = std::max(1, frames / 2);
    const int speed = front ? 0 : std::max(1, iround(legLen * (run ? 0.8 : 0.5) * amount / half));
    const int stride = speed * half;
    const int liftMax = std::max(1, iround(legLen * (run ? 0.35 : 0.22) * amount));
    const int bob = std::max(1, iround(bb.h / 48.0 * amount));
    const double armSwing = (run ? 0.5 : 0.3) * amount;
    const int twistAmp = twist && stride > 0 ? std::max(1, iround(bb.h / 48.0 * amount)) : 0;

    struct LegPose {
        double fx = 0;   // foot offset from under the hip, px (+ = forward)
        int lift = 0;
        bool planted = true;
    };
    auto legPose = [&](const LegRig& L, int f) {
        LegPose p;
        const double q = std::fmod(static_cast<double>(f) / frames + L.phase, 1.0);
        if (q < 0.5) {
            // Planted: back at `speed` px a frame.
            const int k = iround(q * frames);
            p.fx = stride / 2.0 - speed * k;
        } else {
            const double s = (q - 0.5) * 2;
            p.fx = stride * (-0.5 + smoothstep(s));
            p.lift = iround(liftMax * std::pow(std::sin(kPi * s), 0.8));
            p.planted = false;
        }
        p.fx *= dir;
        return p;
    };

    const int m = stride + liftMax + bob + legLen + 6;
    const IntRect big{bb.x - m, bb.y - m, bb.w + 2 * m, bb.h + 2 * m};
    std::vector<Image> frameImgs;
    std::vector<std::vector<rig::PartPose>> allPoses;
    for (int f = 0; f < frames; ++f) {
        const double t = static_cast<double>(f) / frames;
        std::vector<rig::PartPose> pose = rig.rest();
        // Lowest just after each contact, highest at the up pose ...
        int dy = iround(std::sin(4 * kPi * t) * bob * (run ? 1.5 : 1.0));
        // Legs seen from the front cannot stretch: the body only sinks.
        if (front) dy = std::max(0, dy);
        // ... and low enough for every planted foot to reach the ground.
        std::vector<LegPose> lp;
        for (const LegRig& L : legs) lp.push_back(legPose(L, f));
        // The pelvis goes with the near leg's stride, the shoulders against it.
        int pelvisDx = 0;
        if (twistAmp > 0)
            for (std::size_t i = 0; i < legs.size(); ++i)
                if (legs[i].nearLeg) {
                    pelvisDx = dir * iround(twistAmp * std::clamp(lp[i].fx * dir / (stride / 2.0), -1.0, 1.0));
                    break;
                }
        const int shoulderDx = -pelvisDx;
        for (std::size_t i = 0; i < legs.size(); ++i) {
            if (!lp[i].planted || front) continue;
            const LegRig& L = legs[i];
            const double reach = 0.97 * (L.l1 + L.l2);
            const double dx = L.ankle.x + lp[i].fx - (L.hip.x + pelvisDx);
            const double vertical = std::sqrt(std::max(0.0, reach * reach - dx * dx));
            const double need = (L.ankle.y - L.hip.y) - vertical;
            dy = std::max(dy, static_cast<int>(std::ceil(need - 1e-9)));
        }
        pose[static_cast<std::size_t>(bodyPart)].dy = dy;
        if (shoulderPart >= 0) {
            pose[static_cast<std::size_t>(shoulderPart)].dy = dy;
            pose[static_cast<std::size_t>(shoulderPart)].dx = shoulderDx;
        }
        if (pelvisPart >= 0) {
            pose[static_cast<std::size_t>(pelvisPart)].dy = dy;
            pose[static_cast<std::size_t>(pelvisPart)].dx = pelvisDx;
        }
        for (std::size_t i = 0; i < legs.size(); ++i) {
            const LegRig& L = legs[i];
            const LegPose& p = lp[i];
            const rig::Vec hip{L.hip.x + pelvisDx, L.hip.y + dy};
            rig::PartPose& thigh = pose[static_cast<std::size_t>(L.thigh)];
            rig::PartPose& shin = pose[static_cast<std::size_t>(L.shin)];
            rig::PartPose& foot = pose[static_cast<std::size_t>(L.foot)];
            if (front) {
                // In place: the planted foot and shin stay; the swinging
                // foot lifts and the thigh foreshortens (the knee comes
                // towards the viewer) between the sunk hip and the raised knee.
                const double thighRows = std::max(1.0, L.knee.y - L.hip.y);
                const int lift = std::min(p.lift, std::max(0, static_cast<int>(thighRows) - 1 - dy));
                thigh.dy = dy;
                thigh.sy = std::max(0.35, (thighRows - dy - lift) / thighRows);
                shin.dy = -lift;
                foot.dy = -lift;
                continue;
            }
            // The foot: where it is drawn plus the cycle's offset; planted
            // feet keep their row.
            const rig::Vec ankle{L.ankle.x + p.fx, L.ankle.y - p.lift};
            const int bend = (L.hind ? -dir : dir) > 0 ? -1 : 1;
            const rig::Vec knee = rig::solveTwoBone(hip, ankle, L.l1, L.l2, bend);
            // Where the shin's end lands (the ankle, if reachable).
            const double shinA = std::atan2(ankle.y - knee.y, ankle.x - knee.x);
            const rig::Vec reached{knee.x + L.l2 * std::cos(shinA), knee.y + L.l2 * std::sin(shinA)};
            thigh.dx = hip.x - L.hip.x;
            thigh.dy = hip.y - L.hip.y;
            thigh.angle = std::atan2(knee.y - hip.y, knee.x - hip.x) - std::atan2(L.knee.y - L.hip.y, L.knee.x - L.hip.x);
            shin.dx = knee.x - L.knee.x;
            shin.dy = knee.y - L.knee.y;
            shin.angle = shinA - std::atan2(L.ankle.y - L.knee.y, L.ankle.x - L.knee.x);
            // The foot stays flat; a planted foot sits exactly on its spot.
            foot.dx = p.planted ? p.fx : reached.x - L.ankle.x;
            foot.dy = p.planted ? 0 : std::min(0.0, reached.y - L.ankle.y);
            // A turned shin or thigh is wider than its bone: its corner must
            // not poke below the ground this foot stands on.
            thigh.maxY = shin.maxY = L.ground;
        }
        // Arms swing against the leg on their side; front views lift them a little.
        for (std::size_t a = 0; a < armParts.size(); ++a) {
            const IntRect& r = subj.arms[a];
            const bool leftArm = r.x + r.w / 2 < bb.x + bb.w / 2;
            const std::size_t opposite = leftArm ? std::min<std::size_t>(1, legs.size() - 1) : 0;
            rig::PartPose& q = pose[static_cast<std::size_t>(armParts[a])];
            q.dy = dy;
            if (front) {
                q.dy = dy - iround(0.5 * lp[opposite].lift);
                continue;
            }
            q.dx = shoulderDx;
            const double forward = stride > 0 ? lp[opposite].fx * dir / (stride / 2.0) : 0.0;
            q.angle = -dir * armSwing * std::clamp(forward, -1.0, 1.0);
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
