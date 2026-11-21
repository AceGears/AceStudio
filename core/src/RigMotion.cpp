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
    // A head, neck or tail: drawn shorter as its stretch turns toward or
    // away from the viewer. `shownRef` is how much of the stretch shows when
    // the part is as drawn.
    bool foreshorten = false;
    double shownRef = 0, least = 1;
};

// A flyer rig (auto-rig's "wing", or "near wing" and "far wing", on a
// "body"): the motion's wings (its own wing bones, else its arms) beat the
// wing bones about their shoulders, as retarget --body flyer does. A wing
// is as drawn at the top of the stroke (a loop) or in the first frame (a
// one-shot) and turns down by the fall of the source wing's elevation; with
// the skeleton's own wing bones it is also drawn shorter as the source wing
// folds. The body rises and falls a little with the motion's root.
Result<MotionKeysResult> wingKeys(const Document& doc, const mocap::Motion& m, const MotionKeysOptions& opt, const std::vector<std::size_t>& wingBones) {
    const Rig& rig = *doc.rig();
    const double amount = std::clamp(opt.amount, 0.25, 2.0);
    if (!opt.bones.empty() || !opt.legs.empty()) return bad("Bone and leg pairs (--bones, --legs) do not apply to a wing rig: its wings follow the motion's wings or arms");
    const mocap::BoneMap map = mocap::autoMap(m, opt.mapOverrides);
    const mocap::WingSource ws = mocap::wingSource(m, map, opt.wingRoots);
    if (!ws.found() && !opt.wingRoots.empty())
        return bad("The wing joints given are not two joints of the motion with joints below them");
    if (!ws.found())
        return bad("The motion has nothing to drive the rig's wings: no two chains of joints named wing and no two arms "
                   "(try preset:wingbeat or preset:flap)");
    const double start = std::clamp(opt.start, 0.0, m.duration());
    const double span = opt.length > 0 ? std::min(opt.length, m.duration() - start) : m.duration() - start;
    if (span <= 0) return bad("The time span is empty");
    const int fps = std::clamp(opt.fps, 1, 60);
    const int frames = std::clamp(opt.frames > 0 ? opt.frames : std::max(2, static_cast<int>(std::lround(span * fps))), 2, 240);
    const double step = opt.loop ? span / frames : span / (frames - 1);
    const int first = opt.first >= 0 ? opt.first : rig.restFrame + 1;
    if (first > 100000) return bad("The first frame is out of range");
    const int last = first + frames - 1;
    if (rig.restFrame >= first && rig.restFrame <= last)
        return bad("The keys would cover the rest frame " + std::to_string(rig.restFrame) + "; start after it (--first)");

    // The source wings over the span: elevation and reach, and the root's height.
    int rootJ = 0;
    for (std::size_t j = 0; j < m.joints.size(); ++j)
        if (m.joints[j].parent < 0) {
            rootJ = static_cast<int>(j);
            break;
        }
    std::vector<double> elev[2], reach[2], rootY;
    double longest[2] = {0, 0};
    for (int f = 0; f < frames; ++f) {
        const std::vector<Vec3> w = mocap::jointPositions(m, start + f * step);
        rootY.push_back(w[uz(rootJ)].y);
        for (int a = 0; a < 2; ++a) {
            const Vec3 d = sub(w[uz(ws.tip[a])], w[uz(ws.shoulder[a])]);
            elev[a].push_back(std::atan2(d.y, std::hypot(d.x, d.z)));
            reach[a].push_back(len(d));
            longest[a] = std::max(longest[a], len(d));
        }
    }
    int top[2] = {0, 0};
    if (opt.loop)
        for (int a = 0; a < 2; ++a)
            for (int f = 1; f < frames; ++f)
                if (elev[a][uz(f)] > elev[a][uz(top[a])]) top[a] = f;
    double rootMean = rootY[0];
    if (opt.loop) {
        rootMean = 0;
        for (double y : rootY) rootMean += y / frames;
    }

    // The rig: which way it faces (the body bone points to the head), and
    // which source wing each wing bone takes (the near wing is the one on
    // the viewer's side: the right wing of a rig facing screen right).
    const std::vector<BoneWorld> rest = solve(rig, RigPose{});
    double facing = opt.facing != 0 ? opt.facing : 1;
    if (opt.facing == 0)
        if (const Bone* body = rig.bone("body"); body && std::abs(std::cos(body->angle)) > 0.5) facing = std::cos(body->angle) < 0 ? -1 : 1;
    struct Wing {
        std::size_t bone;
        int src;
        double sign, scale;
    };
    std::vector<Wing> wings;
    double wingLen = 0;
    struct {
        int framesAdded = 0;
        mocap::Camera camera;
        std::vector<std::pair<std::string, std::string>> matched;
        std::vector<std::string> notes;
    } res;
    for (std::size_t i : wingBones) {
        const Bone& b = rig.bones[i];
        if (b.locked) continue;
        const bool farWing = lower(b.name).find("far") != std::string::npos;
        const int nearSrc = facing > 0 ? 1 : 0;
        Wing w{i, farWing ? 1 - nearSrc : nearSrc, 1, farWing ? 0.8 : 1.0};
        // A wing falls toward the tail (as retarget's wings do): anticlockwise
        // on screen for a rig facing right. Only a wing drawn pointing
        // forward more than up or down turns the other way, so it still falls.
        const double dx = rest[i].tailX - rest[i].headX, dy = rest[i].tailY - rest[i].headY;
        w.sign = dx * facing > 0 && std::abs(dy) < std::abs(dx) ? facing : -facing;
        wings.push_back(w);
        wingLen += b.length / static_cast<double>(wingBones.size());
        res.matched.emplace_back(b.name, std::string(ws.own ? "wing " : "arm ") + m.joints[uz(ws.shoulder[w.src])].name + " -> " +
                                             m.joints[uz(ws.tip[w.src])].name + " (turns by its rise and fall)");
    }
    if (wings.empty()) return bad("The rig's wing bones are locked");
    const double srcLen = 0.5 * (longest[0] + longest[1]);
    const double px = srcLen > 1e-9 ? wingLen / srcLen : 0;   // pixels per source unit

    Document out = doc;
    const int ms = std::clamp(static_cast<int>(std::lround(1000.0 / fps)), 10, 65535);
    while (out.frameCount() <= last) {
        auto f = out.insertFrame(out.frameCount(), ms);
        if (!f) return f.error();
        ++res.framesAdded;
    }
    Rig keyed = rig;
    std::erase_if(keyed.keys, [&](const RigKey& key) { return key.frame >= first && key.frame <= last; });
    double least = 1;
    for (int f = 0; f < frames; ++f) {
        RigPose pose;
        pose.dy = std::clamp(-px * (rootY[uz(f)] - rootMean) * amount, -wingLen / 4, wingLen / 4);
        for (const Wing& w : wings) {
            const Bone& b = rig.bones[w.bone];
            const int a = w.src;
            const double down = std::clamp(amount * (elev[a][uz(top[a])] - elev[a][uz(f)]), -0.5, 2.0);
            const double turn = clampTurn(b, w.sign * down * w.scale);
            if (turn != 0.0) pose.turn[b.id] = turn;
            if (ws.own && longest[a] > 1e-9) {
                const double fold = (reach[a][uz(top[a])] - reach[a][uz(f)]) / longest[a];
                const double s = std::clamp(1.0 - amount * fold, 0.55, 1.1);
                if (std::abs(s - 1.0) >= 0.02) {
                    pose.stretch[b.id] = s;
                    least = std::min(least, s);
                }
            }
        }
        if (auto st = setKey(keyed, first + f, pose, Curve{}); !st) return st.error();
    }
    res.notes.push_back(std::string("Wings: the rig's ") + (wings.size() == 1 ? "wing follows" : "wings follow") + " the motion's " +
                        (ws.own ? "own wing bones" : "arms") + "; a wing is as drawn at the top of the stroke.");
    if (least < 0.98)
        res.notes.push_back("The wings fold on the way up: they are drawn up to " + std::to_string(static_cast<int>(std::lround((1 - least) * 100))) + "% shorter.");
    if (!ws.own) res.notes.push_back("With a motion's arms the wings only rise and fall: no folding.");
    std::size_t unmatched = 0;
    for (const Bone& b : rig.bones) {
        bool isWing = false;
        for (const Wing& w : wings) isWing = isWing || rig.bones[w.bone].id == b.id;
        if (!isWing && b.parent != kNoBone) ++unmatched;
    }
    if (unmatched) res.notes.push_back(std::to_string(unmatched) + " bone(s) match nothing in the motion and follow their parents.");
    res.camera = opt.camera ? *opt.camera : mocap::Camera{};
    if (!opt.camera) res.camera.turn = facing < 0 ? -90 : 90;
    out.setRig(std::move(keyed));
    if (auto st = out.validate(); !st) return st.error();
    return MotionKeysResult{std::move(out), first, frames, res.framesAdded, res.camera, std::move(res.matched), std::move(res.notes)};
}

// A swimmer rig (auto-rig's "tail fin", "dorsal fin" and "ventral fin" on
// a "body"): the motion's spine turns the tail fin at the stalk and its fin
// joints paddle the other fins, as retarget --body swimmer does. The body
// is one rigid part and does not bend.
Result<MotionKeysResult> finKeys(const Document& doc, const mocap::Motion& m, const MotionKeysOptions& opt, const std::vector<std::size_t>& finBones) {
    const Rig& rig = *doc.rig();
    const double amount = std::clamp(opt.amount, 0.25, 2.0);
    if (!opt.bones.empty() || !opt.legs.empty()) return bad("Bone and leg pairs (--bones, --legs) do not apply to a fin rig: its fins follow the motion's spine and fin joints");
    mocap::RetargetOptions ro;
    ro.start = opt.start;
    ro.length = opt.length;
    ro.fps = opt.fps;
    ro.frames = opt.frames;
    ro.loop = opt.loop;
    auto read = mocap::swimSource(m, ro);
    if (!read) return bad("The motion cannot drive the rig's fins: " + read.error().message);
    const mocap::SwimSource& sw = read.value();
    const int frames = sw.frames;
    const int fps = std::clamp(opt.fps, 1, 60);
    const int first = opt.first >= 0 ? opt.first : rig.restFrame + 1;
    if (first > 100000) return bad("The first frame is out of range");
    const int last = first + frames - 1;
    if (rig.restFrame >= first && rig.restFrame <= last)
        return bad("The keys would cover the rest frame " + std::to_string(rig.restFrame) + "; start after it (--first)");

    // The rig: the body bone runs from the tail stalk to the head.
    const std::vector<BoneWorld> rest = solve(rig, RigPose{});
    const Bone* body = rig.bone("body");
    std::size_t bodyAt = 0;
    for (std::size_t i = 0; i < rig.bones.size(); ++i)
        if (body && rig.bones[i].id == body->id) bodyAt = i;
    double dir = 1;   // +1: the head is at screen left
    if (opt.facing != 0) dir = opt.facing > 0 ? -1 : 1;
    else if (body) dir = (rest[bodyAt].tailX - rest[bodyAt].headX) < 0 ? 1 : -1;
    bool hasTail = false;
    for (std::size_t i : finBones) hasTail = hasTail || lower(rig.bones[i].name) == "tail fin";

    struct {
        int framesAdded = 0;
        mocap::Camera camera;
        std::vector<std::pair<std::string, std::string>> matched;
        std::vector<std::string> notes;
    } res;
    res.notes = sw.notes;
    struct Fin {
        std::size_t bone;
        bool tail, top;
        int drive;
    };
    std::vector<Fin> fins;
    std::size_t riding = 0;
    for (std::size_t i : finBones) {
        const Bone& b = rig.bones[i];
        if (b.locked) continue;
        const std::string n = lower(b.name);
        Fin f{i, n == "tail fin", n.rfind("dorsal", 0) == 0, -1};
        if (f.tail) {
            res.matched.emplace_back(b.name, "spine " + m.joints[uz(sw.chain.front())].name + " -> " + m.joints[uz(sw.chain.back())].name + " (turns as its end turns)");
        } else {
            // Where it joins, from the head along the body.
            double u = 0.5;
            const double bx = rest[bodyAt].tailX - rest[bodyAt].headX, by = rest[bodyAt].tailY - rest[bodyAt].headY;
            if (body && bx * bx + by * by > 1e-9) {
                const double t = ((rest[i].headX - rest[bodyAt].headX) * bx + (rest[i].headY - rest[bodyAt].headY) * by) / (bx * bx + by * by);
                u = std::clamp(1.0 - t, 0.0, 1.0);
            }
            auto at = [&](std::size_t q) { return std::min(1.0, sw.fins[q].u / (hasTail ? 0.8 : 1.0)); };
            for (std::size_t k = 0; k < sw.fins.size(); ++k) {
                if (sw.fins[k].top != f.top) continue;
                if (f.drive < 0 || std::abs(at(k) - u) < std::abs(at(uz(f.drive)) - u)) f.drive = static_cast<int>(k);
            }
            if (f.drive < 0) {
                ++riding;
                continue;
            }
            res.matched.emplace_back(b.name, "fin " + sw.fins[uz(f.drive)].name + " (paddles with it)");
        }
        fins.push_back(f);
    }
    if (fins.empty())
        return bad("The motion has nothing to drive the rig's fins: the rig has no tail fin, and the motion no moving fin joint off its spine "
                   "(joints named fin, flipper, pectoral, pelvic or dorsal; try preset:swim)");

    Document out = doc;
    const int ms = std::clamp(static_cast<int>(std::lround(1000.0 / fps)), 10, 65535);
    while (out.frameCount() <= last) {
        auto f = out.insertFrame(out.frameCount(), ms);
        if (!f) return f.error();
        ++res.framesAdded;
    }
    Rig keyed = rig;
    std::erase_if(keyed.keys, [&](const RigKey& key) { return key.frame >= first && key.frame <= last; });
    double least = 1, peak = 0;
    for (int f = 0; f < frames; ++f) {
        RigPose pose;
        for (const Fin& fn : fins) {
            const Bone& b = rig.bones[fn.bone];
            double turn = 0;
            if (fn.tail) {
                turn = dir * std::clamp(sw.tailTurn(f, 0.8) * amount, -0.6, 0.6);
            } else {
                const mocap::SwimFinSource& fd = sw.fins[uz(fn.drive)];
                // Toward the tail: anticlockwise for a fin hanging down on a
                // fish facing left, clockwise for one standing up.
                turn = std::clamp(fd.turn[uz(f)] * amount, -0.35, 0.35) * (fn.top ? dir : -dir);
                if (!fd.shown.empty()) {
                    const double sc = std::clamp(1.0 + (fd.shown[uz(f)] - 1.0) * amount, 0.5, 1.25);
                    if (std::abs(sc - 1.0) >= 0.02) {
                        pose.stretch[b.id] = sc;
                        least = std::min(least, sc);
                    }
                }
            }
            turn = clampTurn(b, turn);
            peak = std::max(peak, std::abs(turn));
            if (turn != 0.0) pose.turn[b.id] = turn;
        }
        if (auto st = setKey(keyed, first + f, pose, Curve{}); !st) return st.error();
    }
    res.notes.push_back("Fins: the tail fin turns with the end of the motion's spine and the other fins paddle with its fin joints; "
                        "the body is one rigid part and does not bend (retarget --body swimmer bends it).");
    if (peak < 0.02) res.notes.push_back("The motion hardly moves the fins (try preset:swim).");
    if (least < 0.98)
        res.notes.push_back("A fin swings toward or away from the viewer: it is drawn up to " + std::to_string(static_cast<int>(std::lround((1 - least) * 100))) + "% shorter there.");
    if (riding)
        res.notes.push_back(std::to_string(riding) + " fin(s) stay as drawn: the motion has no moving fin joint of their kind off its spine "
                            "(joints named fin, flipper, pectoral, pelvic or dorsal; preset:swim has them).");
    res.camera = opt.camera ? *opt.camera : mocap::Camera{};
    if (!opt.camera) res.camera.turn = dir > 0 ? -90 : 90;
    out.setRig(std::move(keyed));
    if (auto st = out.validate(); !st) return st.error();
    return MotionKeysResult{std::move(out), first, frames, res.framesAdded, res.camera, std::move(res.matched), std::move(res.notes)};
}

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

    // A wing rig (auto-rig's wings: no numbered legs or arms) goes its own way.
    {
        std::vector<std::size_t> wingBones;
        bool limbs = false;
        for (std::size_t i = 0; i < rig.bones.size(); ++i) {
            const std::string n = lower(rig.bones[i].name);
            if (n == "wing" || n == "near wing" || n == "far wing") wingBones.push_back(i);
            limbs = limbs || autoName(rig.bones[i].name).has_value();
        }
        if (!wingBones.empty() && !limbs) return wingKeys(doc, m, opt, wingBones);
        // A fin rig (auto-rig's fins) likewise.
        std::vector<std::size_t> finBones;
        for (std::size_t i = 0; i < rig.bones.size(); ++i) {
            const std::string n = lower(rig.bones[i].name);
            if (n == "tail fin" || n.rfind("dorsal fin", 0) == 0 || n.rfind("ventral fin", 0) == 0) finBones.push_back(i);
        }
        if (!finBones.empty() && wingBones.empty() && !limbs) return finKeys(doc, m, opt, finBones);
    }

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
        for (std::size_t i = 0; i < cm.legCount(); ++i) footJs.push_back(cm.leg(i).joints.back());
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
        // A motion with middle pairs on a rig with more than four legs: the
        // rig's legs are grouped from the back forward instead, the groups
        // between the first and last taking the middle pairs.
        const std::size_t srcPairs = cm.legCount() / 2;
        std::vector<std::pair<int, std::vector<int>>> groups;   // the source pair's first leg, the rig's leg numbers
        if (srcPairs > 2 && legNums.size() > 4) {
            std::vector<int> all;
            for (const auto& [n, i] : legNums) all.push_back(n);
            std::stable_sort(all.begin(), all.end(), [&](int la, int lb) { return rest[legNums[la]].headX * facing < rest[legNums[lb]].headX * facing; });
            const std::size_t count = std::min(srcPairs, all.size() / 2);
            groups.resize(count);
            for (std::size_t gi = 0; gi < count; ++gi) {
                int first = 0;
                if (gi + 1 == count) first = 2;
                else if (gi > 0)
                    first = 4 + 2 * static_cast<int>(std::clamp<long>(std::lround(static_cast<double>(gi * (srcPairs - 1)) / static_cast<double>(count - 1)) - 1, 0,
                                                                      static_cast<long>(srcPairs) - 3));
                groups[gi].first = first;
            }
            for (std::size_t r = 0; r < all.size(); ++r) groups[r * count / all.size()].second.push_back(all[r]);
            for (auto& grp : groups) std::sort(grp.second.begin(), grp.second.end());
            res.notes.push_back("The motion has " + std::to_string(cm.legCount()) + " legs and the rig " + std::to_string(all.size()) +
                                ": they are paired from the back forward.");
        } else {
            groups = {{0, hind}, {2, front}};   // creatureMap: hind legs first, then front
            if (srcPairs > 2)
                res.notes.push_back("The motion has " + std::to_string(cm.legCount()) + " legs; the rig's " + std::to_string(legNums.size()) +
                                    " take its hind and front pairs (the middle legs are not used).");
            else if (legNums.size() > 4)
                res.notes.push_back("The rig has " + std::to_string(legNums.size()) +
                                    " legs and the motion four: the legs behind the middle follow its hind pair, the others its front pair.");
        }
        for (const auto& [a, drawn] : groups) {
            const bool aNear = restPr.depth[uz(cm.leg(uz(a)).joints.front())] >= restPr.depth[uz(cm.leg(uz(a + 1)).joints.front())];
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
                const int which = quadLeg[an->n];
                const mocap::CreatureLeg& leg = cm.leg(uz(which));
                if (leg.joints.size() < 2 || (an->part == 1 && leg.joints.size() < 3)) continue;
                mt.from = leg.joints[an->part == 0 ? 0 : 1];
                mt.to = an->part == 0 ? leg.joints[1] : leg.joints.back();
                mt.fromBone = an->part == 0 ? MB::LThigh : MB::LShin;
                mt.toBone = an->part == 0 ? MB::LShin : MB::LFoot;
                mt.label = std::string(which >= 4 ? "middle" : leg.hind ? "hind" : "front") + " leg " + m.joints[uz(mt.from)].name + " -> " + m.joints[uz(mt.to)].name;
            } else if (((n == "head" || n == "neck") && cm.head >= 0) || (n == "tail" && cm.tail >= 0)) {
                // The head (or the neck that carries it) turns by the change
                // of the line from the front legs' top to the head's end (a
                // nod), the tail by the line from the hind legs' top to its tip.
                const bool head = n != "tail";
                mt.from = cm.legs[head ? 2 : 0].joints.front();
                mt.to = head ? cm.head : cm.tail;
                mt.relative = true;
                mt.foreshorten = true;
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

    // How much of a stretch shows on screen (1 = side-on to the viewer).
    auto shown = [&](const Match& mt, int f) {
        const mocap::Projected& pr = projected[uz(f)];
        const double full = len(sub(seen[uz(f)][uz(mt.to)], seen[uz(f)][uz(mt.from)])) / height;
        return full > 1e-9 ? std::hypot(pr.x[uz(mt.to)] - pr.x[uz(mt.from)], pr.y[uz(mt.to)] - pr.y[uz(mt.from)]) / full : 1.0;
    };
    // As drawn: a loop's most side-on moment, a one-shot's first frame.
    for (Match& mt : matches) {
        if (!mt.foreshorten) continue;
        mt.shownRef = shown(mt, 0);
        if (opt.loop)
            for (int f = 1; f < frames; ++f) mt.shownRef = std::max(mt.shownRef, shown(mt, f));
        if (mt.shownRef < 0.15) mt.foreshorten = false;   // seen end-on: nothing to compare with
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

        // Sideways swing: a head, neck or tail turned toward or away from
        // the viewer shows shorter (its far bone, a child, with it).
        for (Match& mt : matches) {
            if (!mt.foreshorten) continue;
            const double s = 1 + (std::clamp(shown(mt, f) / mt.shownRef, 0.4, 1.25) - 1) * amount;
            if (std::abs(s - 1) < 0.02) continue;
            pose.stretch[rig.bones[mt.bone].id] = std::clamp(s, 0.25, 1.5);
            mt.least = std::min(mt.least, s);
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
    for (const Match& mt : matches)
        if (mt.foreshorten && mt.least < 0.98)
            res.notes.push_back("The " + lower(rig.bones[mt.bone].name) + " swings toward or away from the viewer: it is drawn up to " +
                                std::to_string(static_cast<int>(std::lround((1 - std::max(mt.least, 0.25)) * 100))) + "% shorter there.");
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
