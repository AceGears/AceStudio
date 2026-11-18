// The motion library: built-in presets, BVH writing and saved presets
// (MotionLibrary.h).

#include "acestudio/core/MotionLibrary.h"

#include "acestudio/core/PngCodec.h"

#include "TestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <string>

using namespace acestudio;
using namespace acestudio::mocap;

namespace {

std::filesystem::path fixture(const char* name) { return std::filesystem::path(ACESTUDIO_SOURCE_DIR) / "tests" / "data" / "anim" / name; }

double maxDistance(const std::vector<Vec3>& a, const std::vector<Vec3>& b) {
    double d = 0;
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i)
        d = std::max(d, std::hypot(a[i].x - b[i].x, a[i].y - b[i].y, a[i].z - b[i].z));
    return a.size() == b.size() ? d : 1e9;
}

Motion presetOrFail(const char* name) {
    auto m = builtInPreset(name);
    REQUIRE(m);
    return std::move(m).value();
}

} // namespace

TEST_CASE("Built-in humanoid presets map to a full humanoid and keep their timing", "[mocap][library]") {
    const auto list = builtInPresets();
    REQUIRE(list.size() == 7);
    for (const PresetInfo& p : list) {
        CAPTURE(p.name);
        CHECK(p.builtIn);
        CHECK_FALSE(p.description.empty());
        if (p.fourLegged) continue;   // test_creature_motion.cpp
        const Motion m = presetOrFail(p.name.c_str());
        const BoneMap map = autoMap(m);
        CHECK(map.hasLegs());
        for (Bone b : {Bone::Head, Bone::LUpperArm, Bone::LForearm, Bone::RUpperArm, Bone::RForearm, Bone::LToe, Bone::RToe})
            CHECK(map[b] >= 0);
        // Loops end one frame before they repeat; one-shots end where they start.
        const int steps = static_cast<int>(std::lround(p.seconds * 30));
        CHECK(m.frameCount == (p.loops ? steps : steps + 1));
        if (!p.loops) {
            CHECK(maxDistance(jointPositions(m, 0), jointPositions(m, m.duration())) < 1e-6);
        }
        // Nothing goes below the floor (ankles at y >= 0, toes a little lower).
        const int lfoot = map[Bone::LFoot], rfoot = map[Bone::RFoot];
        for (int f = 0; f < m.frameCount; ++f) {
            const auto w = jointPositions(m, f * m.frameTime);
            CHECK(std::min(w[static_cast<std::size_t>(lfoot)].y, w[static_cast<std::size_t>(rfoot)].y) > -0.01);
        }
    }
    CHECK_FALSE(builtInPreset("moonwalk"));
}

TEST_CASE("The walk alternates its feet and keeps one on the floor", "[mocap][library]") {
    const Motion m = presetOrFail("walk");
    const BoneMap map = autoMap(m);
    const auto lf = static_cast<std::size_t>(map[Bone::LFoot]), rf = static_cast<std::size_t>(map[Bone::RFoot]);
    int leftAhead = 0, rightAhead = 0;
    for (int f = 0; f < m.frameCount; ++f) {
        const auto w = jointPositions(m, f * m.frameTime);
        CHECK(std::min(w[lf].y, w[rf].y) < 0.01);   // a planted foot
        if (w[lf].z > w[rf].z + 5) ++leftAhead;
        if (w[rf].z > w[lf].z + 5) ++rightAhead;
    }
    CHECK(leftAhead >= 5);
    CHECK(rightAhead >= 5);
    // It travels forward (+Z): retargeting finds which way the figure faces.
    CHECK(jointPositions(m, m.duration()).front().z > jointPositions(m, 0).front().z + 20);
}

TEST_CASE("The jump leaves the floor and lands", "[mocap][library]") {
    const Motion m = presetOrFail("jump");
    const BoneMap map = autoMap(m);
    const auto lf = static_cast<std::size_t>(map[Bone::LFoot]);
    double highest = 0;
    for (int f = 0; f < m.frameCount; ++f) highest = std::max(highest, jointPositions(m, f * m.frameTime)[lf].y);
    CHECK(highest > 20);
    CHECK(jointPositions(m, m.duration())[lf].y < 0.01);
}

TEST_CASE("Every built-in humanoid preset retargets onto a sprite", "[mocap][library][anim]") {
    auto sprite = readPng(fixture("rogue64.png"));
    REQUIRE(sprite);
    for (const PresetInfo& p : builtInPresets()) {
        CAPTURE(p.name);
        if (p.fourLegged) continue;   // test_creature_motion.cpp
        const Motion m = presetOrFail(p.name.c_str());
        RetargetOptions o;
        o.fps = 12;
        o.loop = p.loops;
        auto r = retarget(sprite.value(), m, o);
        REQUIRE(r);
        CHECK(r.value().clip.frames.size() >= 6);
        CHECK_FALSE(r.value().approximateView);
    }
}

TEST_CASE("BVH writing round-trips through the reader", "[mocap][library]") {
    const Motion walk = presetOrFail("walk");
    auto text = writeBvh(walk);
    REQUIRE(text);
    auto back = parseBvh(text.value());
    REQUIRE(back);
    REQUIRE(back.value().joints.size() == walk.joints.size());
    CHECK(back.value().frameCount == walk.frameCount);
    CHECK(back.value().channelCount == walk.channelCount);
    for (std::size_t i = 0; i < walk.joints.size(); ++i) CHECK(back.value().joints[i].name == walk.joints[i].name);
    for (int f = 0; f < walk.frameCount; f += 7)
        CHECK(maxDistance(jointPositions(walk, f * walk.frameTime), jointPositions(back.value(), f * walk.frameTime)) < 1e-4);

    // Awkward names (glTF allows anything) come back as unique BVH names.
    Motion odd;
    auto add = [&](std::string name, int parent, std::vector<Channel> ch) {
        Joint j;
        j.name = std::move(name);
        j.parent = parent;
        j.offset = {0, 5, 1};
        j.channels = std::move(ch);
        j.firstChannel = odd.channelCount;
        odd.channelCount += static_cast<int>(j.channels.size());
        odd.joints.push_back(std::move(j));
    };
    add("root {main}", -1, {Channel::Xposition, Channel::Yposition, Channel::Zposition, Channel::Xrotation});
    add("arm  left\tupper", 0, {Channel::Zrotation, Channel::Xrotation, Channel::Yrotation});
    add("arm left upper", 0, {Channel::Yrotation});
    add("   ", 1, {Channel::Xrotation});
    odd.frameCount = 2;
    odd.frameTime = 0.05;
    odd.values = {1, 2, 3, 10, 20, 30, 40, -50, 5, 0.25, 1.5, 2.5, 3.5, -10, 20, 30, 40, 5};
    auto oddText = writeBvh(odd);
    REQUIRE(oddText);
    auto oddBack = parseBvh(oddText.value());
    REQUIRE(oddBack);
    REQUIRE(oddBack.value().joints.size() == 4);
    CHECK(oddBack.value().joints[1].name != oddBack.value().joints[2].name);
    CHECK(oddBack.value().joints[0].name == "root _main_");
    // Joint 3 hangs under joint 1, so BVH lists it third: same poses, new order.
    CHECK(oddBack.value().joints[2].parent == 1);
    for (double t : {0.0, 0.05}) {
        auto a = jointPositions(odd, t), b = jointPositions(oddBack.value(), t);
        std::swap(a[2], a[3]);
        CHECK(maxDistance(a, b) < 1e-9);
    }

    // Refused: two roots, no frames, a value BVH cannot hold.
    Motion twoRoots = odd;
    twoRoots.joints[2].parent = -1;
    CHECK_FALSE(writeBvh(twoRoots));
    Motion empty;
    CHECK_FALSE(writeBvh(empty));
    Motion huge = odd;
    huge.values[3] = 1e9;
    CHECK_FALSE(writeBvh(huge));
}

TEST_CASE("Preset names are checked", "[mocap][library][security]") {
    CHECK(checkPresetName("my walk"));
    CHECK(checkPresetName("Run_2-fast"));
    for (const char* bad : {"", " walk", "walk ", "walk", "WALK", "../x", "a/b", "a\\b", "c:x", "CON", "com1", "x.bvh", "\xc3\xa9t\xc3\xa9"})
        CHECK_FALSE(checkPresetName(bad));
    CHECK_FALSE(checkPresetName(std::string(65, 'a')));
}

TEST_CASE("Presets are saved, listed, loaded and removed in a folder", "[mocap][library]") {
    TempDir tmp;
    const auto dir = tmp / "motions";
    auto none = savedPresets(dir);
    REQUIRE(none);
    CHECK(none.value().empty());   // a missing folder is an empty library

    const Motion run = presetOrFail("run");
    REQUIRE(savePreset(dir, "my run", run));
    CHECK_FALSE(savePreset(dir, "my run", run));          // already saved
    CHECK(savePreset(dir, "my run", run, true));          // overwrite
    CHECK_FALSE(savePreset(dir, "walk", run));            // built-in name
    CHECK_FALSE(savePreset(dir, "../escape", run));
    CHECK_FALSE(std::filesystem::exists(tmp / "escape.bvh"));
    {
        std::ofstream bad(dir / "broken.bvh");
        bad << "HIERARCHY\nROOT A\n{\n";
    }
    auto list = savedPresets(dir);
    REQUIRE(list);
    REQUIRE(list.value().size() == 2);
    CHECK(list.value()[0].name == "broken");
    CHECK_FALSE(list.value()[0].error.empty());
    CHECK(list.value()[1].name == "my run");
    CHECK(list.value()[1].error.empty());
    CHECK(std::abs(list.value()[1].seconds - run.duration()) < 1e-6);

    auto loaded = loadPreset("my run", dir);
    REQUIRE(loaded);
    CHECK(maxDistance(jointPositions(run, 0.3), jointPositions(loaded.value(), 0.3)) < 1e-4);
    CHECK(loadPreset("walk", dir));                       // built-ins come first
    CHECK_FALSE(loadPreset("missing", dir));
    CHECK_FALSE(loadPreset("../motions/my run", dir));

    CHECK_FALSE(removePreset(dir, "walk"));
    REQUIRE(removePreset(dir, "my run"));
    CHECK_FALSE(removePreset(dir, "my run"));
    CHECK_FALSE(loadPreset("my run", dir));
}
