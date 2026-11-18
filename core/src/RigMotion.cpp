#include "acestudio/core/RigMotion.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <map>
#include <set>

namespace acestudio::rigs {

namespace {

using mocap::Vec3;
using MB = mocap::Bone;

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180;

Error bad(std::string m) { return Error{ErrorCode::InvalidArgument, std::move(m)}; }
std::size_t uz(int i) { return static_cast<std::size_t>(i); }

double wrap(double a) {
    while (a > kPi) a -= 2 * kPi;
    while (a < -kPi) a += 2 * kPi;
    return a;
}
double len(Vec3 v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// A stretch of the skeleton: from a joint to the first of `to` that is mapped.
struct Stretch {
    MB from = MB::Hips;
    std::vector<MB> to;
};

// The natural stretch for a joint named on a rig bone.
Stretch stretchFor(MB b) {
    switch (b) {
        case MB::Hips: return {MB::Hips, {MB::Neck, MB::Chest, MB::Spine, MB::Head}};
        case MB::Spine: return {MB::Spine, {MB::Chest, MB::Neck, MB::Head}};
        case MB::Chest: return {MB::Chest, {MB::Neck, MB::Head}};
        case MB::Neck: return {MB::Neck, {MB::Head}};
        case MB::Head: return {MB::Neck, {MB::Head}};
        case MB::LShoulder: return {MB::LShoulder, {MB::LUpperArm}};
        case MB::LUpperArm: return {MB::LUpperArm, {MB::LForearm, MB::LHand}};
        case MB::LForearm: return {MB::LForearm, {MB::LHand}};
        case MB::LHand: return {MB::LForearm, {MB::LHand}};
        case MB::RShoulder: return {MB::RShoulder, {MB::RUpperArm}};
        case MB::RUpperArm: return {MB::RUpperArm, {MB::RForearm, MB::RHand}};
        case MB::RForearm: return {MB::RForearm, {MB::RHand}};
        case MB::RHand: return {MB::RForearm, {MB::RHand}};
        case MB::LThigh: return {MB::LThigh, {MB::LShin}};
        case MB::LShin: return {MB::LShin, {MB::LFoot}};
        case MB::LFoot: return {MB::LFoot, {MB::LToe}};
        case MB::LToe: return {MB::LFoot, {MB::LToe}};
        case MB::RThigh: return {MB::RThigh, {MB::RShin}};
        case MB::RShin: return {MB::RShin, {MB::RFoot}};
        case MB::RFoot: return {MB::RFoot, {MB::RToe}};
        case MB::RToe: return {MB::RFoot, {MB::RToe}};
    }
    return {MB::Hips, {MB::Neck}};
}

bool isLeg(MB b) {
    switch (b) {
        case MB::LThigh: case MB::LShin: case MB::LFoot: case MB::LToe:
        case MB::RThigh: case MB::RShin: case MB::RFoot: case MB::RToe: return true;
        default: return false;
    }
}

// Auto-rig names: "legN thigh|shin|foot" and "armN" (N from 0).
struct AutoName {
    bool leg = false;
    int n = 0;
    int part = 0;   // leg: 0 thigh, 1 shin, 2 foot
};
std::optional<AutoName> autoName(const std::string& name) {
    const std::string s = lower(name);
    auto num = [&](std::size_t at, std::size_t& end) -> int {
        end = at;
        while (end < s.size() && std::isdigit(static_cast<unsigned char>(s[end]))) ++end;
        if (end == at || end - at > 3) return -1;
        return std::stoi(s.substr(at, end - at));
    };
    std::size_t end = 0;
    if (s.rfind("leg", 0) == 0) {
        const int n = num(3, end);
        if (n < 0) return std::nullopt;
        const std::string rest = s.substr(end);
        if (rest == " thigh") return AutoName{true, n, 0};
        if (rest == " shin") return AutoName{true, n, 1};
        if (rest == " foot") return AutoName{true, n, 2};
        return std::nullopt;
    }
    if (s.rfind("arm", 0) == 0) {
        const int n = num(3, end);
        if (n >= 0 && end == s.size()) return AutoName{false, n, 0};
    }
    return std::nullopt;
}

struct Match {
    std::size_t bone = 0;    // index in rig.bones
    int from = -1, to = -1;  // joint indices in the motion
    MB fromBone = MB::Hips, toBone = MB::Hips;
};

} // namespace

mocap::Camera drawnCamera(const Rig& rig) {
    std::map<int, std::size_t> thighOf, footOf;   // leg number -> bone index
    for (std::size_t i = 0; i < rig.bones.size(); ++i)
        if (auto an = autoName(rig.bones[i].name); an && an->leg) {
            if (an->part == 0) thighOf[an->n] = i;
            else if (an->part == 2) footOf[an->n] = i;
        }
    mocap::Camera cam;
    if (thighOf.size() >= 2) {
        const Bone& a = rig.bones[thighOf.begin()->second];
        const Bone& b = rig.bones[std::next(thighOf.begin())->second];
        if (std::abs(a.x - b.x) > 0.25 * std::max({a.length, b.length, 1.0})) {
            cam.turn = 0;
            return cam;
        }
    }
    double facing = 1;
    for (const auto& [n, i] : footOf) facing = std::cos(rig.bones[i].angle) < 0 ? -1 : 1;
    cam.turn = facing < 0 ? -90 : 90;
    return cam;
}

Result<MotionKeysResult> keysFromMotion(const Document& doc, const mocap::Motion& m, const MotionKeysOptions& opt) {
    if (!doc.rig()) return bad("The project has no rig (Animation > Rig > Auto-Rig, or rig one by hand)");
    const Rig& rig = *doc.rig();
    if (rig.bones.empty()) return bad("The rig has no bones");
    if (m.frameCount < 1 || m.joints.empty()) return bad("The motion has no frames");
    if (!std::isfinite(opt.amount) || !std::isfinite(opt.start) || !std::isfinite(opt.length)) return bad("An option is not a number");
    const double amount = std::clamp(opt.amount, 0.25, 2.0);

    struct {
        int framesAdded = 0;
        mocap::Camera camera;
        std::vector<std::pair<std::string, std::string>> matched;
        std::vector<std::string> notes;
    } res;
    const mocap::BoneMap map = mocap::autoMap(m, opt.mapOverrides);
    if (map[MB::Hips] < 0) return bad("The motion's hips were not found; pair them with --map Hips=<joint>");

    // Rest geometry of the rig.
    const std::vector<BoneWorld> rest = solve(rig, RigPose{});
    std::map<BoneId, std::size_t> at;
    for (std::size_t i = 0; i < rig.bones.size(); ++i) at[rig.bones[i].id] = i;

    // ---- Camera: from the rig unless given.
    std::map<int, std::size_t> thighOf, armOf;   // number -> bone index
    for (std::size_t i = 0; i < rig.bones.size(); ++i)
        if (auto an = autoName(rig.bones[i].name)) {
            if (!an->leg) armOf[an->n] = i;
            else if (an->part == 0) thighOf[an->n] = i;
        }
    const double drawnTurn = drawnCamera(rig).turn;
    const bool rigFront = drawnTurn == 0;
    res.camera = opt.camera ? *opt.camera : drawnCamera(rig);
    const mocap::Camera& cam = res.camera;

    // ---- Time span and frames (as retarget).
    const double start = std::clamp(opt.start, 0.0, m.duration());
    const double span = opt.length > 0 ? std::min(opt.length, m.duration() - start) : m.duration() - start;
    if (span <= 0) return bad("The time span is empty");
    const int fps = std::clamp(opt.fps, 1, 60);
    const int frames = std::clamp(opt.frames > 0 ? opt.frames : std::max(2, static_cast<int>(std::lround(span * fps))), 2, 240);
    const double step = opt.loop ? span / frames : span / (frames - 1);

    // ---- The skeleton at rest and over the span.
    const std::vector<Vec3> restW = mocap::restPositions(m);
    double minY = 0, maxY = 0;
    for (const Vec3& p : restW) { minY = std::min(minY, p.y); maxY = std::max(maxY, p.y); }
    const double height = std::max(1e-6, maxY - minY);
    auto J = [&](const std::vector<Vec3>& p, MB b) { return p[uz(map[b])]; };
    std::vector<std::vector<Vec3>> world;
    for (int f = 0; f < frames; ++f) world.push_back(mocap::jointPositions(m, start + f * step));
    const bool feet = map[MB::LFoot] >= 0 && map[MB::RFoot] >= 0;
    double floorY = 1e300;
    for (const auto& w : world) floorY = feet ? std::min({floorY, J(w, MB::LFoot).y, J(w, MB::RFoot).y}) : std::min(floorY, J(w, MB::Hips).y - (J(restW, MB::Hips).y - minY));
    const double standHip = J(restW, MB::Hips).y - (feet ? std::min(J(restW, MB::LFoot).y, J(restW, MB::RFoot).y) : minY);
    const Vec3 hips0 = J(world[0], MB::Hips);
    const Vec3 forward = mocap::characterForward(m, map);
    const Vec3 centre{hips0.x, floorY, hips0.z};
    const mocap::Projected restPr = mocap::project(world[0], cam, forward, height, centre);

    // ---- Which source side drives which numbered leg and arm.
    auto side = [&](MB l, MB r, const std::map<int, std::size_t>& drawn) {
        std::map<int, bool> left;   // number -> source left
        if (map[l] < 0 || map[r] < 0 || drawn.empty()) return left;
        const double sepX = std::abs(restPr.x[uz(map[l])] - restPr.x[uz(map[r])]);
        const double sepD = std::abs(restPr.depth[uz(map[l])] - restPr.depth[uz(map[r])]);
        const bool leftScreenLeft = restPr.x[uz(map[l])] <= restPr.x[uz(map[r])];
        const bool leftNear = restPr.depth[uz(map[l])] >= restPr.depth[uz(map[r])];
        std::vector<std::pair<int, std::size_t>> v(drawn.begin(), drawn.end());
        if (sepX >= sepD) {
            std::sort(v.begin(), v.end(), [&](const auto& a, const auto& b) { return rig.bones[a.second].x < rig.bones[b.second].x; });
            for (std::size_t k = 0; k < v.size(); ++k) left[v[k].first] = (k == 0) == leftScreenLeft;
        } else {
            // Seen from the side: the first numbered is taken as the near one.
            for (std::size_t k = 0; k < v.size(); ++k) left[v[k].first] = (k == 0) == leftNear;
        }
        return left;
    };
    std::map<int, std::size_t> legNums = thighOf;
    for (std::size_t i = 0; i < rig.bones.size(); ++i)
        if (auto an = autoName(rig.bones[i].name); an && an->leg && !legNums.count(an->n)) legNums[an->n] = i;
    const std::map<int, bool> legLeft = side(MB::LThigh, MB::RThigh, legNums);
    const std::map<int, bool> armLeft = side(MB::LUpperArm, MB::RUpperArm, armOf);

    // ---- Match rig bones to stretches.
    std::map<std::string, MB> explicitBones;
    for (const auto& [name, b] : opt.bones) {
        if (!rig.bone(name)) return bad("The rig has no bone named '" + name + "'");
        explicitBones[name] = b;
    }
    std::vector<Match> matches;
    std::vector<int> matchOf(rig.bones.size(), -1);
    for (std::size_t i = 0; i < rig.bones.size(); ++i) {
        const Bone& b = rig.bones[i];
        std::optional<Stretch> st;
        if (auto e = explicitBones.find(b.name); e != explicitBones.end()) {
            st = stretchFor(e->second);
        } else if (const std::string n = lower(b.name); n == "body" || n == "torso" || n == "spine") {
            st = stretchFor(MB::Hips);
        } else if (n == "head") {
            st = stretchFor(MB::Head);
        } else if (auto an = autoName(b.name)) {
            if (an->leg) {
                if (auto s = legLeft.find(an->n); s != legLeft.end()) {
                    const bool L = s->second;
                    const MB parts[3] = {L ? MB::LThigh : MB::RThigh, L ? MB::LShin : MB::RShin, L ? MB::LFoot : MB::RFoot};
                    st = stretchFor(parts[an->part]);
                }
            } else if (auto s = armLeft.find(an->n); s != armLeft.end()) {
                st = Stretch{s->second ? MB::LUpperArm : MB::RUpperArm, {s->second ? MB::LHand : MB::RHand, s->second ? MB::LForearm : MB::RForearm}};
            }
        } else {
            for (int k = 0; k < mocap::kBoneCount; ++k)
                if (lower(std::string(mocap::toString(static_cast<MB>(k)))) == n) st = stretchFor(static_cast<MB>(k));
        }
        if (!st || b.locked || map[st->from] < 0) continue;
        Match mt;
        mt.bone = i;
        mt.from = map[st->from];
        mt.fromBone = st->from;
        for (MB t : st->to)
            if (map[t] >= 0) {
                mt.to = map[t];
                mt.toBone = t;
                break;
            }
        if (mt.to < 0 || len(sub(restW[uz(mt.to)], restW[uz(mt.from)])) < 1e-9) continue;
        matchOf[i] = static_cast<int>(matches.size());
        matches.push_back(mt);
        res.matched.emplace_back(b.name, std::string(mocap::toString(mt.fromBone)) + " -> " + std::string(mocap::toString(mt.toBone)));
    }
    if (matches.empty())
        return bad("No bone of the rig matches the motion's skeleton (auto-rig names like 'leg1 thigh', 'arm1', 'body', or joint names like 'LThigh'); "
                   "pair them with --bones \"rig bone=LThigh,...\"");

    // IK chains whose two bones both match.
    struct ChainUse {
        std::size_t chain = 0, tip = 0, upper = 0;
        double scale = 1;   // rig length per screen unit (character heights)
        bool leg = false;
    };
    std::vector<ChainUse> chains;
    for (std::size_t c = 0; c < rig.chains.size(); ++c) {
        const Bone* tip = rig.bone(rig.chains[c].tip);
        if (!tip || !at.count(tip->id) || !at.count(tip->parent)) continue;
        const std::size_t ti = at[tip->id], ui = at[tip->parent];
        if (matchOf[ti] < 0 || matchOf[ui] < 0 || rig.bones[ui].locked) continue;
        const Match& mt = matches[uz(matchOf[ti])];
        const Match& mu = matches[uz(matchOf[ui])];
        const double src = (len(sub(restW[uz(mu.to)], restW[uz(mu.from)])) + len(sub(restW[uz(mt.to)], restW[uz(mt.from)]))) / height;
        const Bone& u = rig.bones[ui];
        const double drawn = std::hypot(tip->x - u.x, tip->y - u.y) + tip->length;
        if (src < 1e-9 || drawn < 1e-9) continue;
        chains.push_back({c, ti, ui, drawn / src, isLeg(mt.toBone) || isLeg(mu.fromBone)});
        res.matched.emplace_back(rig.chains[c].name, "IK to " + std::string(mocap::toString(mt.toBone)));
    }
    // The rig's ground: the lowest point of its matched leg bones at rest.
    double ground = -1e300;
    for (const Match& mt : matches)
        if (isLeg(mt.fromBone)) ground = std::max({ground, rest[mt.bone].tailY, rest[mt.bone].headY});
    const bool hasGround = ground > -1e299;

    // Rig length per screen unit, from every matched bone.
    double drawnSum = 0, srcSum = 0;
    for (const Match& mt : matches) {
        drawnSum += rig.bones[mt.bone].length;
        srcSum += len(sub(restW[uz(mt.to)], restW[uz(mt.from)])) / height;
    }
    const double k = srcSum > 1e-9 ? drawnSum / srcSum : 1;
    const double tiltCos = std::cos(std::clamp(cam.tilt, -90.0, 90.0) * kDeg);

    // ---- Frames.
    int first = opt.first >= 0 ? opt.first : rig.restFrame + 1;
    if (first > 100000) return bad("The first frame is out of range");
    const int last = first + frames - 1;
    if (rig.restFrame >= first && rig.restFrame <= last)
        return bad("The keys would cover the rest frame " + std::to_string(rig.restFrame) + "; start after it (--first)");
    Document out = doc;
    const int ms = std::clamp(static_cast<int>(std::lround(1000.0 / fps)), 10, 65535);
    while (out.frameCount() <= last) {
        auto f = out.insertFrame(out.frameCount(), ms);
        if (!f) return f.error();
        ++res.framesAdded;
    }
    Rig keyed = rig;
    std::erase_if(keyed.keys, [&](const RigKey& key) { return key.frame >= first && key.frame <= last; });

    std::vector<double> prevWorld(rig.bones.size(), 0), prevTurn(rig.bones.size(), 0);
    std::vector<bool> haveWorld(rig.bones.size(), false);
    for (std::size_t i = 0; i < rig.bones.size(); ++i) prevWorld[i] = rig.bones[i].angle;
    int endOn = 0;
    for (int f = 0; f < frames; ++f) {
        std::vector<Vec3> w = world[uz(f)];
        const Vec3 travel{J(w, MB::Hips).x - hips0.x, 0, J(w, MB::Hips).z - hips0.z};
        if (opt.inPlace)
            for (Vec3& q : w) q = sub(q, travel);
        const mocap::Projected pr = mocap::project(w, cam, forward, height, centre);

        RigPose pose;
        const double hipAbove = J(world[uz(f)], MB::Hips).y - floorY;
        pose.dy = k * tiltCos * (standHip - hipAbove) / height * amount;
        if (!opt.inPlace) pose.dx = k * (pr.x[uz(map[MB::Hips])] - restPr.x[uz(map[MB::Hips])]) * amount;

        // Where each matched stretch points on screen (held when seen end-on).
        std::vector<double> want(rig.bones.size(), 0);
        for (const Match& mt : matches) {
            const double dx = pr.x[uz(mt.to)] - pr.x[uz(mt.from)], dy = pr.y[uz(mt.to)] - pr.y[uz(mt.from)];
            const double full = len(sub(w[uz(mt.to)], w[uz(mt.from)])) / height;
            if (std::hypot(dx, dy) >= 0.15 * full && full > 1e-9) {
                prevWorld[mt.bone] = std::atan2(dy, dx);
                haveWorld[mt.bone] = true;
            } else {
                ++endOn;
            }
            want[mt.bone] = prevWorld[mt.bone];
        }
        // Turns, parents first: each matched bone points where its stretch does.
        std::set<std::size_t> fixed;
        auto aim = [&]() {
            std::vector<double> total(rig.bones.size(), 0);
            for (std::size_t i = 0; i < rig.bones.size(); ++i) {
                const Bone& b = rig.bones[i];
                const double parentTotal = b.parent != kNoBone && at.count(b.parent) ? total[at[b.parent]] : 0;
                double own = pose.turn.count(b.id) ? pose.turn[b.id] : 0;
                if (matchOf[i] >= 0 && !fixed.count(i) && haveWorld[i]) {
                    own = wrap(wrap(want[i] - b.angle) - parentTotal) * amount;
                    pose.turn[b.id] = own;
                }
                total[i] = parentTotal + clampTurn(b, own);
            }
        };
        aim();
        // IK: the chain tip goes where the skeleton's does, scaled to the rig.
        for (const ChainUse& cu : chains) {
            const std::vector<BoneWorld> now = solve(rig, pose);
            const Match& mt = matches[uz(matchOf[cu.tip])];
            const Match& mu = matches[uz(matchOf[cu.upper])];
            const double hx = now[cu.upper].headX, hy = now[cu.upper].headY;
            double tx = hx + (pr.x[uz(mt.to)] - pr.x[uz(mu.from)]) * cu.scale;
            double ty = hy + (pr.y[uz(mt.to)] - pr.y[uz(mu.from)]) * cu.scale;
            // amount: between the rest reach and the skeleton's.
            const double rx = hx + rest[cu.tip].tailX - rest[cu.upper].headX, ry = hy + rest[cu.tip].tailY - rest[cu.upper].headY;
            tx = rx + (tx - rx) * amount;
            ty = ry + (ty - ry) * amount;
            if (cu.leg && hasGround) ty = std::min(ty, ground);
            if (auto st = reach(rig, pose, cu.chain, tx, ty); !st) return st.error();
            fixed.insert(cu.tip);
            fixed.insert(cu.upper);
        }
        if (!chains.empty()) aim();   // bones below a chain follow its new turns

        // Keep turns continuous from key to key (no spins through 180 degrees).
        for (auto& [id, t] : pose.turn) {
            const std::size_t i = at[id];
            if (f > 0) t = prevTurn[i] + wrap(t - prevTurn[i]);
            t = clampTurn(rig.bones[i], t);
            prevTurn[i] = t;
        }
        if (auto st = setKey(keyed, first + f, pose, Curve{}); !st) return st.error();
    }
    if (endOn > frames) res.notes.push_back("Some bones point at the camera in this view; they keep their last direction there.");
    {
        double diff = std::abs(wrap((cam.turn - drawnTurn) * kDeg)) / kDeg;
        if (rigFront) diff = std::min(diff, std::abs(wrap((cam.turn - 180) * kDeg)) / kDeg);
        if (diff > 30 || std::abs(cam.tilt) > 40)
            res.notes.push_back("The camera is far from the view the rig is drawn in; the parts are not redrawn for it.");
    }
    std::size_t unmatched = 0;
    for (std::size_t i = 0; i < rig.bones.size(); ++i)
        if (matchOf[i] < 0) ++unmatched;
    if (unmatched)
        res.notes.push_back(std::to_string(unmatched) + " bone(s) match nothing in the motion and follow their parents.");

    out.setRig(std::move(keyed));
    if (auto st = out.validate(); !st) return st.error();
    return MotionKeysResult{std::move(out), first, frames, res.framesAdded, res.camera, std::move(res.matched), std::move(res.notes)};
}

} // namespace acestudio::rigs
