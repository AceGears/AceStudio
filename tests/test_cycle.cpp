// A walk from drawn poses (CycleStabilize.h) and the walk checks of
// checkClip (docs/research/walk-animation-failure-2026-11.md).

#include "acestudio/core/CycleStabilize.h"

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <vector>

using namespace acestudio;
using namespace acestudio::motion;

namespace {

const Rgba8 kSkin{230, 180, 140, 255}, kCloth{90, 60, 140, 255};

// A leg colour and a badge colour of its own for each pose, so a frame shows
// whose legs and whose upper body it was built from.
Rgba8 legColour(int pose) { return Rgba8{static_cast<std::uint8_t>(40 + pose * 30), 60, 200, 255}; }
Rgba8 badgeColour(int pose) { return Rgba8{250, static_cast<std::uint8_t>(40 + pose * 40), 30, 255}; }

// A figure seen from the front, 94 px tall on a 48 x 100 canvas: each foot
// can be lifted. Every pose is drawn a little differently (the badge, and
// the body a column wider in odd poses), as generated poses are.
Image frontPose(int pose, int liftLeft, int liftRight) {
    Image img(48, 100);
    img.fillRect({12, 2, 24, 30}, kSkin);
    img.fillRect({pose % 2 ? 9 : 10, 32, pose % 2 ? 30 : 28, 36}, kCloth);
    img.fillRect({20, 40, 8, 8}, badgeColour(pose));
    img.fillRect({12, 68, 10, 28 - liftLeft}, legColour(pose));
    img.fillRect({26, 68, 10, 28 - liftRight}, legColour(pose));
    return img;
}

// The same figure from the side: the legs stride apart by `spread` columns
// each way and both feet stay down.
Image sidePose(int pose, int spread) {
    Image img(64, 100);
    img.fillRect({20, 2, 24, 30}, kSkin);
    img.fillRect({18, 32, 28, 36}, kCloth);
    img.fillRect({28, 40, 8, 8}, badgeColour(pose));
    img.fillRect({22 - spread, 68, 8, 28}, legColour(pose));
    img.fillRect({34 + spread, 68, 8, 28}, legColour(pose));
    return img;
}

int count(const Image& img, Rgba8 c) {
    int n = 0;
    for (const Rgba8& p : img.pixels()) n += p == c ? 1 : 0;
    return n;
}

int lowestRow(const Image& img) { return img.opaqueBounds().y + img.opaqueBounds().h - 1; }

} // namespace

TEST_CASE("Front poses become stand, step, stand, other step under one upper body", "[cycle]") {
    // As a generated sheet: stand, left step, right step, left step again.
    const std::vector<Image> poses{frontPose(0, 0, 0), frontPose(1, 10, 0), frontPose(2, 0, 10), frontPose(3, 10, 0)};
    CycleOptions o;
    o.master = 0;
    auto res = stabilizeCycle(poses, o);
    REQUIRE(res);
    const CycleResult& r = res.value();
    CHECK(r.frontView);
    CHECK(r.order == std::vector<int>{0, 1, 0, 2});
    CHECK(r.notes.empty());
    REQUIRE(r.clip.frames.size() == 4);
    const int ground = lowestRow(r.clip.frames[0]);
    for (std::size_t i = 0; i < 4; ++i) {
        INFO("frame " << i);
        const Image& f = r.clip.frames[i];
        // The master's upper body in every frame, nobody else's.
        CHECK(count(f, badgeColour(0)) == 64);
        for (int p = 1; p < 4; ++p) CHECK(count(f, badgeColour(p)) == 0);
        CHECK(count(f, kSkin) == 24 * 30);
        // The legs of the pose this frame plays, and only those.
        for (int p = 0; p < 4; ++p) CHECK((count(f, legColour(p)) > 0) == (p == r.order[i]));
        // One ground line.
        CHECK(lowestRow(f) == ground);
        CHECK(f.width() == r.clip.frames[0].width());
    }
    const ClipCheck chk = checkClip(r.clip);
    for (const auto& i : chk.issues) UNSCOPED_INFO(i);
    CHECK(chk.issues.empty());
    CHECK(chk.sameLegSteps == 0);
}

TEST_CASE("The clip checks report a front walk that steps with the same leg twice", "[cycle][qa]") {
    const std::vector<Image> poses{frontPose(0, 0, 0), frontPose(1, 10, 0), frontPose(2, 0, 10), frontPose(3, 10, 0)};
    CycleOptions o;
    o.master = 0;
    o.view = CycleView::Front;
    o.order = {0, 1, 2, 3};   // the sheet's own order: stand, left, right, left
    auto res = stabilizeCycle(poses, o);
    REQUIRE(res);
    const ClipCheck chk = checkClip(res.value().clip);
    CHECK(chk.sameLegSteps == 1);   // frames 1 and 3
    CHECK_FALSE(chk.issues.empty());
}

TEST_CASE("Side poses alternate stand and stride", "[cycle]") {
    // Stand, stride, stride, stand: as drawn, it stalls for two frames.
    const std::vector<Image> poses{sidePose(0, 0), sidePose(1, 8), sidePose(2, 8), sidePose(3, 1)};
    auto res = stabilizeCycle(poses);
    REQUIRE(res);
    const CycleResult& r = res.value();
    CHECK_FALSE(r.frontView);
    CHECK(r.order == std::vector<int>{0, 1, 3, 2});
    REQUIRE(r.master >= 0);
    REQUIRE(r.master < 4);
    for (const Image& f : r.clip.frames) {
        CHECK(count(f, badgeColour(r.master)) == 64);
        CHECK(lowestRow(f) == lowestRow(r.clip.frames[0]));
    }
    CHECK(checkClip(r.clip).issues.empty());
}

TEST_CASE("A single step pose is used and noted", "[cycle]") {
    auto res = stabilizeCycle({frontPose(0, 0, 0), frontPose(1, 10, 0)});
    REQUIRE(res);
    CHECK(res.value().order == std::vector<int>{0, 1});
    CHECK(res.value().notes.size() == 1);
}

TEST_CASE("The stabiliser refuses bad input", "[cycle]") {
    CHECK_FALSE(stabilizeCycle({frontPose(0, 0, 0)}));
    CHECK_FALSE(stabilizeCycle({frontPose(0, 0, 0), Image(8, 8)}));
    CycleOptions o;
    o.order = {0, 5};
    CHECK_FALSE(stabilizeCycle({frontPose(0, 0, 0), frontPose(1, 10, 0)}, o));
    o.order.clear();
    o.master = 2;
    CHECK_FALSE(stabilizeCycle({frontPose(0, 0, 0), frontPose(1, 10, 0)}, o));
}

TEST_CASE("The clip checks report legs that only slide on a large figure", "[cycle][qa]") {
    // A 120 px figure whose two boots are the same drawing in every frame,
    // moved sideways and lifted: what a rig makes of short legs.
    const Rgba8 boot{200, 40, 40, 255}, tread{250, 250, 250, 255};
    auto frame = [&](int dxLeft, int dxRight, int liftLeft, bool redraw) {
        Image img(96, 124);
        img.fillRect({28, 2, 40, 50}, kSkin);
        img.fillRect({30, 52, 36, 46}, kCloth);
        for (int side = 0; side < 2; ++side) {
            const int x = (side == 0 ? 30 + dxLeft : 52 + dxRight), y = 98 - (side == 0 ? liftLeft : 0);
            img.fillRect({x, y, 14, 24}, boot);
            // A drawn step changes the boot itself; a slide does not.
            if (redraw) img.fillRect({x + 2, y + 4 + (dxLeft + 8) % 12, 10, 3}, tread);
            else img.fillRect({x + 2, y + 18, 10, 3}, tread);
        }
        return img;
    };
    auto clipOf = [&](bool redraw) {
        Clip c;
        c.name = "walk";
        c.loops = true;
        c.body = Body::Biped;
        const int dx[] = {0, -4, -8, -4, 0, 4, 8, 4};
        for (int i = 0; i < 8; ++i) {
            c.frames.push_back(frame(dx[i], -dx[i], i == 6 ? 5 : 0, redraw));
            c.durations.push_back(100);
        }
        return c;
    };
    const ClipCheck slide = checkClip(clipOf(false));
    CHECK(slide.legsOnlySlide);
    CHECK_FALSE(slide.issues.empty());
    // Boots that are redrawn from frame to frame are step poses.
    CHECK_FALSE(checkClip(clipOf(true)).legsOnlySlide);
    // A small sprite is not judged: a few pixels of leg cannot show a pose.
    Clip tiny;
    tiny.name = "walk";
    tiny.loops = true;
    tiny.body = Body::Biped;
    for (int i = 0; i < 4; ++i) {
        Image img(24, 34);
        img.fillRect({6, 2, 12, 20}, kCloth);
        img.fillRect({6 + (i % 2), 22, 4, 10}, boot);
        img.fillRect({14 - (i % 2), 22, 4, 10}, boot);
        tiny.frames.push_back(img);
        tiny.durations.push_back(100);
    }
    CHECK_FALSE(checkClip(tiny).legsOnlySlide);
}
