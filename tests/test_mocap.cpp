// BVH import, bone mapping, camera and retargeting onto a sprite (Mocap.h).

#include "acestudio/core/Mocap.h"

#include "acestudio/core/PngCodec.h"

#include "TestSupport.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>

using namespace acestudio;
using namespace acestudio::mocap;
using Catch::Approx;

namespace {

constexpr double kPi = 3.14159265358979323846;

std::filesystem::path fixture(const char* name) { return std::filesystem::path(ACESTUDIO_SOURCE_DIR) / "tests" / "data" / "anim" / name; }

struct Names {
    std::string hips, spine, spine1, neck, head, lsh, larm, lfore, lhand, rsh, rarm, rfore, rhand, lup, lleg, lfoot, ltoe, rup, rleg,
        rfoot, rtoe;
};

Names mixamo() {
    return {"mixamorig:Hips", "mixamorig:Spine", "mixamorig:Spine1", "mixamorig:Neck", "mixamorig:Head",
            "mixamorig:LeftShoulder", "mixamorig:LeftArm", "mixamorig:LeftForeArm", "mixamorig:LeftHand",
            "mixamorig:RightShoulder", "mixamorig:RightArm", "mixamorig:RightForeArm", "mixamorig:RightHand",
            "mixamorig:LeftUpLeg", "mixamorig:LeftLeg", "mixamorig:LeftFoot", "mixamorig:LeftToeBase",
            "mixamorig:RightUpLeg", "mixamorig:RightLeg", "mixamorig:RightFoot", "mixamorig:RightToeBase"};
}

// A walk facing +Z (left is +X), one cycle a second at `fps`: legs swing
// about X, the knee bends in the swing, arms swing against the legs, the
// hips sit so the lower foot is on the floor (y = 0 at the ankles) and move
// forward by about one stride a step.
std::string walkBvh(const Names& n, int frames = 30, int fps = 30, bool travel = true) {
    const double l1 = 42, l2 = 40, A = 0.35;
    std::ostringstream s;
    s << "HIERARCHY\nROOT " << n.hips << "\n{\n OFFSET 0 0 0\n CHANNELS 6 Xposition Yposition Zposition Zrotation Xrotation Yrotation\n";
    auto joint = [&](const std::string& name, double x, double y, double z) {
        s << "JOINT " << name << "\n{\n OFFSET " << x << " " << y << " " << z << "\n CHANNELS 3 Zrotation Xrotation Yrotation\n";
    };
    auto endSite = [&](double x, double y, double z) { s << "End Site\n{\n OFFSET " << x << " " << y << " " << z << "\n}\n"; };
    joint(n.spine, 0, 10, 0);
    joint(n.spine1, 0, 10, 0);
    joint(n.neck, 0, 12, 0);
    joint(n.head, 0, 8, 0);
    endSite(0, 10, 0);
    s << "}\n}\n";
    for (int side = 0; side < 2; ++side) {
        const double sx = side == 0 ? 1 : -1;
        joint(side == 0 ? n.lsh : n.rsh, 4 * sx, 8, 0);
        joint(side == 0 ? n.larm : n.rarm, 4 * sx, 0, 0);
        joint(side == 0 ? n.lfore : n.rfore, 0, -25, 0);
        joint(side == 0 ? n.lhand : n.rhand, 0, -22, 0);
        endSite(0, -8, 0);
        s << "}\n}\n}\n}\n";
    }
    s << "}\n}\n";   // spine1, spine
    for (int side = 0; side < 2; ++side) {
        const double sx = side == 0 ? 1 : -1;
        joint(side == 0 ? n.lup : n.rup, 9 * sx, -5, 0);
        joint(side == 0 ? n.lleg : n.rleg, 0, -l1, 0);
        joint(side == 0 ? n.lfoot : n.rfoot, 0, -l2, 0);
        joint(side == 0 ? n.ltoe : n.rtoe, 0, -6, 12);
        endSite(0, 0, 6);
        s << "}\n}\n}\n}\n";
    }
    s << "}\nMOTION\nFrames: " << frames << "\nFrame Time: " << 1.0 / fps << "\n";
    for (int f = 0; f < frames; ++f) {
        const double t = static_cast<double>(f) / fps;
        double thigh[2], knee[2], vertical[2];
        for (int side = 0; side < 2; ++side) {
            const double ph = 2 * kPi * (t + side * 0.5);
            thigh[side] = -A * std::sin(ph);                       // radians, negative = forward
            knee[side] = 1.1 * std::max(0.0, std::cos(ph));        // bends while the leg swings forward
            vertical[side] = l1 * std::cos(thigh[side]) + l2 * std::cos(thigh[side] + knee[side]);
        }
        const double hipY = std::max(vertical[0], vertical[1]) + 5;
        const double z = travel ? 4 * (l1 + l2) * std::sin(A) * t : 0;
        s << 0 << " " << hipY << " " << z << " 0 0 0";
        s << " 0 0 0 0 0 0 0 0 0 0 0 0";   // spine, spine1, neck, head
        for (int side = 0; side < 2; ++side) {
            // The arm swings against the leg on its side.
            const double swing = thigh[side] * -0.8 * 180 / kPi;
            s << " 0 0 0 " << " 0 " << swing << " 0" << " 0 -15 0" << " 0 0 0";
        }
        for (int side = 0; side < 2; ++side)
            s << " 0 " << thigh[side] * 180 / kPi << " 0 0 " << knee[side] * 180 / kPi << " 0 0 " << -(thigh[side] + knee[side]) * 180 / kPi
              << " 0 0 0 0";
        s << "\n";
    }
    return s.str();
}

int lowestRow(const Image& img) {
    const IntRect r = img.opaqueBounds();
    return r.y + r.h - 1;
}

} // namespace

TEST_CASE("BVH: hierarchy, channels and forward kinematics", "[mocap]") {
    const char* text =
        "HIERARCHY\nROOT Root Bone\n{\n OFFSET 0 0 0\n CHANNELS 6 Xposition Yposition Zposition Zrotation Xrotation Yrotation\n"
        " JOINT Child\n {\n  OFFSET 0 10 0\n  CHANNELS 3 Zrotation Xrotation Yrotation\n  End Site\n  {\n   OFFSET 0 5 0\n  }\n }\n}\n"
        "MOTION\nFrames: 2\nFrame Time: 0.5\n"
        "0 0 0 0 0 0 0 0 0\n"
        "1 2 3 90 0 0 0 0 0\n";
    auto r = parseBvh(text);
    REQUIRE(r);
    const Motion& m = r.value();
    REQUIRE(m.joints.size() == 3);
    CHECK(m.joints[0].name == "Root Bone");
    CHECK(m.joints[2].endSite);
    CHECK(m.joints[2].name == "Child_end");
    CHECK(m.channelCount == 9);
    CHECK(m.frameCount == 2);
    CHECK(m.duration() == Approx(1.0));
    const auto p0 = jointPositions(m, 0);
    CHECK(p0[1].y == Approx(10));
    // Frame 1: the root moved to (1,2,3) and turned 90 degrees about Z: the
    // child's offset (0,10,0) points along -X.
    const auto p1 = jointPositions(m, 0.5);
    CHECK(p1[0].x == Approx(1));
    CHECK(p1[1].x == Approx(1 - 10).margin(1e-9));
    CHECK(p1[1].y == Approx(2).margin(1e-9));
    CHECK(p1[2].x == Approx(1 - 15).margin(1e-9));
    // Between the frames the positions blend.
    CHECK(jointPositions(m, 0.25)[0].z == Approx(1.5));
}

TEST_CASE("BVH: rotations apply in the order the channels are listed", "[mocap]") {
    auto make = [](const char* order) {
        return std::string("HIERARCHY\nROOT A\n{\n OFFSET 0 0 0\n CHANNELS 2 ") + order +
               "\n JOINT B\n {\n  OFFSET 0 0 10\n  CHANNELS 0\n }\n}\nMOTION\nFrames: 1\nFrame Time: 0.1\n90 90\n";
    };
    auto xy = parseBvh(make("Xrotation Yrotation"));
    auto yx = parseBvh(make("Yrotation Xrotation"));
    REQUIRE(xy);
    REQUIRE(yx);
    const Vec3 a = jointPositions(xy.value(), 0)[1], b = jointPositions(yx.value(), 0)[1];
    // Rx(90) Ry(90) (0,0,10) = (10,0,0); Ry(90) Rx(90) (0,0,10) = (0,-10,0).
    CHECK(a.x == Approx(10).margin(1e-9));
    CHECK(b.y == Approx(-10).margin(1e-9));
}

TEST_CASE("BVH: malformed files are refused with the line, never crash", "[mocap][security]") {
    const std::string good = walkBvh(mixamo(), 4);
    REQUIRE(parseBvh(good));
    CHECK_FALSE(parseBvh(""));
    CHECK_FALSE(parseBvh("HIERARCHY\nROOT A\n{\n OFFSET 0 0\n"));
    CHECK_FALSE(parseBvh(good.substr(0, good.size() - 20)));                                  // values missing
    CHECK_FALSE(parseBvh(good + " 1"));                                                       // one value too many
    std::string scale = good;
    scale.replace(scale.find("Zrotation Xrotation Yrotation"), 9, "Xscale   ");
    auto bad = parseBvh(scale);
    REQUIRE_FALSE(bad);
    CHECK(bad.error().message.find("Xscale") != std::string::npos);
    std::string nan = good;
    nan.replace(nan.rfind("0 0 0"), 1, "nan");
    CHECK_FALSE(parseBvh(nan));
    CHECK_FALSE(parseBvh("HIERARCHY\nROOT A\n{\n OFFSET 0 0 0\n CHANNELS 1 Xrotation\n}\nMOTION\nFrames: -3\nFrame Time: 0.1\n"));
    CHECK_FALSE(parseBvh("HIERARCHY\nROOT A\n{\n OFFSET 0 0 0\n CHANNELS 1 Xrotation\n}\nMOTION\nFrames: 1\nFrame Time: 0\n5\n"));
    BvhLimits tight;
    tight.maxDepth = 3;
    CHECK_FALSE(parseBvh(good, tight));
    tight = {};
    tight.maxJoints = 5;
    CHECK_FALSE(parseBvh(good, tight));
    tight = {};
    tight.maxBytes = 100;
    CHECK_FALSE(parseBvh(good, tight));
}

TEST_CASE("Bones are found by name in the common naming schemes", "[mocap]") {
    auto check = [](const Names& n) {
        auto m = parseBvh(walkBvh(n, 2));
        REQUIRE(m);
        const BoneMap map = autoMap(m.value());
        INFO(n.lup);
        CHECK(map.hasLegs());
        CHECK(m.value().joints[static_cast<std::size_t>(map[Bone::LThigh])].name == n.lup);
        CHECK(m.value().joints[static_cast<std::size_t>(map[Bone::RShin])].name == n.rleg);
        CHECK(m.value().joints[static_cast<std::size_t>(map[Bone::LUpperArm])].name == n.larm);
        CHECK(m.value().joints[static_cast<std::size_t>(map[Bone::RHand])].name == n.rhand);
        CHECK(m.value().joints[static_cast<std::size_t>(map[Bone::Head])].name == n.head);
        CHECK(m.value().joints[static_cast<std::size_t>(map[Bone::LToe])].name == n.ltoe);
    };
    check(mixamo());
    check({"Bip01 Pelvis", "Bip01 Spine", "Bip01 Spine1", "Bip01 Neck", "Bip01 Head", "Bip01 L Clavicle", "Bip01 L UpperArm",
           "Bip01 L Forearm", "Bip01 L Hand", "Bip01 R Clavicle", "Bip01 R UpperArm", "Bip01 R Forearm", "Bip01 R Hand",
           "Bip01 L Thigh", "Bip01 L Calf", "Bip01 L Foot", "Bip01 L Toe0", "Bip01 R Thigh", "Bip01 R Calf", "Bip01 R Foot",
           "Bip01 R Toe0"});
    check({"hip", "abdomen", "chest", "neck", "head", "lCollar", "lShldr", "lForeArm", "lHand", "rCollar", "rShldr", "rForeArm",
           "rHand", "lThigh", "lShin", "lFoot", "lToe", "rThigh", "rShin", "rFoot", "rToe"});
    check({"pelvis", "spine_01", "spine_02", "neck_01", "head", "clavicle_l", "upperarm_l", "lowerarm_l", "hand_l", "clavicle_r",
           "upperarm_r", "lowerarm_r", "hand_r", "thigh_l", "calf_l", "foot_l", "ball_l", "thigh_r", "calf_r", "foot_r", "ball_r"});
}

TEST_CASE("Bones are found by shape when the names say nothing", "[mocap]") {
    Names n;
    std::string* all[] = {&n.hips, &n.spine, &n.spine1, &n.neck, &n.head, &n.lsh, &n.larm, &n.lfore, &n.lhand, &n.rsh, &n.rarm,
                          &n.rfore, &n.rhand, &n.lup, &n.lleg, &n.lfoot, &n.ltoe, &n.rup, &n.rleg, &n.rfoot, &n.rtoe};
    int i = 0;
    for (std::string* s : all) *s = "j" + std::to_string(i++);
    auto m = parseBvh(walkBvh(n, 2));
    REQUIRE(m);
    const BoneMap map = autoMap(m.value());
    CHECK(map.hasLegs());
    CHECK(m.value().joints[static_cast<std::size_t>(map[Bone::LThigh])].name == n.lup);
    CHECK(m.value().joints[static_cast<std::size_t>(map[Bone::RFoot])].name == n.rfoot);
    CHECK(m.value().joints[static_cast<std::size_t>(map[Bone::Head])].name == n.head);
    CHECK(m.value().joints[static_cast<std::size_t>(map[Bone::LUpperArm])].name == n.larm);
    CHECK(m.value().joints[static_cast<std::size_t>(map[Bone::RHand])].name == n.rhand);
    // An override wins.
    const BoneMap o = autoMap(m.value(), {{Bone::Head, n.neck}});
    CHECK(m.value().joints[static_cast<std::size_t>(o[Bone::Head])].name == n.neck);
}

TEST_CASE("The camera turns about the character", "[mocap]") {
    auto m = parseBvh(walkBvh(mixamo(), 2));
    REQUIRE(m);
    const BoneMap map = autoMap(m.value());
    const Vec3 fwd = characterForward(m.value(), map);
    CHECK(fwd.z == Approx(1).margin(1e-6));
    const std::vector<Vec3> pts{{0, 0, 0}, {0, 0, 1}, {1, 0, 0}, {0, 1, 0}};
    // Right: the character faces screen right.
    auto right = project(pts, *cameraPreset("right"), fwd, 1, {});
    CHECK(right.x[1] == Approx(1).margin(1e-9));
    CHECK(right.y[3] == Approx(-1).margin(1e-9));   // up is up
    auto left = project(pts, *cameraPreset("left"), fwd, 1, {});
    CHECK(left.x[1] == Approx(-1).margin(1e-9));
    // Front: it faces the viewer, its left (+X) on screen right, nearer the camera when forward.
    auto front = project(pts, *cameraPreset("front"), fwd, 1, {});
    CHECK(front.x[2] == Approx(1).margin(1e-9));
    CHECK(front.depth[1] == Approx(1).margin(1e-9));
    // Top-down from the front: forward is down the screen.
    auto top = project(pts, *cameraPreset("top-down"), fwd, 1, {});
    CHECK(top.y[1] == Approx(1).margin(1e-9));
    // Roll turns the picture clockwise: up becomes right.
    Camera c = *cameraPreset("right");
    c.roll = 90;
    auto rolled = project(pts, c, fwd, 1, {});
    CHECK(rolled.x[3] == Approx(1).margin(1e-9));
    // Perspective: nearer is bigger.
    Camera p = *cameraPreset("front");
    p.perspective = true;
    p.distance = 3;
    auto persp = project({{1, 0, 1}, {1, 0, -1}}, p, fwd, 1, {});
    CHECK(persp.x[0] > persp.x[1]);
    for (const std::string& name : cameraPresetNames()) CHECK(cameraPreset(name));
    CHECK_FALSE(cameraPreset("sideways"));
}

namespace {

// A walk retargeted onto rogue64 (side view): 12 frames, one size, feet on
// the ground within the foot stagger, moving, and a clean clip check.
void checkWalkRetarget(const Motion& m) {
    auto sprite = readPng(fixture("rogue64.png"));
    REQUIRE(sprite);
    RetargetOptions o;
    o.fps = 12;
    auto r = retarget(sprite.value(), m, o);
    REQUIRE(r);
    const RetargetResult& res = r.value();
    for (const auto& n : res.notes) UNSCOPED_INFO(n);
    CHECK_FALSE(res.approximateView);
    REQUIRE(res.clip.frames.size() == 12);
    REQUIRE(res.source.frames.size() == 12);
    // One frame size, and the feet stay on one ground line.
    int ground = 0;
    for (const Image& f : res.clip.frames) ground = std::max(ground, lowestRow(f));
    std::set<std::string> distinct;
    for (std::size_t i = 0; i < res.clip.frames.size(); ++i) {
        INFO("frame " << i);
        const Image& f = res.clip.frames[i];
        CHECK(f.width() == res.clip.frames[0].width());
        CHECK(f.height() == res.clip.frames[0].height());
        // Each foot stands on its own row (the far foot of a three-quarter
        // view a little higher): the lowest row stays within that stagger.
        CHECK(lowestRow(f) <= ground);
        CHECK(lowestRow(f) >= ground - res.clip.footStagger);
        CHECK_FALSE(res.source.frames[i].isFullyTransparent());
        distinct.insert(std::string(reinterpret_cast<const char*>(f.rawBytes()), f.byteSize()));
    }
    // It moves: most frames differ.
    CHECK(distinct.size() >= 10);
    const motion::ClipCheck chk = motion::checkClip(res.clip);
    for (const auto& i : chk.issues) UNSCOPED_INFO(i);
    CHECK(chk.issues.empty());
    CHECK(res.clip.depthPops == 0);
}

} // namespace

TEST_CASE("A 3D walk retargets onto a side-view sprite", "[mocap][anim]") {
    auto m = parseBvh(walkBvh(mixamo(), 30));
    REQUIRE(m);
    checkWalkRetarget(m.value());
}

TEST_CASE("A camera far from the drawn view is reported as an approximation", "[mocap][anim]") {
    auto sprite = readPng(fixture("rogue64.png"));
    REQUIRE(sprite);
    auto m = parseBvh(walkBvh(mixamo(), 12, 12, false));
    REQUIRE(m);
    RetargetOptions o;
    o.camera = cameraPreset("front");
    auto r = retarget(sprite.value(), m.value(), o);
    REQUIRE(r);
    CHECK(r.value().approximateView);
    CHECK_FALSE(r.value().notes.empty());
    // Bodies no motion drives (a blob) and skeletons without legs are refused, not guessed.
    o.body = motion::Body::Blob;
    CHECK_FALSE(retarget(sprite.value(), m.value(), o));
    auto armless = parseBvh("HIERARCHY\nROOT A\n{\n OFFSET 0 0 0\n CHANNELS 1 Xrotation\n}\nMOTION\nFrames: 1\nFrame Time: 0.1\n5\n");
    REQUIRE(armless);
    CHECK_FALSE(retarget(sprite.value(), armless.value(), RetargetOptions{}));
}

// ---- glTF / GLB ------------------------------------------------------------------
//
// Files are assembled in memory here (JSON plus a binary chunk), and expected
// world positions are computed independently with glTF's own matrix math:
// world = parent * T * R * S, column-major 4x4.

namespace {

using json = nlohmann::json;

struct Q {
    double x = 0, y = 0, z = 0, w = 1;
};

Q qmul(Q a, Q b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

Q axisAngle(double ax, double ay, double az, double deg) {
    const double l = std::sqrt(ax * ax + ay * ay + az * az);
    const double h = deg * kPi / 360, s = std::sin(h);
    return {ax / l * s, ay / l * s, az / l * s, std::cos(h)};
}

using M4 = std::array<double, 16>;   // column-major

M4 mul4(const M4& a, const M4& b) {
    M4 r{};
    for (std::size_t c = 0; c < 4; ++c)
        for (std::size_t row = 0; row < 4; ++row) {
            double s = 0;
            for (std::size_t k = 0; k < 4; ++k) s += a[k * 4 + row] * b[c * 4 + k];
            r[c * 4 + row] = s;
        }
    return r;
}

M4 trs(const std::array<double, 3>& t, Q q, const std::array<double, 3>& s) {
    const double n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    q = {q.x / n, q.y / n, q.z / n, q.w / n};
    const double R[3][3] = {{1 - 2 * (q.y * q.y + q.z * q.z), 2 * (q.x * q.y - q.z * q.w), 2 * (q.x * q.z + q.y * q.w)},
                            {2 * (q.x * q.y + q.z * q.w), 1 - 2 * (q.x * q.x + q.z * q.z), 2 * (q.y * q.z - q.x * q.w)},
                            {2 * (q.x * q.z - q.y * q.w), 2 * (q.y * q.z + q.x * q.w), 1 - 2 * (q.x * q.x + q.y * q.y)}};
    M4 m{};
    for (std::size_t c = 0; c < 3; ++c)
        for (std::size_t r = 0; r < 3; ++r) m[c * 4 + r] = R[r][c] * s[c];
    m[12] = t[0];
    m[13] = t[1];
    m[14] = t[2];
    m[15] = 1;
    return m;
}

struct TNode {
    std::string name;
    int parent = -1;
    std::array<double, 3> t{0, 0, 0};
    std::array<double, 3> s{1, 1, 1};
    Q r;
};

struct TTrack {
    int node = 0;
    std::string path, interp;
    std::vector<float> times, values;
};

struct Scene {
    std::vector<TNode> nodes;   // parents before children
    std::vector<TTrack> tracks;
    std::vector<int> skin;
};

// The track's value at t, sampled as the glTF spec says.
std::vector<double> sampleTrack(const TTrack& tr, double t) {
    const std::size_t n = tr.times.size(), c = tr.path == "rotation" ? 4 : 3;
    const bool cubic = tr.interp == "CUBICSPLINE";
    auto val = [&](std::size_t k, std::size_t element) {   // element: 0 in, 1 value, 2 out
        std::vector<double> v(c);
        for (std::size_t i = 0; i < c; ++i) v[i] = static_cast<double>(tr.values[(cubic ? 3 * k + element : k) * c + i]);
        return v;
    };
    if (t <= static_cast<double>(tr.times[0])) return val(0, 1);
    if (t >= static_cast<double>(tr.times[n - 1])) return val(n - 1, 1);
    std::size_t k = 0;
    while (static_cast<double>(tr.times[k + 1]) <= t) ++k;
    const double ta = static_cast<double>(tr.times[k]), tb = static_cast<double>(tr.times[k + 1]);
    const double u = (t - ta) / (tb - ta);
    if (tr.interp == "STEP") return val(k, 1);
    std::vector<double> out(c);
    if (tr.interp == "LINEAR") {
        std::vector<double> a = val(k, 1), b = val(k + 1, 1);
        if (c == 4) {
            auto unit = [](std::vector<double>& q) {
                const double l = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
                for (double& x : q) x /= l;
            };
            unit(a);
            unit(b);
            double d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
            if (d < 0) {
                for (double& x : b) x = -x;
                d = -d;
            }
            const double th = std::acos(std::min(1.0, d));
            for (std::size_t i = 0; i < 4; ++i)
                out[i] = th < 1e-9 ? a[i] : (std::sin((1 - u) * th) * a[i] + std::sin(u * th) * b[i]) / std::sin(th);
        } else {
            for (std::size_t i = 0; i < c; ++i) out[i] = a[i] * (1 - u) + b[i] * u;
        }
        return out;
    }
    // CUBICSPLINE (glTF 2.0 spec, Appendix C).
    const std::vector<double> v0 = val(k, 1), b0 = val(k, 2), a1 = val(k + 1, 0), v1 = val(k + 1, 1);
    const double dt = tb - ta;
    for (std::size_t i = 0; i < c; ++i)
        out[i] = (2 * u * u * u - 3 * u * u + 1) * v0[i] + (u * u * u - 2 * u * u + u) * dt * b0[i] + (-2 * u * u * u + 3 * u * u) * v1[i] +
                 (u * u * u - u * u) * dt * a1[i];
    return out;
}

// World positions of every node at t (nullopt = the rest pose).
std::vector<Vec3> expectedWorld(const Scene& sc, std::optional<double> t) {
    std::vector<M4> world(sc.nodes.size());
    std::vector<Vec3> out;
    for (std::size_t i = 0; i < sc.nodes.size(); ++i) {
        TNode n = sc.nodes[i];
        if (t)
            for (const TTrack& tr : sc.tracks) {
                if (tr.node != static_cast<int>(i)) continue;
                const std::vector<double> v = sampleTrack(tr, *t);
                if (tr.path == "translation") n.t = {v[0], v[1], v[2]};
                if (tr.path == "scale") n.s = {v[0], v[1], v[2]};
                if (tr.path == "rotation") n.r = {v[0], v[1], v[2], v[3]};
            }
        const M4 local = trs(n.t, n.r, n.s);
        world[i] = n.parent < 0 ? local : mul4(world[static_cast<std::size_t>(n.parent)], local);
        out.push_back({world[i][12], world[i][13], world[i][14]});
    }
    return out;
}

class GltfBuilder {
public:
    json doc = {{"asset", {{"version", "2.0"}, {"generator", "AceStudio tests"}}},
                {"nodes", json::array()},
                {"accessors", json::array()},
                {"bufferViews", json::array()}};
    std::vector<std::uint8_t> bin;

    int accessor(const std::vector<float>& v, const std::string& type) {
        while (bin.size() % 4 != 0) bin.push_back(0);
        const std::size_t off = bin.size();
        bin.resize(off + v.size() * 4);
        std::memcpy(bin.data() + off, v.data(), v.size() * 4);
        doc["bufferViews"].push_back({{"buffer", 0}, {"byteOffset", off}, {"byteLength", v.size() * 4}});
        const std::size_t comps = type == "SCALAR" ? 1 : type == "VEC3" ? 3 : 4;
        doc["accessors"].push_back(
            {{"bufferView", doc["bufferViews"].size() - 1}, {"componentType", 5126}, {"type", type}, {"count", v.size() / comps}});
        return static_cast<int>(doc["accessors"].size() - 1);
    }
    void channel(std::size_t anim, const TTrack& tr) {
        json& a = doc["animations"][anim];
        const int in = accessor(tr.times, "SCALAR"), out = accessor(tr.values, tr.path == "rotation" ? "VEC4" : "VEC3");
        a["samplers"].push_back({{"input", in}, {"output", out}, {"interpolation", tr.interp}});
        a["channels"].push_back({{"sampler", a["samplers"].size() - 1}, {"target", {{"node", tr.node}, {"path", tr.path}}}});
    }
};

std::string base64(const std::vector<std::uint8_t>& b) {
    static const char* abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < b.size(); i += 3) {
        const std::uint32_t v = (static_cast<std::uint32_t>(b[i]) << 16) | (i + 1 < b.size() ? static_cast<std::uint32_t>(b[i + 1]) << 8 : 0u) |
                                (i + 2 < b.size() ? static_cast<std::uint32_t>(b[i + 2]) : 0u);
        out += abc[(v >> 18) & 63];
        out += abc[(v >> 12) & 63];
        out += i + 1 < b.size() ? abc[(v >> 6) & 63] : '=';
        out += i + 2 < b.size() ? abc[v & 63] : '=';
    }
    return out;
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
}

// A GLB of `doc` (its buffer 0 becomes the BIN chunk).
std::vector<std::uint8_t> glbOf(json doc, std::vector<std::uint8_t> bin) {
    doc["buffers"] = json::array({{{"byteLength", bin.size()}}});
    std::string text = doc.dump();
    while (text.size() % 4 != 0) text += ' ';
    while (bin.size() % 4 != 0) bin.push_back(0);
    std::vector<std::uint8_t> out;
    put32(out, 0x46546C67u);
    put32(out, 2);
    put32(out, static_cast<std::uint32_t>(12 + 8 + text.size() + 8 + bin.size()));
    put32(out, static_cast<std::uint32_t>(text.size()));
    put32(out, 0x4E4F534Au);
    out.insert(out.end(), text.begin(), text.end());
    put32(out, static_cast<std::uint32_t>(bin.size()));
    put32(out, 0x004E4942u);
    out.insert(out.end(), bin.begin(), bin.end());
    return out;
}

// A .gltf (JSON text) whose buffer 0 is at `uri`.
std::vector<std::uint8_t> gltfOf(json doc, std::size_t binSize, const std::string& uri) {
    doc["buffers"] = json::array({{{"byteLength", binSize}, {"uri", uri}}});
    const std::string text = doc.dump(1);
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

GltfBuilder build(const Scene& sc, bool idle = true) {
    GltfBuilder b;
    for (std::size_t i = 0; i < sc.nodes.size(); ++i) {
        const TNode& n = sc.nodes[i];
        json o = {{"name", n.name}, {"translation", n.t}, {"rotation", {n.r.x, n.r.y, n.r.z, n.r.w}}, {"scale", n.s}};
        json children = json::array();
        for (std::size_t c = 0; c < sc.nodes.size(); ++c)
            if (sc.nodes[c].parent == static_cast<int>(i)) children.push_back(c);
        if (!children.empty()) o["children"] = children;
        b.doc["nodes"].push_back(o);
    }
    if (!sc.skin.empty()) b.doc["skins"] = json::array({{{"joints", sc.skin}}});
    for (const TTrack& t : sc.tracks) b.channel(0, t);
    b.doc["animations"][0]["name"] = "Walk";
    if (idle) {
        // A second, two-second animation (only the hips turn).
        TTrack turn{1, "rotation", "LINEAR", {0, 2}, {}};
        for (const Q q : {Q{}, axisAngle(0, 0, 1, 90)})
            for (double v : {q.x, q.y, q.z, q.w}) turn.values.push_back(static_cast<float>(v));
        b.channel(1, turn);
        b.doc["animations"][1]["name"] = "Idle";
    }
    return b;
}

void pushQ(std::vector<float>& v, Q q) {
    for (double d : {q.x, q.y, q.z, q.w}) v.push_back(static_cast<float>(d));
}

// A Z-up chain under an Armature turned -90 degrees about X (Z-up to Y-up)
// and scaled to metres, as Blender and FBX conversions write it. The hips
// translate (LINEAR), the spine turns (LINEAR), the head snaps (STEP) and the
// thigh swings on a Hermite curve (CUBICSPLINE). A mesh node sits outside.
Scene chainScene() {
    Scene sc;
    sc.nodes = {
        {"Armature", -1, {0.5, 0, 0.2}, {0.01, 0.01, 0.01}, axisAngle(1, 0, 0, -90)},
        {"Hips", 0, {0, 0, 100}, {1, 1, 1}, axisAngle(0, 0, 1, 20)},
        {"Spine", 1, {0, 0, 10}, {1, 1, 1}, {}},
        {"Head", 2, {0, 0, 8}, {1, 1, 1}, {}},
        {"LeftUpLeg", 1, {9, 0, -5}, {1, 1, 1}, {}},
        {"LeftLeg", 4, {0, 0, -42}, {1, 1, 1}, axisAngle(1, 0, 0, 10)},
        {"Body", -1, {0, 0, 0}, {1, 1, 1}, {}},
    };
    sc.skin = {4, 5, 1, 2, 3};   // any order: the importer orders parents first
    sc.tracks.push_back({1, "translation", "LINEAR", {0, 0.5f, 1}, {0, 0, 100, 10, 5, 95, 20, 0, 100}});
    TTrack spine{2, "rotation", "LINEAR", {0, 1}, {}};
    pushQ(spine.values, Q{});
    pushQ(spine.values, axisAngle(1, 1, 0, 80));
    sc.tracks.push_back(spine);
    TTrack head{3, "rotation", "STEP", {0, 0.4f, 0.8f}, {}};
    pushQ(head.values, Q{});
    pushQ(head.values, axisAngle(0, 0, 1, 30));
    pushQ(head.values, axisAngle(1, 0, 0, -45));
    sc.tracks.push_back(head);
    TTrack thigh{4, "rotation", "CUBICSPLINE", {0, 0.6f, 1}, {}};
    const Q keys[3] = {Q{}, axisAngle(1, 0, 0, 40), axisAngle(1, 0.2, 0, -20)};
    const Q tangents[3] = {{0.3, 0, 0, 0}, {0.1, 0.05, 0, -0.02}, {-0.2, 0, 0.1, 0}};
    for (int k = 0; k < 3; ++k) {
        pushQ(thigh.values, tangents[k]);   // in
        pushQ(thigh.values, keys[k]);
        pushQ(thigh.values, {-tangents[k].x, tangents[k].y, tangents[k].z, tangents[k].w});   // out
    }
    sc.tracks.push_back(thigh);
    return sc;
}

void checkAgainstScene(const Motion& m, const Scene& sc, double margin) {
    std::vector<std::size_t> nodeOf;
    for (const Joint& j : m.joints) {
        std::size_t n = 0;
        while (n < sc.nodes.size() && sc.nodes[n].name != j.name) ++n;
        REQUIRE(n < sc.nodes.size());
        nodeOf.push_back(n);
    }
    const std::vector<Vec3> rest = restPositions(m), restExp = expectedWorld(sc, std::nullopt);
    for (std::size_t i = 0; i < m.joints.size(); ++i) {
        INFO("rest, joint " << m.joints[i].name);
        CHECK(rest[i].x == Approx(restExp[nodeOf[i]].x).margin(margin));
        CHECK(rest[i].y == Approx(restExp[nodeOf[i]].y).margin(margin));
        CHECK(rest[i].z == Approx(restExp[nodeOf[i]].z).margin(margin));
    }
    for (int f : {0, 1, 5, 11, 12, 13, 15, 17, 18, 23, 24, 25, 29, 30}) {
        if (f >= m.frameCount) continue;
        const double t = f * m.frameTime;
        const std::vector<Vec3> got = jointPositions(m, t), exp = expectedWorld(sc, t);
        for (std::size_t i = 0; i < m.joints.size(); ++i) {
            INFO("frame " << f << ", joint " << m.joints[i].name);
            CHECK(got[i].x == Approx(exp[nodeOf[i]].x).margin(margin));
            CHECK(got[i].y == Approx(exp[nodeOf[i]].y).margin(margin));
            CHECK(got[i].z == Approx(exp[nodeOf[i]].z).margin(margin));
        }
    }
}

// Error text of a refused file ("" if it was accepted).
std::string refusal(const std::vector<std::uint8_t>& bytes, const std::filesystem::path& dir = {}) {
    auto r = parseGltf(bytes, dir);
    return r ? std::string() : r.error().message;
}

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

void writeBytes(const std::filesystem::path& p, const std::vector<std::uint8_t>& b) {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

} // namespace

TEST_CASE("glTF: a skinned chain reproduces glTF world positions", "[mocap][gltf]") {
    const Scene sc = chainScene();
    const GltfBuilder b = build(sc);
    auto r = parseGltf(glbOf(b.doc, b.bin), {});
    REQUIRE(r);
    const Motion& m = r.value();
    // The skin's joints, parents first; the Armature and mesh node are not joints.
    REQUIRE(m.joints.size() == 5);
    const char* names[] = {"Hips", "Spine", "Head", "LeftUpLeg", "LeftLeg"};
    const int parents[] = {-1, 0, 1, 0, 3};
    for (std::size_t i = 0; i < 5; ++i) {
        CHECK(m.joints[i].name == names[i]);
        CHECK(m.joints[i].parent == parents[i]);
    }
    // Root: 3 positions + 3 rotations; the others 3 rotations.
    CHECK(m.joints[0].channels.size() == 6);
    for (std::size_t i = 1; i < 5; ++i) CHECK(m.joints[i].channels.size() == 3);
    CHECK(m.channelCount == 18);
    // One second at 30 fps.
    CHECK(m.frameCount == 31);
    CHECK(m.frameTime == Approx(1.0 / 30));
    checkAgainstScene(m, sc, 1e-9);
    // Up stays up: the Armature's turn makes the chain's +Z world +Y.
    for (double t : {0.0, 0.5, 1.0}) {
        const std::vector<Vec3> p = jointPositions(m, t);
        CHECK(p[2].y > p[0].y + 0.1);   // head above hips
        CHECK(p[4].y < p[0].y - 0.3);   // knee below hips
    }
}

TEST_CASE("glTF: non-uniform and animated scale still give exact positions", "[mocap][gltf]") {
    Scene sc = chainScene();
    sc.nodes[0].s = {0.01, 0.02, 0.015};
    sc.tracks.push_back({2, "scale", "LINEAR", {0, 1}, {1, 1, 1, 1.5f, 0.5f, 2}});
    sc.tracks.push_back({5, "translation", "STEP", {0, 0.5f}, {0, 0, -42, 0, 3, -40}});
    const GltfBuilder b = build(sc, false);
    auto r = parseGltf(glbOf(b.doc, b.bin), {});
    REQUIRE(r);
    // Joints moved by scale or translation get position channels.
    CHECK(r.value().joints[2].channels.size() == 6);   // Head, under the scaled spine
    CHECK(r.value().joints[4].channels.size() == 6);   // LeftLeg, translated
    checkAgainstScene(r.value(), sc, 1e-9);
}

TEST_CASE("glTF: .gltf with a data: URI or a file beside it reads like the GLB", "[mocap][gltf]") {
    const Scene sc = chainScene();
    const GltfBuilder b = build(sc);
    auto glb = parseGltf(glbOf(b.doc, b.bin), {});
    REQUIRE(glb);
    auto same = [&](const Motion& m) {
        REQUIRE(m.joints.size() == glb.value().joints.size());
        for (std::size_t i = 0; i < m.joints.size(); ++i) CHECK(m.joints[i].name == glb.value().joints[i].name);
        CHECK(m.values == glb.value().values);
    };
    auto data = parseGltf(gltfOf(b.doc, b.bin.size(), "data:application/octet-stream;base64," + base64(b.bin)), {});
    REQUIRE(data);
    same(data.value());

    TempDir dir;
    writeBytes(dir / "walk data.bin", b.bin);
    writeBytes(dir / "walk.gltf", gltfOf(b.doc, b.bin.size(), "walk%20data.bin"));
    writeBytes(dir / "walk.glb", glbOf(b.doc, b.bin));
    auto file = loadMotion(dir / "walk.gltf");
    REQUIRE(file);
    same(file.value());
    auto glbFile = loadMotion(dir / "walk.glb");
    REQUIRE(glbFile);
    same(glbFile.value());

    // Animations by index or name.
    auto names = motionAnimationNames(dir / "walk.glb");
    REQUIRE(names);
    CHECK((names.value() == std::vector<std::string>{"Walk", "Idle"}));
    GltfOptions o;
    o.animation = "Idle";
    auto idle = loadMotion(dir / "walk.glb", o);
    REQUIRE(idle);
    CHECK(idle.value().frameCount == 61);   // two seconds at 30 fps
    o.animation = "1";
    auto byIndex = loadMotion(dir / "walk.glb", o);
    REQUIRE(byIndex);
    CHECK(byIndex.value().values == idle.value().values);
    o = {};
    o.animationIndex = 1;
    o.fps = 10;
    auto slow = loadMotion(dir / "walk.glb", o);
    REQUIRE(slow);
    CHECK(slow.value().frameCount == 21);
    o = {};
    o.animation = "Run";
    auto missing = loadMotion(dir / "walk.glb", o);
    REQUIRE_FALSE(missing);
    CHECK(contains(missing.error().message, "Walk"));   // lists what there is
    CHECK_FALSE(loadMotion(dir / "walk.fbx"));
    auto bvhNames = motionAnimationNames(dir / "x.bvh");
    REQUIRE(bvhNames);
    CHECK(bvhNames.value().empty());
}

TEST_CASE("glTF: malformed and unsafe files are refused clearly, never crash", "[mocap][gltf][security]") {
    const Scene sc = chainScene();
    const GltfBuilder b = build(sc);
    const std::vector<std::uint8_t> good = glbOf(b.doc, b.bin);
    REQUIRE(refusal(good).empty());

    // Container.
    std::vector<std::uint8_t> g = good;
    g.resize(g.size() - 10);
    CHECK(contains(refusal(g), "truncated"));
    g = good;
    g.resize(10);
    CHECK_FALSE(refusal(g).empty());
    g = good;
    g[0] = 'x';   // not GLB, and not JSON either
    CHECK_FALSE(refusal(g).empty());
    g = good;
    g[4] = 1;
    CHECK(contains(refusal(g), "version 1"));
    g = good;
    g[12] = 0xF0;
    g[13] = g[14] = g[15] = 0xFF;   // JSON chunk length past the end
    CHECK(contains(refusal(g), "chunk"));
    g = good;
    g[16] = 'X';   // first chunk not JSON
    CHECK(contains(refusal(g), "JSON"));
    CHECK_FALSE(refusal({}).empty());

    auto mutated = [&](auto edit) {
        json d = b.doc;
        edit(d);
        return refusal(glbOf(d, b.bin));
    };
    // Accessors: the first channel's input is accessor 0.
    std::string e = mutated([](json& d) { d["accessors"][0]["byteOffset"] = 4000; });
    CHECK(contains(e, "accessor 0"));
    CHECK(contains(e, "past the end"));
    e = mutated([](json& d) { d["accessors"][0]["count"] = 1000000; });
    CHECK(contains(e, "accessor 0"));
    e = mutated([](json& d) { d["accessors"][0]["count"] = -1; });
    CHECK(contains(e, "count"));
    e = mutated([](json& d) { d["bufferViews"][0]["byteOffset"] = 1u << 30; });
    CHECK(contains(e, "outside buffer"));
    e = mutated([](json& d) { d["accessors"][1]["componentType"] = 5130; });
    CHECK(contains(e, "componentType"));
    CHECK(contains(e, "accessor 1"));
    e = mutated([](json& d) { d["accessors"][0]["componentType"] = 5121; });   // times as bytes
    CHECK(contains(e, "not allowed"));
    e = mutated([](json& d) { d["accessors"][0]["type"] = "VEC2"; });
    CHECK(contains(e, "SCALAR"));
    e = mutated([](json& d) { d["accessors"][0]["sparse"] = {{"count", 1}}; });
    CHECK(contains(e, "sparse"));
    e = mutated([](json& d) { d["accessors"][0]["normalized"] = "yes"; });
    CHECK(contains(e, "normalized"));
    e = mutated([](json& d) { d["bufferViews"][0]["byteStride"] = 2; });
    CHECK(contains(e, "byteStride"));
    e = mutated([](json& d) { d["accessors"][0]["bufferView"] = 999; });
    CHECK(contains(e, "bufferView 999"));
    // A buffer that declares more than it holds, or more than the limit.
    const std::string dataUri = "data:application/octet-stream;base64," + base64(b.bin);
    CHECK(contains(refusal(gltfOf(b.doc, b.bin.size() + 4, dataUri)), "declares"));
    CHECK(contains(refusal(gltfOf(b.doc, std::size_t{1} << 30, dataUri)), "buffer 0"));
    // Animation data.
    e = mutated([](json& d) { d["animations"][0]["channels"][0]["target"]["node"] = 99; });
    CHECK(contains(e, "channel 0"));
    e = mutated([](json& d) { d["animations"][0]["samplers"][0]["interpolation"] = "BEZIER"; });
    CHECK(contains(e, "BEZIER"));
    e = mutated([](json& d) { d["animations"][0]["samplers"][0]["output"] = 0; });   // 3 times as VEC3 values
    CHECK(contains(e, "VEC3"));
    e = mutated([](json& d) { d.erase("animations"); });
    CHECK(contains(e, "no animations"));
    e = mutated([](json& d) { d["nodes"][2]["children"] = {1}; });   // spine -> hips: two parents
    CHECK(contains(e, "two parents"));
    e = mutated([](json& d) { d["nodes"][3]["children"] = {3}; });
    CHECK(contains(e, "own child"));
    e = mutated([](json& d) { d["nodes"][3]["matrix"] = std::vector<double>(16, 0.0); });
    CHECK(contains(e, "matrix"));
    e = mutated([](json& d) { d["extensionsRequired"] = {"KHR_draco_mesh_compression"}; });
    CHECK(contains(e, "KHR_draco_mesh_compression"));
    e = mutated([](json& d) { d["asset"]["version"] = "1.0"; });
    CHECK(contains(e, "version"));
    e = mutated([](json& d) { d["skins"][0]["joints"] = {1, 77}; });
    CHECK(contains(e, "skin 0"));
    {   // Times going backwards.
        Scene bad = chainScene();
        bad.tracks[0].times = {0, 0.7f, 0.5f};
        const GltfBuilder bb = build(bad);
        CHECK(contains(refusal(glbOf(bb.doc, bb.bin)), "backwards"));
    }
    {   // CUBICSPLINE needs three values per keyframe.
        Scene bad = chainScene();
        bad.tracks[3].values.resize(3 * 4);
        const GltfBuilder bb = build(bad);
        CHECK(contains(refusal(glbOf(bb.doc, bb.bin)), "CUBICSPLINE"));
    }
    // Deep nesting is refused before parsing.
    const std::string deep = std::string(100000, '[') + std::string(100000, ']');
    CHECK(contains(refusal(std::vector<std::uint8_t>(deep.begin(), deep.end())), "nested"));

    // External buffers: only plain relative names inside the folder.
    TempDir dir;
    writeBytes(dir / "ok.bin", b.bin);
    REQUIRE(refusal(gltfOf(b.doc, b.bin.size(), "ok.bin"), dir.path()).empty());
    CHECK_FALSE(refusal(gltfOf(b.doc, b.bin.size(), "ok.bin")).empty());   // no folder given
    for (const char* uri : {"../ok.bin", "sub/../../ok.bin", "./../ok.bin", "%2E%2E/ok.bin", "/etc/passwd", "C:/Windows/win.ini",
                            "C:\\Windows\\win.ini", "\\\\server\\share\\x.bin", "http://example.com/x.bin", "file:///etc/passwd",
                            "data:application/octet-stream,plain", "data:application/octet-stream;base64,@@@@", "missing.bin", ""}) {
        INFO(uri);
        CHECK_FALSE(refusal(gltfOf(b.doc, b.bin.size(), uri), dir.path()).empty());
    }

    // Byte flips anywhere in the GLB: refused or read, never a crash.
    for (std::size_t i = 0; i < good.size(); i += 3) {
        g = good;
        g[i] = static_cast<std::uint8_t>(g[i] ^ 0x5A);
        (void)parseGltf(g, {});
    }
    // Limits.
    GltfOptions tight;
    tight.maxBytes = 100;
    CHECK_FALSE(parseGltf(good, {}, tight));
    tight = {};
    tight.maxJoints = 3;
    CHECK_FALSE(parseGltf(good, {}, tight));
    tight = {};
    tight.maxKeyframes = 2;
    CHECK(contains(parseGltf(good, {}, tight).error().message, "count"));
    tight = {};
    tight.fps = 0;
    CHECK_FALSE(parseGltf(good, {}, tight));
}

TEST_CASE("glTF: a Mixamo-named humanoid walk retargets like the BVH walk", "[mocap][gltf][anim]") {
    // The BVH walk, written as a GLB: centimetre bones under an Armature
    // scaled to metres, one rotation channel per joint and the hips' path.
    auto bvh = parseBvh(walkBvh(mixamo(), 30));
    REQUIRE(bvh);
    const Motion& src = bvh.value();
    Scene sc;
    sc.nodes.push_back({"Armature", -1, {0, 0, 0}, {0.01, 0.01, 0.01}, {}});
    std::vector<float> times;
    for (int f = 0; f < src.frameCount; ++f) times.push_back(static_cast<float>(f * src.frameTime));
    for (std::size_t i = 0; i < src.joints.size(); ++i) {
        const Joint& j = src.joints[i];
        sc.nodes.push_back({j.name, j.parent + 1, {j.offset.x, j.offset.y, j.offset.z}, {1, 1, 1}, {}});
        sc.skin.push_back(static_cast<int>(i) + 1);
        if (j.channels.empty()) continue;
        TTrack rot{static_cast<int>(i) + 1, "rotation", "LINEAR", times, {}};
        TTrack pos{static_cast<int>(i) + 1, "translation", "LINEAR", times, {}};
        for (int f = 0; f < src.frameCount; ++f) {
            const double* v = &src.values[static_cast<std::size_t>(f * src.channelCount + j.firstChannel)];
            Q q;
            Vec3 p = j.offset;
            for (std::size_t c = 0; c < j.channels.size(); ++c) {
                switch (j.channels[c]) {
                case Channel::Xposition: p.x += v[c]; break;
                case Channel::Yposition: p.y += v[c]; break;
                case Channel::Zposition: p.z += v[c]; break;
                case Channel::Xrotation: q = qmul(q, axisAngle(1, 0, 0, v[c])); break;
                case Channel::Yrotation: q = qmul(q, axisAngle(0, 1, 0, v[c])); break;
                case Channel::Zrotation: q = qmul(q, axisAngle(0, 0, 1, v[c])); break;
                }
            }
            pushQ(rot.values, q);
            for (double d : {p.x, p.y, p.z}) pos.values.push_back(static_cast<float>(d));
        }
        sc.tracks.push_back(rot);
        if (j.parent < 0) sc.tracks.push_back(pos);
    }
    const GltfBuilder b = build(sc, false);
    auto r = parseGltf(glbOf(b.doc, b.bin), {});
    REQUIRE(r);
    const Motion& m = r.value();
    REQUIRE(m.joints.size() == src.joints.size());
    REQUIRE(m.frameCount == src.frameCount);
    for (std::size_t i = 0; i < m.joints.size(); ++i) {
        CHECK(m.joints[i].name == src.joints[i].name);
        CHECK(m.joints[i].parent == src.joints[i].parent);
    }
    // Same poses, in metres (float keyframes: micrometre agreement).
    for (int f = 0; f < m.frameCount; ++f) {
        const std::vector<Vec3> a = jointPositions(m, f * m.frameTime), e = jointPositions(src, f * src.frameTime);
        for (std::size_t i = 0; i < a.size(); ++i) {
            INFO("frame " << f << ", " << m.joints[i].name);
            CHECK(a[i].x == Approx(e[i].x * 0.01).margin(2e-6));
            CHECK(a[i].y == Approx(e[i].y * 0.01).margin(2e-6));
            CHECK(a[i].z == Approx(e[i].z * 0.01).margin(2e-6));
        }
    }
    const BoneMap map = autoMap(m);
    CHECK(map.hasLegs());
    CHECK(m.joints[static_cast<std::size_t>(map[Bone::LThigh])].name == "mixamorig:LeftUpLeg");
    checkWalkRetarget(m);
}
