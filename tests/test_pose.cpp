// Editable skeletons and pose cycles (Phase 5).
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "acestudio/core/Pose.h"

#include <set>

using namespace acestudio;
using Catch::Approx;

TEST_CASE("Pose conditioning image uses the joint and limb colours on black", "[pose]") {
    const Pose p = standingPose();
    const Image img = renderPose(p, 512, 512);
    REQUIRE(img.width() == 512);
    CHECK(img.pixel(5, 5) == Rgba8{0, 0, 0, 255});
    // The neck joint sits at its own colour (limbs are blended over it at 60%).
    const Keypoint n = p[Joint::Nose];
    const Rgba8 at = img.pixel(static_cast<int>(n.x * 512), static_cast<int>(n.y * 512));
    CHECK((at.r > 0 || at.g > 0 || at.b > 0));
    // Many distinct colours: every limb draws something.
    std::set<std::uint32_t> colours;
    for (auto px : img.pixels()) colours.insert(px.r << 16 | px.g << 8 | px.b);
    CHECK(colours.size() > 17);
    // Hidden joints and their limbs are not drawn.
    Pose hidden = p;
    for (auto& k : hidden.points) k.visible = false;
    const Image dark = renderPose(hidden, 64, 64);
    for (auto px : dark.pixels()) CHECK(px == Rgba8{0, 0, 0, 255});
    // Sizes scale with the image; tiny images still draw.
    std::size_t lit = 0;
    const Image small = renderPose(p, 64, 64);
    for (auto px : small.pixels()) lit += (px.r || px.g || px.b) ? 1 : 0;
    CHECK(lit > 40);
}

TEST_CASE("Walk, run and idle keep the feet on the ground line", "[pose]") {
    for (PoseCycle c : {PoseCycle::Idle, PoseCycle::Walk, PoseCycle::Run}) {
        for (const Pose& p : cyclePoses(c, 8)) CHECK(p.groundY() == Approx(0.9).margin(1e-9));
    }
    // Legs alternate: at t = 0.25 the right ankle is ahead, at 0.75 the left one.
    const Pose a = cyclePose(PoseCycle::Walk, 0.25), b = cyclePose(PoseCycle::Walk, 0.75);
    CHECK(a[Joint::RAnkle].x > a[Joint::LAnkle].x);
    CHECK(b[Joint::LAnkle].x > b[Joint::RAnkle].x);
    // A cycle loops: phase 1 equals phase 0.
    CHECK(cyclePose(PoseCycle::Walk, 1.0) == cyclePose(PoseCycle::Walk, 0.0));
    // Running reaches further than walking.
    CHECK(std::abs(cyclePose(PoseCycle::Run, 0.25)[Joint::RAnkle].x - 0.5) >
          std::abs(cyclePose(PoseCycle::Walk, 0.25)[Joint::RAnkle].x - 0.5));
}

TEST_CASE("Jump leaves the ground mid-cycle and lands", "[pose]") {
    CHECK(cyclePose(PoseCycle::Jump, 0.0).groundY() == Approx(0.9));
    CHECK(cyclePose(PoseCycle::Jump, 0.5).groundY() < 0.75);    // airborne
    CHECK(cyclePose(PoseCycle::Jump, 0.95).groundY() == Approx(0.9).margin(1e-9));
    // Arms go up while airborne.
    const Pose air = cyclePose(PoseCycle::Jump, 0.5);
    CHECK(air[Joint::RWrist].y < air[Joint::RShoulder].y);
}

TEST_CASE("Mirroring, transforms and JSON round trip", "[pose]") {
    const Pose p = cyclePose(PoseCycle::Walk, 0.3);
    const Pose mm = p.mirrored().mirrored();
    for (int i = 0; i < kJointCount; ++i) {
        CHECK(mm.points[static_cast<std::size_t>(i)].x == Approx(p.points[static_cast<std::size_t>(i)].x));
        CHECK(mm.points[static_cast<std::size_t>(i)].visible == p.points[static_cast<std::size_t>(i)].visible);
    }
    const Pose m = p.mirrored();
    CHECK(m[Joint::LAnkle].x == Approx(1.0 - p[Joint::RAnkle].x));
    const Pose t = p.transformed(0.5, 0.25, 0.1);
    CHECK(t[Joint::Neck].x == Approx(p[Joint::Neck].x * 0.5 + 0.25));
    auto back = poseFromJson(poseToJson(p));
    REQUIRE(back);
    for (int i = 0; i < kJointCount; ++i) {
        CHECK(back.value().points[static_cast<std::size_t>(i)].x == Approx(p.points[static_cast<std::size_t>(i)].x));
        CHECK(back.value().points[static_cast<std::size_t>(i)].visible == p.points[static_cast<std::size_t>(i)].visible);
    }
    CHECK_FALSE(poseFromJson(nlohmann::json::array()));
    CHECK_FALSE(poseFromJson({{"keypoints", {{"neck", {{"x", "a"}, {"y", 0}}}}}}));
    CHECK_FALSE(poseFromJson({{"keypoints", {{"neck", {{"x", 1e9}, {"y", 0}}}}}}));
    CHECK(poseCycleFromString("run") == PoseCycle::Run);
    CHECK_FALSE(poseCycleFromString("fly"));
}

TEST_CASE("Framing box covers every pose at the target aspect", "[pose]") {
    const auto poses = cyclePoses(PoseCycle::Walk, 8);
    const auto b = poseFramingBox(poses, 1.0);
    CHECK(b[2] - b[0] == Approx(b[3] - b[1]));          // square
    for (const Pose& p : poses)
        for (const Keypoint& k : p.points)
            if (k.visible) {
                CHECK(k.x > b[0]);
                CHECK(k.x < b[2]);
                CHECK(k.y > b[1]);
                CHECK(k.y < b[3]);
            }
    CHECK(b[0] >= 0.0);
    CHECK(b[3] <= 1.0);
    const auto wide = poseFramingBox(poses, 2.0);
    CHECK((wide[2] - wide[0]) / (wide[3] - wide[1]) == Approx(2.0));
    Pose none;
    for (auto& k : none.points) k.visible = false;
    CHECK(poseFramingBox({none}, 1.0) == std::array<double, 4>{0, 0, 1, 1});
}

TEST_CASE("Cycles fit in the image: a jump's raised hands and head stay inside", "[pose]") {
    for (PoseCycle c : {PoseCycle::Idle, PoseCycle::Walk, PoseCycle::Run, PoseCycle::Jump}) {
        const auto poses = cyclePoses(c, 8);
        for (const Pose& p : poses)
            for (const Keypoint& k : p.points) {
                if (!k.visible) continue;
                INFO(toString(c));
                CHECK(k.x >= 0.03 - 1e-9);
                CHECK(k.x <= 0.97 + 1e-9);
                CHECK(k.y >= 0.07);          // margin + head room (scaled with the figure) above the eyes
                CHECK(k.y <= 0.97 + 1e-9);
            }
    }
    // The jump still rises: some frame's feet are well above the first frame's.
    const auto jump = cyclePoses(PoseCycle::Jump, 6);
    double highest = jump[0].groundY();
    for (const Pose& p : jump) highest = std::min(highest, p.groundY());
    CHECK(jump[0].groundY() - highest > 0.08);
    // Poses that already fit are unchanged.
    auto walk = std::vector<Pose>{cyclePose(PoseCycle::Walk, 0.0)};
    const Pose before = walk[0];
    fitPoses(walk);
    CHECK(walk[0] == before);
}
