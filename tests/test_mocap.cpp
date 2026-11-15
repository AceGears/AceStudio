// BVH import, bone mapping, camera and retargeting onto a sprite (Mocap.h).

#include "acestudio/core/Mocap.h"

#include "acestudio/core/PngCodec.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
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

TEST_CASE("A 3D walk retargets onto a side-view sprite", "[mocap][anim]") {
    auto sprite = readPng(fixture("rogue64.png"));
    REQUIRE(sprite);
    auto m = parseBvh(walkBvh(mixamo(), 30));
    REQUIRE(m);
    RetargetOptions o;
    o.fps = 12;
    auto r = retarget(sprite.value(), m.value(), o);
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
    // Non-bipeds and skeletons without legs are refused, not guessed.
    o.body = motion::Body::Swimmer;
    CHECK_FALSE(retarget(sprite.value(), m.value(), o));
    auto armless = parseBvh("HIERARCHY\nROOT A\n{\n OFFSET 0 0 0\n CHANNELS 1 Xrotation\n}\nMOTION\nFrames: 1\nFrame Time: 0.1\n5\n");
    REQUIRE(armless);
    CHECK_FALSE(retarget(sprite.value(), armless.value(), RetargetOptions{}));
}
