// The motion library: built-in presets made by code, BVH writing, and saved
// presets in a folder (MotionLibrary.h).

#include "acestudio/core/MotionLibrary.h"

#include "acestudio/core/FileUtil.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <functional>
#include <set>

namespace acestudio::mocap {

namespace fs = std::filesystem;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;
constexpr int kFps = 30;

std::size_t uz(int v) { return static_cast<std::size_t>(v); }

std::string utf8(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

// ---- The preset skeleton --------------------------------------------------------

// Leg bone lengths (thigh, shin), in skeleton units (about 170 to the head top).
constexpr double kThigh = 42, kShin = 40;

// What a preset moves, per frame. Angles in degrees; distances in skeleton units.
//   Lean: the spine tips forward (+) or back (-); Chest adds to its top half;
//   Twist turns the spine about the vertical; Neck nods the head down (+).
//   ArmX swings an arm forward (-) or back (+); ArmZ lifts it out to the side;
//   Fore bends the elbow (+). Thigh swings a leg forward (-); Knee bends it (+).
//   Lift raises the hips above where the legs put them (jumps, run flight).
enum P : std::size_t { RootX, Lift, RootZ, Lean, Chest, Twist, Neck, LArmX, LArmZ, LFore, RArmX, RArmZ, RFore, LThigh, LKnee, RThigh, RKnee, kParams };
using Pose = std::array<double, kParams>;

Pose rest() {
    Pose p{};
    p[LArmZ] = p[RArmZ] = 8;
    p[LFore] = p[RFore] = 10;
    p[LThigh] = p[RThigh] = -3;
    p[LKnee] = p[RKnee] = 6;
    return p;
}

Pose restWith(std::initializer_list<std::pair<P, double>> changes) {
    Pose p = rest();
    for (const auto& [k, v] : changes) p[k] = v;
    return p;
}

struct Key {
    double t;
    Pose pose;
};

// Smooth (ease-in-out) blend between keys; `keys` are in time order.
Pose keyed(const std::vector<Key>& keys, double t) {
    if (t <= keys.front().t) return keys.front().pose;
    for (std::size_t i = 1; i < keys.size(); ++i) {
        if (t > keys[i].t) continue;
        const double span = keys[i].t - keys[i - 1].t;
        const double s = span > 0 ? (t - keys[i - 1].t) / span : 1.0;
        const double e = s * s * (3 - 2 * s);
        Pose p{};
        for (std::size_t k = 0; k < kParams; ++k) p[k] = keys[i - 1].pose[k] * (1 - e) + keys[i].pose[k] * e;
        return p;
    }
    return keys.back().pose;
}

Pose idle(double t) {
    const double b = 0.5 - 0.5 * std::cos(2 * kPi * t / 2.0);   // breath, 0..1..0 over 2 s
    Pose p = rest();
    p[Chest] = -2 + 3 * b;
    p[Neck] = 1.5 * b;
    p[LArmX] = p[RArmX] = 2 * std::sin(2 * kPi * t / 2.0 + 0.6);
    p[LKnee] = p[RKnee] = 6 + 4 * b;
    p[LThigh] = p[RThigh] = -3 - 2 * b;
    return p;
}

// A step cycle: `stride` (degrees of thigh swing), `kneeLift` (bend while the
// leg swings forward), `kneeBase` (bend in the stance), arms against the legs.
Pose cycle(double t, double period, double stride, double kneeBase, double kneeLift, double armSwing, double elbow, double lean,
           double bounce, double speed) {
    Pose p = rest();
    for (int side = 0; side < 2; ++side) {
        const double ph = 2 * kPi * (t / period + side * 0.5);
        const double thigh = -stride * std::sin(ph);
        p[side == 0 ? LThigh : RThigh] = thigh;
        p[side == 0 ? LKnee : RKnee] = kneeBase + kneeLift * std::max(0.0, std::cos(ph));
        p[side == 0 ? LArmX : RArmX] = -armSwing * thigh;
        p[side == 0 ? LFore : RFore] = elbow;
    }
    p[Lean] = lean;
    p[Twist] = 4 * std::sin(2 * kPi * t / period);
    p[Lift] = bounce * std::abs(std::sin(2 * kPi * t / period));
    p[RootZ] = speed * t;
    return p;
}

Pose walk(double t) {
    const double stride = 20;
    // One stride per step: about the distance the feet cover, so they do not slide.
    const double speed = 4 * (kThigh + kShin) * std::sin(stride * kDeg) / 1.0;
    return cycle(t, 1.0, stride, 0, 63, 0.8, 15, 3, 0, speed);
}

Pose run(double t) {
    const double stride = 38, period = 0.6;
    const double speed = 4 * (kThigh + kShin) * std::sin(stride * kDeg) / period;
    return cycle(t, period, stride, 18, 85, 1.1, 85, 12, 5, speed);
}

const std::vector<Key>& jumpKeys() {
    static const std::vector<Key> k = {
        {0.00, rest()},
        {0.25, restWith({{Lean, 22}, {LThigh, -48}, {RThigh, -48}, {LKnee, 95}, {RKnee, 95}, {LArmX, 45}, {RArmX, 45}, {LFore, 25}, {RFore, 25}})},
        {0.38, restWith({{Lift, 8}, {Lean, 4}, {LThigh, 0}, {RThigh, 0}, {LKnee, 0}, {RKnee, 0}, {LArmX, -150}, {RArmX, -150}, {LFore, 5}, {RFore, 5}})},
        {0.55, restWith({{Lift, 32}, {LThigh, -32}, {RThigh, -32}, {LKnee, 60}, {RKnee, 60}, {LArmX, -120}, {RArmX, -120}})},
        {0.72, restWith({{Lift, 10}, {LThigh, -12}, {RThigh, -12}, {LKnee, 18}, {RKnee, 18}, {LArmX, -60}, {RArmX, -60}})},
        {0.80, restWith({{Lean, 16}, {LThigh, -42}, {RThigh, -42}, {LKnee, 84}, {RKnee, 84}, {LArmX, -15}, {RArmX, -15}, {LFore, 30}, {RFore, 30}})},
        {1.00, rest()},
    };
    return k;
}

const std::vector<Key>& attackKeys() {
    // A right-hand punch with a step: guard, wind up, strike, hold, recover.
    static const std::vector<Key> k = {
        {0.00, rest()},
        {0.08, restWith({{LArmX, -35}, {RArmX, -30}, {LFore, 100}, {RFore, 110}, {LKnee, 12}, {RKnee, 12}, {LThigh, -6}, {RThigh, -6}})},
        {0.18, restWith({{Twist, 22}, {Lean, -3}, {LArmX, -35}, {RArmX, 15}, {LFore, 100}, {RFore, 120}, {LKnee, 14}, {RKnee, 14}, {LThigh, -7}, {RThigh, -7}})},
        {0.30, restWith({{Twist, -26}, {Lean, 9}, {LArmX, -25}, {RArmX, -88}, {LFore, 110}, {RFore, 2}, {LThigh, -22}, {LKnee, 18}, {RThigh, 10}, {RKnee, 6}})},
        {0.42, restWith({{Twist, -22}, {Lean, 8}, {LArmX, -25}, {RArmX, -82}, {LFore, 110}, {RFore, 6}, {LThigh, -20}, {LKnee, 16}, {RThigh, 9}, {RKnee, 6}})},
        {0.60, rest()},
    };
    return k;
}

const std::vector<Key>& hurtKeys() {
    static const std::vector<Key> k = {
        {0.00, rest()},
        {0.12, restWith({{Lean, -18}, {Neck, -15}, {LArmX, 30}, {RArmX, 30}, {LArmZ, 25}, {RArmZ, 25}, {LFore, 35}, {RFore, 35}, {LThigh, -8}, {RThigh, -8}, {LKnee, 20}, {RKnee, 20}})},
        {0.30, restWith({{Lean, -10}, {Neck, -6}, {LArmX, 15}, {RArmX, 15}, {LArmZ, 18}, {RArmZ, 18}, {LFore, 25}, {RFore, 25}, {LKnee, 14}, {RKnee, 14}})},
        {0.50, rest()},
    };
    return k;
}

struct Builtin {
    const char* name;
    const char* description;
    double seconds;
    bool loops;
    Pose (*pose)(double);       // humanoid presets
    Motion (*make)() = nullptr; // others (four-legged), made whole
    bool fourLegged = false;
};

Motion quadWalk();

Pose jump(double t) { return keyed(jumpKeys(), t); }
Pose attack(double t) { return keyed(attackKeys(), t); }
Pose hurt(double t) { return keyed(hurtKeys(), t); }

constexpr Builtin kBuiltins[] = {
    {"idle", "Standing, breathing: a slow 2 s loop", 2.0, true, idle},
    {"walk", "A walk cycle: one step per leg in 1 s, arms swinging", 1.0, true, walk},
    {"run", "A run cycle: longer strides, bent arms, forward lean, a flight phase (0.6 s)", 0.6, true, run},
    {"jump", "Crouch, take off, tuck, land and recover (1 s, starts and ends at rest)", 1.0, false, jump},
    {"attack", "A right-hand punch with a step: guard, wind-up, strike, recover (0.6 s)", 0.6, false, attack},
    {"hurt", "Knocked back: recoil and recover (0.5 s)", 0.5, false, hurt},
    {"quad-walk", "A four-legged walk: hind, front, hind, front, one stride in 1.2 s (for four-legged sprites)", 1.2, true, nullptr,
     quadWalk, true},
};

const Builtin* findBuiltin(std::string_view name) {
    for (const Builtin& b : kBuiltins)
        if (name == b.name) return &b;
    return nullptr;
}

// Joint indexes in the order presetSkeleton adds them.
struct Skeleton {
    Motion m;
    int hips = -1, spine = -1, spine1 = -1, neck = -1, head = -1;
    int arm[2] = {-1, -1}, fore[2] = {-1, -1}, upLeg[2] = {-1, -1}, leg[2] = {-1, -1}, foot[2] = {-1, -1};
};

Skeleton presetSkeleton() {
    Skeleton s;
    auto add = [&](std::string name, int parent, Vec3 offset, bool root = false, bool end = false) {
        Joint j;
        j.name = end ? s.m.joints[uz(parent)].name + "_end" : std::move(name);
        j.parent = parent;
        j.offset = offset;
        j.endSite = end;
        if (root)
            j.channels = {Channel::Xposition, Channel::Yposition, Channel::Zposition, Channel::Zrotation, Channel::Xrotation, Channel::Yrotation};
        else if (!end)
            j.channels = {Channel::Zrotation, Channel::Xrotation, Channel::Yrotation};
        j.firstChannel = s.m.channelCount;
        s.m.channelCount += static_cast<int>(j.channels.size());
        s.m.joints.push_back(std::move(j));
        return static_cast<int>(s.m.joints.size()) - 1;
    };
    s.hips = add("Hips", -1, {0, 0, 0}, true);
    s.spine = add("Spine", s.hips, {0, 10, 0});
    s.spine1 = add("Spine1", s.spine, {0, 10, 0});
    s.neck = add("Neck", s.spine1, {0, 12, 0});
    s.head = add("Head", s.neck, {0, 8, 0});
    add({}, s.head, {0, 10, 0}, false, true);
    for (int side = 0; side < 2; ++side) {
        const double sx = side == 0 ? 1 : -1;   // left is +X (the character faces +Z)
        const std::string pre = side == 0 ? "Left" : "Right";
        const int sh = add(pre + "Shoulder", s.spine1, {4 * sx, 8, 0});
        s.arm[side] = add(pre + "Arm", sh, {4 * sx, 0, 0});
        s.fore[side] = add(pre + "ForeArm", s.arm[side], {0, -25, 0});
        const int hand = add(pre + "Hand", s.fore[side], {0, -22, 0});
        add({}, hand, {0, -8, 0}, false, true);
    }
    for (int side = 0; side < 2; ++side) {
        const double sx = side == 0 ? 1 : -1;
        const std::string pre = side == 0 ? "Left" : "Right";
        s.upLeg[side] = add(pre + "UpLeg", s.hips, {9 * sx, -5, 0});
        s.leg[side] = add(pre + "Leg", s.upLeg[side], {0, -kThigh, 0});
        s.foot[side] = add(pre + "Foot", s.leg[side], {0, -kShin, 0});
        const int toe = add(pre + "ToeBase", s.foot[side], {0, -6, 12});
        add({}, toe, {0, 0, 6}, false, true);
    }
    return s;
}

Motion generate(const Builtin& b) {
    if (b.make) return b.make();
    Skeleton s = presetSkeleton();
    Motion& m = s.m;
    const int steps = static_cast<int>(std::lround(b.seconds * kFps));
    m.frameCount = b.loops ? steps : steps + 1;
    m.frameTime = 1.0 / kFps;
    m.values.assign(uz(m.frameCount) * uz(m.channelCount), 0.0);
    for (int f = 0; f < m.frameCount; ++f) {
        const Pose p = b.pose(static_cast<double>(f) / kFps);
        double* v = &m.values[uz(f) * uz(m.channelCount)];
        // Channels are Z, X, Y rotation (after the root's X, Y, Z position).
        auto rot = [&](int joint, double x, double y, double z) {
            const int c = m.joints[uz(joint)].firstChannel;
            v[c] = z;
            v[c + 1] = x;
            v[c + 2] = y;
        };
        // The hips sit so the lower ankle is on the floor (y = 0), plus Lift.
        double vertical = 0;
        for (int side = 0; side < 2; ++side) {
            const double th = p[side == 0 ? LThigh : RThigh] * kDeg, kn = p[side == 0 ? LKnee : RKnee] * kDeg;
            vertical = std::max(vertical, kThigh * std::cos(th) + kShin * std::cos(th + kn));
        }
        v[0] = p[RootX];
        v[1] = vertical + 5 + std::max(0.0, p[Lift]);   // lowering comes from the knees
        v[2] = p[RootZ];
        rot(s.spine, p[Lean] / 2, p[Twist] / 2, 0);
        rot(s.spine1, p[Lean] / 2 + p[Chest], p[Twist] / 2, 0);
        rot(s.neck, p[Neck] - p[Lean] / 2, -p[Twist] / 2, 0);
        for (int side = 0; side < 2; ++side) {
            const double out = p[side == 0 ? LArmZ : RArmZ];
            rot(s.arm[side], p[side == 0 ? LArmX : RArmX], 0, side == 0 ? out : -out);
            rot(s.fore[side], -p[side == 0 ? LFore : RFore], 0, 0);
            const double th = p[side == 0 ? LThigh : RThigh], kn = p[side == 0 ? LKnee : RKnee];
            rot(s.upLeg[side], th, 0, 0);
            rot(s.leg[side], kn, 0, 0);
            rot(s.foot[side], -(th + kn), 0, 0);   // the foot stays flat
        }
    }
    return std::move(s.m);
}

// ---- The four-legged preset --------------------------------------------------------

// A four-legged animal, Y up, facing +Z, its back level: Hips (the pelvis,
// the root) and a spine forward to the chest, a neck and head up and
// forward, a tail back; hind legs under the hips and front legs under the
// chest, each three bones and a toe end. Units as the humanoid (a dog about
// 60 high at the back).
constexpr double kQuadThigh = 20, kQuadShin = 18, kQuadCannon = 14;   // hind
constexpr double kQuadUpper = 18, kQuadFore = 18, kQuadPastern = 12;  // front

Motion quadWalk() {
    Motion m;
    auto add = [&](std::string name, int parent, Vec3 offset, bool root = false, bool end = false) {
        Joint j;
        j.name = end ? m.joints[uz(parent)].name + "_end" : std::move(name);
        j.parent = parent;
        j.offset = offset;
        j.endSite = end;
        if (root)
            j.channels = {Channel::Xposition, Channel::Yposition, Channel::Zposition, Channel::Zrotation, Channel::Xrotation, Channel::Yrotation};
        else if (!end)
            j.channels = {Channel::Zrotation, Channel::Xrotation, Channel::Yrotation};
        j.firstChannel = m.channelCount;
        m.channelCount += static_cast<int>(j.channels.size());
        m.joints.push_back(std::move(j));
        return static_cast<int>(m.joints.size()) - 1;
    };
    const int hips = add("Hips", -1, {0, 0, 0}, true);
    const int spine = add("Spine", hips, {0, 2, 16});
    const int chest = add("Chest", spine, {0, 0, 18});
    const int neck = add("Neck", chest, {0, 8, 6});
    const int head = add("Head", neck, {0, 10, 6});
    add({}, head, {0, 0, 12}, false, true);
    const int tail = add("Tail", hips, {0, 2, -6});
    const int tail1 = add("Tail1", tail, {0, -4, -10});
    add({}, tail1, {0, -6, -8}, false, true);
    // Legs: [0] left hind, [1] right hind, [2] left front, [3] right front
    // (left is +X when facing +Z).
    int upper[4], lower[4], foot[4], toes[4];
    for (int i = 0; i < 4; ++i) {
        const bool hind = i < 2;
        const double sx = i % 2 == 0 ? 1 : -1;
        const std::string pre = std::string(i % 2 == 0 ? "Left" : "Right") + (hind ? "Hind" : "Front");
        upper[i] = add(pre + "Thigh", hind ? hips : chest, {6 * sx, hind ? -4.0 : -6.0, 0});
        lower[i] = add(pre + "Shin", upper[i], {0, -(hind ? kQuadThigh : kQuadUpper), 0});
        foot[i] = add(pre + "Foot", lower[i], {0, -(hind ? kQuadShin : kQuadFore), 0});
        toes[i] = add(pre + "Toe", foot[i], {0, -(hind ? kQuadCannon : kQuadPastern), 0});
        add({}, toes[i], {0, 0, 4}, false, true);
    }

    constexpr double period = 1.2, stride = 22;
    // A lateral-sequence walk: left hind, left front, right hind, right front.
    constexpr double phase[4] = {0.0, 0.5, 0.25, 0.75};
    // Rest bends: the hind stifle forward and hock back; the front leg
    // nearly straight. Lift bends the middle joint while the leg swings.
    constexpr double restUpper[4] = {-25, -25, 6, 6}, restMid[4] = {50, 50, -4, -4}, lift[4] = {30, 30, 55, 55};
    const double legLen = (kQuadThigh + kQuadShin + kQuadCannon + kQuadUpper + kQuadFore + kQuadPastern) / 2;
    const double speed = 4 * legLen * std::sin(stride * kDeg) / period;
    const int steps = static_cast<int>(std::lround(period * kFps));
    m.frameCount = steps;
    m.frameTime = 1.0 / kFps;
    m.values.assign(uz(m.frameCount) * uz(m.channelCount), 0.0);
    for (int f = 0; f < m.frameCount; ++f) {
        const double t = static_cast<double>(f) / kFps;
        double* v = &m.values[uz(f) * uz(m.channelCount)];
        auto rot = [&](int joint, double x, double y, double z) {
            const int c = m.joints[uz(joint)].firstChannel;
            v[c] = z;
            v[c + 1] = x;
            v[c + 2] = y;
        };
        // How far below its top each leg's toe reaches (to stand the lowest on the floor).
        double reach = 0;
        for (int i = 0; i < 4; ++i) {
            const bool hind = i < 2;
            const double ph = 2 * kPi * (t / period + phase[i]);
            const double a = restUpper[i] - stride * std::sin(ph);
            const double b = restMid[i] + lift[i] * std::max(0.0, std::cos(ph));
            rot(upper[i], a, 0, 0);
            rot(lower[i], b, 0, 0);
            rot(foot[i], -(a + b), 0, 0);   // the cannon or pastern stays upright
            const double l1 = hind ? kQuadThigh : kQuadUpper, l2 = hind ? kQuadShin : kQuadFore, l3 = hind ? kQuadCannon : kQuadPastern;
            const double top = 4;   // every leg starts 4 below the hips (the chest sits 2 above them, its legs 6 below it)
            reach = std::max(reach, top + l1 * std::cos(a * kDeg) + l2 * std::cos((a + b) * kDeg) + l3);
        }
        v[0] = 0;
        v[1] = reach;   // near; set exactly below
        v[2] = speed * t;
        // The back rocks a little with each step, the head nods twice a
        // stride and the tail swings and lifts.
        rot(spine, 3 * std::sin(2 * kPi * 2 * t / period), 2 * std::sin(2 * kPi * t / period), 0);
        rot(neck, 8 * std::sin(2 * kPi * 2 * t / period + 0.8), 0, 0);
        rot(tail, 10 - 8 * std::sin(2 * kPi * 2 * t / period + 1.6), 12 * std::sin(2 * kPi * t / period), 0);
        rot(tail1, 8, 10 * std::sin(2 * kPi * t / period - 0.7), 0);
        rot(chest, 0, -2 * std::sin(2 * kPi * t / period), 0);
    }
    // The lowest toe on the floor in every frame (the spine's sway moves the
    // front legs a little, so the height is measured, not worked out).
    for (int f = 0; f < m.frameCount; ++f) {
        const std::vector<Vec3> w = jointPositions(m, f * m.frameTime);
        double lowest = 1e300;
        for (int i = 0; i < 4; ++i) lowest = std::min(lowest, w[uz(toes[i])].y);
        m.values[uz(f) * uz(m.channelCount) + 1] -= lowest;
    }
    return m;
}

// ---- BVH writing -----------------------------------------------------------------

const char* channelName(Channel c) {
    switch (c) {
    case Channel::Xposition: return "Xposition";
    case Channel::Yposition: return "Yposition";
    case Channel::Zposition: return "Zposition";
    case Channel::Xrotation: return "Xrotation";
    case Channel::Yrotation: return "Yrotation";
    case Channel::Zrotation: return "Zrotation";
    }
    return "Xrotation";
}

std::string num(double v) {
    if (v == 0) return "0";   // also turns -0 into 0
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.9g", v);
    return buf;
}

// A name the BVH reader takes back as one joint name: no braces or control
// characters, no leading/trailing spaces, not empty.
std::string bvhName(const std::string& raw, std::size_t index) {
    // The reader joins a name's words with one space, so runs of spaces collapse.
    std::string s;
    for (char c : raw) {
        const unsigned char u = static_cast<unsigned char>(c);
        const char out = (u < 0x20 || c == '{' || c == '}' || u == 0x7f) ? '_' : c;
        if (out == ' ' && !s.empty() && s.back() == ' ') continue;
        s += out;
    }
    const auto first = s.find_first_not_of(' ');
    if (first == std::string::npos) return "Joint" + std::to_string(index);
    s = s.substr(first, s.find_last_not_of(' ') - first + 1);
    // A first word the reader treats as a keyword would end the name early.
    if (s == "End" || s.rfind("End ", 0) == 0) s = "_" + s;
    return s;
}

bool reservedOnWindows(std::string upper) {
    if (upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL") return true;
    if (upper.size() == 4 && (upper.rfind("COM", 0) == 0 || upper.rfind("LPT", 0) == 0) && upper[3] >= '0' && upper[3] <= '9') return true;
    return false;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

} // namespace

std::vector<PresetInfo> builtInPresets() {
    std::vector<PresetInfo> out;
    for (const Builtin& b : kBuiltins) {
        PresetInfo i;
        i.name = b.name;
        i.description = b.description;
        i.seconds = b.seconds;
        i.loops = b.loops;
        i.builtIn = true;
        i.fourLegged = b.fourLegged;
        out.push_back(std::move(i));
    }
    return out;
}

Result<Motion> builtInPreset(std::string_view name) {
    const Builtin* b = findBuiltin(name);
    if (!b) return Error{ErrorCode::NotFound, "no built-in motion named '" + std::string(name) + "'"};
    return generate(*b);
}

Result<std::string> writeBvh(const Motion& m) {
    if (m.joints.empty() || m.channelCount <= 0 || m.frameCount <= 0)
        return Error{ErrorCode::InvalidArgument, "a motion needs joints, channels and at least one frame to be written as BVH"};
    if (m.values.size() != uz(m.frameCount) * uz(m.channelCount))
        return Error{ErrorCode::InvalidArgument, "the motion's values do not match its frames and channels"};
    if (!(m.frameTime > 0 && m.frameTime <= 10)) return Error{ErrorCode::InvalidArgument, "frame time must be above 0 and at most 10 seconds"};
    int roots = 0;
    for (const Joint& j : m.joints) roots += j.parent < 0 ? 1 : 0;
    if (roots != 1 || m.joints.front().parent >= 0 || m.joints.front().endSite)
        return Error{ErrorCode::InvalidArgument, "BVH holds one skeleton with one root joint; this motion has " + std::to_string(roots)};
    for (double v : m.values)
        if (!std::isfinite(v) || std::abs(v) > 1e7) return Error{ErrorCode::InvalidArgument, "the motion has a value BVH cannot hold"};

    std::vector<std::vector<int>> children(m.joints.size());
    for (std::size_t i = 0; i < m.joints.size(); ++i) {
        const int p = m.joints[i].parent;
        if (p >= static_cast<int>(i)) return Error{ErrorCode::InvalidArgument, "a joint comes before its parent"};
        if (p >= 0) {
            if (m.joints[uz(p)].endSite) return Error{ErrorCode::InvalidArgument, "an end site has a child joint"};
            children[uz(p)].push_back(static_cast<int>(i));
        }
    }
    // Unique, BVH-safe names.
    std::vector<std::string> names(m.joints.size());
    std::set<std::string> used;
    for (std::size_t i = 0; i < m.joints.size(); ++i) {
        if (m.joints[i].endSite) continue;
        std::string n = bvhName(m.joints[i].name, i);
        for (int k = 2; used.count(n); ++k) n = bvhName(m.joints[i].name, i) + "_" + std::to_string(k);
        used.insert(n);
        names[i] = std::move(n);
    }

    std::string s = "HIERARCHY\n";
    // Depth-first, iteratively (deep skeletons do not grow the call stack).
    struct Item {
        int joint;
        bool close;
    };
    std::vector<Item> todo = {{0, false}};
    std::vector<int> depthOf(m.joints.size(), 0);
    std::vector<int> order;   // joints in the order the file lists them; its values follow that order
    while (!todo.empty()) {
        const Item it = todo.back();
        todo.pop_back();
        const Joint& j = m.joints[uz(it.joint)];
        const int d = depthOf[uz(it.joint)];
        const std::string pad(uz(d) * 2, ' ');
        if (it.close) {
            s += pad + "}\n";
            continue;
        }
        if (j.endSite) {
            s += pad + "End Site\n" + pad + "{\n" + pad + "  OFFSET " + num(j.offset.x) + " " + num(j.offset.y) + " " + num(j.offset.z) + "\n" + pad + "}\n";
            continue;
        }
        order.push_back(it.joint);
        s += pad + (j.parent < 0 ? "ROOT " : "JOINT ") + names[uz(it.joint)] + "\n" + pad + "{\n";
        s += pad + "  OFFSET " + num(j.offset.x) + " " + num(j.offset.y) + " " + num(j.offset.z) + "\n";
        s += pad + "  CHANNELS " + std::to_string(j.channels.size());
        for (Channel c : j.channels) s += std::string(" ") + channelName(c);
        s += "\n";
        todo.push_back({it.joint, true});
        const auto& kids = children[uz(it.joint)];
        for (auto k = kids.rbegin(); k != kids.rend(); ++k) {
            depthOf[uz(*k)] = d + 1;
            todo.push_back({*k, false});
        }
    }
    s += "MOTION\nFrames: " + std::to_string(m.frameCount) + "\nFrame Time: " + num(m.frameTime) + "\n";
    for (int f = 0; f < m.frameCount; ++f) {
        const double* v = &m.values[uz(f) * uz(m.channelCount)];
        bool first = true;
        for (int jo : order) {
            const Joint& j = m.joints[uz(jo)];
            for (std::size_t c = 0; c < j.channels.size(); ++c) {
                if (!first) s += ' ';
                first = false;
                s += num(v[uz(j.firstChannel) + c]);
            }
        }
        s += '\n';
    }
    return s;
}

Result<fs::path> defaultPresetDir() {
    auto dir = userDataDir();
    if (!dir) return dir.error();
    return dir.value() / "motions";
}

Status checkPresetName(std::string_view name) {
    if (name.empty() || name.size() > 64) return Error{ErrorCode::InvalidArgument, "a preset name is 1 to 64 characters"};
    for (char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u >= 0x80 || !(std::isalnum(u) || c == ' ' || c == '-' || c == '_'))
            return Error{ErrorCode::InvalidArgument, "a preset name uses letters, digits, spaces, '-' and '_' only"};
    }
    if (name.front() == ' ' || name.back() == ' ') return Error{ErrorCode::InvalidArgument, "a preset name cannot start or end with a space"};
    std::string upper(name);
    for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (reservedOnWindows(upper)) return Error{ErrorCode::InvalidArgument, "'" + std::string(name) + "' is reserved by Windows; choose another name"};
    for (const Builtin& b : kBuiltins)
        if (lower(name) == b.name) return Error{ErrorCode::InvalidArgument, "'" + std::string(name) + "' is a built-in motion; choose another name"};
    return {};
}

Result<std::vector<PresetInfo>> savedPresets(const fs::path& dir) {
    std::vector<PresetInfo> out;
    std::error_code ec;
    if (!fs::exists(dir, ec)) return out;
    if (!fs::is_directory(dir, ec)) return Error{ErrorCode::InvalidArgument, utf8(dir) + " is not a folder"};
    fs::directory_iterator it(dir, ec);
    if (ec) return Error{ErrorCode::IoError, "cannot list " + utf8(dir) + ": " + ec.message()};
    for (const fs::directory_entry& e : it) {
        if (out.size() >= 1000) break;   // a library, not an archive
        if (!e.is_regular_file(ec) || lower(utf8(e.path().extension())) != ".bvh") continue;
        PresetInfo i;
        i.name = utf8(e.path().stem());
        i.file = e.path();
        if (auto st = checkPresetName(i.name); !st) {
            i.error = st.error().message;
        } else if (auto m = loadBvh(e.path()); !m) {
            i.error = m.error().message;
        } else {
            i.seconds = m.value().duration();
            i.description = std::to_string(m.value().joints.size()) + " joints, " + std::to_string(m.value().frameCount) + " frames";
        }
        out.push_back(std::move(i));
    }
    std::sort(out.begin(), out.end(), [](const PresetInfo& a, const PresetInfo& b) { return lower(a.name) < lower(b.name); });
    return out;
}

Status savePreset(const fs::path& dir, std::string_view name, const Motion& m, bool overwrite) {
    if (auto st = checkPresetName(name); !st) return st;
    auto text = writeBvh(m);
    if (!text) return text.error();
    // What is saved must load again.
    if (auto back = parseBvh(text.value()); !back)
        return Error{ErrorCode::InvalidArgument, "this motion cannot be saved as BVH: " + back.error().message};
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return Error{ErrorCode::IoError, "cannot create " + utf8(dir) + ": " + ec.message()};
    const fs::path file = dir / (std::string(name) + ".bvh");
    if (!overwrite && fs::exists(file, ec)) return Error{ErrorCode::InvalidArgument, "a preset named '" + std::string(name) + "' is already saved"};
    const std::string& t = text.value();
    return writeFileAtomic(file, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(t.data()), t.size()));
}

Status removePreset(const fs::path& dir, std::string_view name) {
    if (findBuiltin(lower(name))) return Error{ErrorCode::InvalidArgument, "built-in motions cannot be removed"};
    if (auto st = checkPresetName(name); !st) return st;
    std::error_code ec;
    const fs::path file = dir / (std::string(name) + ".bvh");
    if (!fs::exists(file, ec)) return Error{ErrorCode::NotFound, "no saved preset named '" + std::string(name) + "'"};
    if (!fs::remove(file, ec) || ec) return Error{ErrorCode::IoError, "cannot remove " + utf8(file) + (ec ? ": " + ec.message() : std::string())};
    return {};
}

Result<Motion> loadPreset(std::string_view name, const fs::path& dir) {
    if (const Builtin* b = findBuiltin(name)) return generate(*b);
    // The name check also keeps the lookup inside `dir` (no separators, no "..").
    if (auto st = checkPresetName(name); !st) return Error{ErrorCode::NotFound, "no motion preset named '" + std::string(name) + "'"};
    std::error_code ec;
    const fs::path file = dir / (std::string(name) + ".bvh");
    if (!fs::exists(file, ec)) {
        std::string names;
        for (const Builtin& b : kBuiltins) names += (names.empty() ? "" : ", ") + std::string(b.name);
        return Error{ErrorCode::NotFound, "no motion preset named '" + std::string(name) + "' (built in: " + names + "; saved presets are in " + utf8(dir) + ")"};
    }
    return loadBvh(file);
}

} // namespace acestudio::mocap
