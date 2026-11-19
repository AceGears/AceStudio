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

bool isArm(MB b) {
    switch (b) {
        case MB::LShoulder: case MB::LUpperArm: case MB::LForearm: case MB::LHand:
        case MB::RShoulder: case MB::RUpperArm: case MB::RForearm: case MB::RHand: return true;
        default: return false;
    }
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
    bool relative = false;   // turns by the stretch's change (armsFromDrawing)
    double ref = 0;          // the stretch's reference direction on screen
    std::string label;       // four-legged motions: what drives it, by joint name
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
    // Two legs standing apart are a front view; four-legged rigs are drawn
    // from the side (their legs stand apart along the body).
    if (thighOf.size() >= 2 && thighOf.size() < 4) {
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
    // A four-legged motion (legs found by shape, Mocap.h creatureMap) drives
    // the rig's numbered legs: hind with hind, front with front, near with near.
    const mocap::CreatureMap cm = mocap::creatureMap(m);
    const bool quad = cm.hasFourLegs();
    if (quad && !opt.bones.empty())
        return bad("Bone pairs (--bones) name humanoid joints; pair a four-legged motion's legs with --legs");
    if (!quad && !opt.legs.empty())
        return bad("Leg pairs (--legs) are for four-legged motions; this motion has no four legs");
    if (opt.facing < -1 || opt.facing > 1) return bad("The facing is left (-1), right (1) or read from the art (0)");
    const int hipsJ = quad ? cm.root : map[MB::Hips];
    if (hipsJ < 0) return bad("The motion's hips were not found; pair them with --map Hips=<joint>");

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
    mocap::Camera drawnCam = drawnCamera(rig);
    const Bone* headBone = rig.bone("neck") ? rig.bone("neck") : rig.bone("head");
    double legMid = 0;
    for (const auto& [n, i] : thighOf) legMid += rest[i].headX;
    if (!thighOf.empty()) legMid /= static_cast<double>(thighOf.size());
    if (quad && opt.facing != 0) {
        drawnCam.turn = opt.facing > 0 ? 90 : -90;
    } else if (quad && headBone && !thighOf.empty() && std::abs(headBone->x - legMid) >= 1.0) {
        // Auto-rig's head bone starts where the head leaves the body.
        drawnCam.turn = headBone->x > legMid ? 90 : -90;
    } else if (quad && thighOf.size() >= 2) {
        // A four-legged side view faces the end its art rises highest at
        // (the head): the top rows of the parts bound to the root bone, from
        // the middle of the legs. Auto-rig feet are blocks, so their bones
        // do not tell.
        double legX = 0;
        for (const auto& [n, i] : thighOf) legX += rest[i].headX;
        legX /= static_cast<double>(thighOf.size());
        int top = 1 << 30;
        double sx = 0;
        int count = 0;
        for (const auto& [layer, bone] : rig.bindings) {
            const Bone* b = rig.bone(bone);
            if (!b || b->parent != kNoBone) continue;
            const Cel* c = doc.cel(layer, rig.restFrame);
            if (!c || c->image.isNull()) continue;
            for (int y = 0; y < c->image.height(); ++y)
                for (int x = 0; x < c->image.width(); ++x) {
                    if (c->image.pixel(x, y).a == 0) continue;
                    const int gy = c->y + y;
                    if (gy < top - 2) {   // a new top: start again
                        top = gy;
                        sx = 0;
                        count = 0;
                    }
                    if (gy <= top + 2) {
                        sx += c->x + x + 0.5;
                        ++count;
                    }
                }
        }
        if (count > 0 && std::abs(sx / count - legX) >= 1.0) drawnCam.turn = sx / count > legX ? 90 : -90;
    }
    const double drawnTurn = drawnCam.turn;
    const bool rigFront = drawnTurn == 0;
    res.camera = opt.camera ? *opt.camera : drawnCam;
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
    std::vector<std::vector<Vec3>> world;
    for (int f = 0; f < frames; ++f) world.push_back(mocap::jointPositions(m, start + f * step));
    auto H = [&](const std::vector<Vec3>& p) { return p[uz(hipsJ)]; };
    std::vector<int> footJs;
    if (quad) {
        for (const auto& leg : cm.legs) footJs.push_back(leg.joints.back());
    } else if (map[MB::LFoot] >= 0 && map[MB::RFoot] >= 0) {
        footJs = {map[MB::LFoot], map[MB::RFoot]};
    }
    const bool feet = !footJs.empty();
    auto lowestFoot = [&](const std::vector<Vec3>& p) {
        double y = 1e300;
        for (int j : footJs) y = std::min(y, p[uz(j)].y);
        return y;
    };
    double floorY = 1e300;
    for (const auto& w : world) floorY = feet ? std::min(floorY, lowestFoot(w)) : std::min(floorY, H(w).y - (H(restW).y - minY));
    // The hips' neutral height over the floor: the skeleton's rest pose, or
    // for a four-legged motion (whose rest pose often stands stiff-legged)
    // the loop's mean height, a one-shot's first frame.
    double standHip = H(restW).y - (feet ? lowestFoot(restW) : minY);
    if (quad) {
        standHip = 0;
        const int n = opt.loop ? frames : 1;
        for (int f = 0; f < n; ++f) standHip += H(world[uz(f)]).y - floorY;
        standHip /= n;
    }
    const Vec3 hips0 = H(world[0]);
    const Vec3 forward = quad ? cm.forward : mocap::characterForward(m, map);
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
    const std::map<int, bool> legLeft = quad ? std::map<int, bool>{} : side(MB::LThigh, MB::RThigh, legNums);
    const std::map<int, bool> armLeft = quad ? std::map<int, bool>{} : side(MB::LUpperArm, MB::RUpperArm, armOf);
    // Four-legged: the rig's legs behind the middle of its legs (the way it
    // faces, from drawnCamera) are hind legs; within a pair the first
    // numbered is the near one (as auto-rig numbers them), and the source's
    // near leg is the one nearer the camera.
    std::map<int, int> quadLeg;   // leg number -> creature leg index
    if (quad && !legNums.empty()) {
        const double facing = drawnTurn < 0 ? -1 : 1;
        double cx = 0;
        for (const auto& [n, i] : legNums) cx += rest[i].headX;
        cx /= static_cast<double>(legNums.size());
        std::vector<int> hind, front;
        for (const auto& [n, i] : legNums) ((rest[i].headX - cx) * facing < 0 ? hind : front).push_back(n);
        if (hind.empty() || front.empty()) {
            // All on one side of the middle: the two furthest back are hind.
            std::vector<int> all;
            for (const auto& [n, i] : legNums) all.push_back(n);
            std::sort(all.begin(), all.end(), [&](int a, int b) { return rest[legNums[a]].headX * facing < rest[legNums[b]].headX * facing; });
            hind.assign(all.begin(), all.begin() + static_cast<long>(std::min<std::size_t>(2, all.size() / 2 + (all.size() == 1))));
            front.assign(all.begin() + static_cast<long>(hind.size()), all.end());
        }
        for (int pair = 0; pair < 2; ++pair) {
            const std::vector<int>& drawn = pair == 0 ? hind : front;
            const int a = pair == 0 ? 0 : 2;   // creatureMap: hind legs first, then front
            const bool aNear = restPr.depth[uz(cm.legs[uz(a)].joints.front())] >= restPr.depth[uz(cm.legs[uz(a + 1)].joints.front())];
            for (std::size_t k = 0; k < drawn.size(); ++k) quadLeg[drawn[k]] = (k % 2 == 0) == aNear ? a : a + 1;
        }
    }
    // Pairs given by hand win.
    for (const auto& [n, which] : opt.legs) {
        if (!legNums.count(n)) return bad("The rig has no leg " + std::to_string(n) + " (auto-rig names: 'leg0 thigh', ...)");
        using QL = MotionKeysOptions::QuadLeg;
        const bool hindLeg = which == QL::HindNear || which == QL::HindFar;
        const bool nearLeg = which == QL::HindNear || which == QL::FrontNear;
        const int a = hindLeg ? 0 : 2;
        const bool aNear = restPr.depth[uz(cm.legs[uz(a)].joints.front())] >= restPr.depth[uz(cm.legs[uz(a + 1)].joints.front())];
        quadLeg[n] = nearLeg == aNear ? a : a + 1;
    }

    // ---- Match rig bones to stretches.
    std::map<std::string, MB> explicitBones;
    for (const auto& [name, b] : opt.bones) {
        if (!rig.bone(name)) return bad("The rig has no bone named '" + name + "'");
        explicitBones[name] = b;
    }
    std::vector<Match> matches;
    std::vector<int> matchOf(rig.bones.size(), -1);
    // The far bone of a long end (auto-rig's "head" on a "neck", "tail tip"
    // on a "tail") follows its parent's turn instead of a stretch of its own.
    struct Follower {
        std::size_t bone, parent;
        bool trails;   // the tail's tip trails; the head keeps half level
    };
    std::vector<Follower> followers;
    for (std::size_t i = 0; i < rig.bones.size(); ++i) {
        const Bone& b = rig.bones[i];
        std::optional<Stretch> st;
        if (quad) {
            // Legs: the thigh from the leg's top to its next joint, the shin
            // from there to the lowest joint (the foot follows the shin).
            // The body turns by the back's change (hind tops to front tops).
            Match mt;
            mt.bone = i;
            const std::string n = lower(b.name);
            const std::string parentName = b.parent != kNoBone && at.count(b.parent) ? lower(rig.bones[at[b.parent]].name) : std::string();
            if (((n == "head" && parentName == "neck") || (n == "tail tip" && parentName == "tail")) && !b.locked) {
                followers.push_back({i, at[b.parent], n != "head"});
                continue;
            }
            if (auto an = autoName(b.name); an && an->leg && an->part < 2 && quadLeg.count(an->n)) {
                const mocap::CreatureLeg& leg = cm.legs[uz(quadLeg[an->n])];
                if (leg.joints.size() < 2 || (an->part == 1 && leg.joints.size() < 3)) continue;
                mt.from = leg.joints[an->part == 0 ? 0 : 1];
                mt.to = an->part == 0 ? leg.joints[1] : leg.joints.back();
                mt.fromBone = an->part == 0 ? MB::LThigh : MB::LShin;
                mt.toBone = an->part == 0 ? MB::LShin : MB::LFoot;
                mt.label = std::string(leg.hind ? "hind" : "front") + " leg " + m.joints[uz(mt.from)].name + " -> " + m.joints[uz(mt.to)].name;
            } else if (((n == "head" || n == "neck") && cm.head >= 0) || (n == "tail" && cm.tail >= 0)) {
                // The head (or the neck that carries it) turns by the change
                // of the line from the front legs' top to the head's end (a
                // nod), the tail by the line from the hind legs' top to its tip.
                const bool head = n != "tail";
                mt.from = cm.legs[head ? 2 : 0].joints.front();
                mt.to = head ? cm.head : cm.tail;
                mt.relative = true;
                mt.label = n + " " + m.joints[uz(mt.from)].name + " -> " + m.joints[uz(mt.to)].name + " (turns by its change)";
            } else if ((n == "body" || n == "torso" || n == "spine") && cm.legs[0].joints.size() && cm.legs[2].joints.size()) {
                mt.from = cm.legs[0].joints.front();
                mt.to = cm.legs[2].joints.front();
                mt.relative = true;
                mt.label = "back " + m.joints[uz(mt.from)].name + " -> " + m.joints[uz(mt.to)].name + " (turns by its change)";
            } else {
                continue;
            }
            if (b.locked || len(sub(restW[uz(mt.to)], restW[uz(mt.from)])) < 1e-9) continue;
            matchOf[i] = static_cast<int>(matches.size());
            matches.push_back(mt);
            res.matched.emplace_back(b.name, mt.label);
            continue;
        }
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
        mt.relative = opt.armsFromDrawing && isArm(mt.fromBone);
        matchOf[i] = static_cast<int>(matches.size());
        matches.push_back(mt);
        res.matched.emplace_back(b.name, std::string(mocap::toString(mt.fromBone)) + " -> " + std::string(mocap::toString(mt.toBone)) +
                                             (mt.relative ? " (from the drawn pose)" : ""));
    }
    if (matches.empty() && quad)
        return bad("No bone of the rig matches the four-legged motion: it needs auto-rig leg names ('leg0 thigh', 'leg0 shin', ...) "
                   "on a rig drawn from the side");
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
        // Four-legged: the straight reach from the leg's top to its foot, at
        // rest, on both sides (a hock or wrist zigzags the source's leg; the
        // drawn leg is often straight).
        const double src = quad ? len(sub(restW[uz(mt.to)], restW[uz(mu.from)])) / height
                                : (len(sub(restW[uz(mu.to)], restW[uz(mu.from)])) + len(sub(restW[uz(mt.to)], restW[uz(mt.from)]))) / height;
        const Bone& u = rig.bones[ui];
        const double drawn = quad ? std::hypot(rest[ti].tailX - rest[ui].headX, rest[ti].tailY - rest[ui].headY)
                                  : std::hypot(tip->x - u.x, tip->y - u.y) + tip->length;
        if (src < 1e-9 || drawn < 1e-9) continue;
        chains.push_back({c, ti, ui, drawn / src, isLeg(mt.toBone) || isLeg(mu.fromBone)});
        res.matched.emplace_back(rig.chains[c].name, "IK to " + (mt.label.empty() ? std::string(mocap::toString(mt.toBone))
                                                                                   : m.joints[uz(mt.to)].name));
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

    // Every frame seen by the camera (travel removed for an in-place cycle).
    std::vector<std::vector<Vec3>> seen;
    std::vector<mocap::Projected> projected;
    for (int f = 0; f < frames; ++f) {
        std::vector<Vec3> w = world[uz(f)];
        const Vec3 travel{H(w).x - hips0.x, 0, H(w).z - hips0.z};
        if (opt.inPlace)
            for (Vec3& q : w) q = sub(q, travel);
        projected.push_back(mocap::project(w, cam, forward, height, centre));
        seen.push_back(std::move(w));
    }
    // A stretch's direction on screen, or nothing when it is seen end-on.
    auto direction = [&](const Match& mt, int f) -> std::optional<double> {
        const mocap::Projected& pr = projected[uz(f)];
        const double dx = pr.x[uz(mt.to)] - pr.x[uz(mt.from)], dy = pr.y[uz(mt.to)] - pr.y[uz(mt.from)];
        const double full = len(sub(seen[uz(f)][uz(mt.to)], seen[uz(f)][uz(mt.from)])) / height;
        if (full > 1e-9 && std::hypot(dx, dy) >= 0.15 * full) return std::atan2(dy, dx);
        return std::nullopt;
    };
    // Arms from the drawing: the reference is the first frame of a one-shot
    // (it starts at rest), else the average direction over the cycle.
    for (Match& mt : matches) {
        if (!mt.relative) continue;
        double sx = 0, sy = 0;
        const std::optional<double> d0 = direction(mt, 0);
        if (!opt.loop && d0) {
            mt.ref = *d0;
            continue;
        }
        for (int f = 0; f < frames; ++f)
            if (auto d = direction(mt, f)) {
                sx += std::cos(*d);
                sy += std::sin(*d);
            }
        mt.ref = std::hypot(sx, sy) > 1e-9 ? std::atan2(sy, sx) : 0;
    }

    std::vector<double> prevWorld(rig.bones.size(), 0), prevTurn(rig.bones.size(), 0);
    std::vector<bool> haveWorld(rig.bones.size(), false);
    for (std::size_t i = 0; i < rig.bones.size(); ++i) prevWorld[i] = rig.bones[i].angle;
    int endOn = 0;
    for (int f = 0; f < frames; ++f) {
        const mocap::Projected& pr = projected[uz(f)];

        RigPose pose;
        const double hipAbove = H(world[uz(f)]).y - floorY;
        pose.dy = k * tiltCos * (standHip - hipAbove) / height * amount;
        if (!opt.inPlace) pose.dx = k * (pr.x[uz(hipsJ)] - restPr.x[uz(hipsJ)]) * amount;

        // Where each matched stretch points on screen (held when seen end-on).
        std::vector<double> want(rig.bones.size(), 0);
        for (const Match& mt : matches) {
            if (const std::optional<double> d = direction(mt, f)) {
                prevWorld[mt.bone] = mt.relative ? rig.bones[mt.bone].angle + wrap(*d - mt.ref) : *d;
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
        // Four-legged: feet keep their drawn angle to the ground (an
        // auto-rig foot is a block under the leg, not a bone of the motion).
        if (quad) {
            std::vector<double> total(rig.bones.size(), 0);
            for (std::size_t i = 0; i < rig.bones.size(); ++i) {
                const Bone& b = rig.bones[i];
                const double parentTotal = b.parent != kNoBone && at.count(b.parent) ? total[at[b.parent]] : 0;
                double own = pose.turn.count(b.id) ? pose.turn[b.id] : 0;
                if (const auto an = autoName(b.name); an && an->leg && an->part == 2 && matchOf[i] < 0 && !b.locked) {
                    own = -parentTotal;
                    pose.turn[b.id] = own;
                }
                total[i] = parentTotal + clampTurn(b, own);
            }
        }

        // Long ends curve: the tail's tip repeats the tail's swing a little
        // later (it trails), the head turns half-way back against its neck.
        for (const Follower& fo : followers) {
            if (matchOf[fo.parent] < 0) continue;
            double own = 0;
            if (fo.trails) {
                const Match& pm = matches[uz(matchOf[fo.parent])];
                const int lag = std::clamp(static_cast<int>(std::lround(0.15 * frames)), 1, frames);
                const int from = opt.loop ? (f - lag + frames) % frames : std::max(0, f - lag);
                if (const std::optional<double> d = direction(pm, from)) own = 0.8 * wrap(*d - pm.ref) * amount;
            } else if (auto pt = pose.turn.find(rig.bones[fo.parent].id); pt != pose.turn.end()) {
                own = -0.5 * pt->second;
            }
            pose.turn[rig.bones[fo.bone].id] = own;
        }

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
    for (const Follower& fo : followers)
        if (matchOf[fo.parent] >= 0) {
            --unmatched;
            res.matched.emplace_back(rig.bones[fo.bone].name,
                                     fo.trails ? "trails the tail (its swing, a little later)" : "half-way back against the neck");
        }
    if (unmatched)
        res.notes.push_back(std::to_string(unmatched) + " bone(s) match nothing in the motion and follow their parents.");

    out.setRig(std::move(keyed));
    if (auto st = out.validate(); !st) return st.error();
    return MotionKeysResult{std::move(out), first, frames, res.framesAdded, res.camera, std::move(res.matched), std::move(res.notes)};
}

std::optional<MotionKeysOptions::QuadLeg> quadLegFromString(std::string_view s) {
    using QL = MotionKeysOptions::QuadLeg;
    if (s == "hind-near") return QL::HindNear;
    if (s == "hind-far") return QL::HindFar;
    if (s == "front-near") return QL::FrontNear;
    if (s == "front-far") return QL::FrontFar;
    return std::nullopt;
}

} // namespace acestudio::rigs
