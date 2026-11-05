#include "acestudio/core/KeyPoses.h"

#include <catch2/catch_test_macros.hpp>

using namespace acestudio;
using namespace acestudio::keypose;

namespace {
const Rgba8 kBody{180, 30, 30, 255}, kWing{120, 20, 20, 255}, kBelly{230, 160, 90, 255};

Image dragon() {
    Image img(40, 32);
    img.fillRect({8, 4, 18, 10}, kWing);     // wings, above the line
    img.fillRect({6, 14, 26, 10}, kBody);    // body
    img.fillRect({10, 20, 18, 3}, kBelly);
    img.fillRect({9, 24, 3, 6}, kBody);      // legs
    img.fillRect({24, 24, 3, 6}, kBody);
    return img;
}
} // namespace

TEST_CASE("The flap plan keeps the drawing as the up pose and asks for three more", "[keypose]") {
    const Plan p = planFor(motion::Action::Flap, motion::Body::Flyer);
    REQUIRE(p.poses.size() == 4);
    CHECK(p.poses.front().original);
    int generated = 0;
    for (const PoseSpec& s : p.poses) generated += s.original ? 0 : 1;
    CHECK(generated == 3);
    CHECK(planFor(motion::Action::Walk, motion::Body::Biped).poses.empty());
    CHECK(posePrompt("red dragon", p.poses[2]).find("red dragon") != std::string::npos);
}

TEST_CASE("Inpainting protects the body and frees the wings and the space around it", "[keypose]") {
    const Image d = dragon();
    const Mask wings = wingMask(d, 10.0 / 26.0, 0, 1);   // the line between wings (rows 4-13) and body (14+)
    CHECK(wings.get(10, 6));
    CHECK_FALSE(wings.get(10, 16));
    const InpaintInputs in = inpaintInputs(d, wings, 512);
    CHECK(in.scale == 12);
    CHECK(in.init.width() % 16 == 0);
    CHECK(in.init.height() % 16 == 0);
    const int ox = (in.init.width() - d.width() * in.scale) / 2, oy = (in.init.height() - d.height() * in.scale) / 2;
    auto at = [&](const Image& img, int x, int y) { return img.pixel(ox + x * in.scale + in.scale / 2, oy + y * in.scale + in.scale / 2); };
    CHECK(at(in.mask, 10, 16) == Rgba8{0, 0, 0, 255});        // body: kept
    CHECK(at(in.mask, 10, 6) == Rgba8{255, 255, 255, 255});   // wing: repainted
    CHECK(at(in.mask, 1, 1) == Rgba8{255, 255, 255, 255});    // empty space: free for wings
    CHECK(at(in.init, 10, 16) == kBody);
    CHECK(at(in.init, 1, 1) == Rgba8{255, 255, 255, 255});
}

TEST_CASE("A pose is aligned on the body and the body is restored pixel for pixel", "[keypose]") {
    const Image d = dragon();
    const Plan p = planFor(motion::Action::Flap, motion::Body::Flyer);
    const Mask wings = wingMask(d, 10.0 / 26.0, 0, 1);
    // A "generated" pose: wings down, the whole thing shifted by (2, 1) and
    // the body re-rendered a shade lighter.
    Image pose(40, 32);
    pose.fillRect({8, 15, 18, 3}, kWing);
    pose.fillRect({6 + 2, 14 + 1, 26, 10}, Rgba8{190, 36, 36, 255});
    pose.fillRect({10 + 2, 20 + 1, 18, 3}, Rgba8{236, 166, 96, 255});   // the belly, re-rendered too
    pose.fillRect({4 + 2, 24 + 1, 30, 4}, kWing);   // the down-swept wing, below the belly
    pose.fillRect({9 + 2, 24 + 1, 3, 6}, kBody);
    pose.fillRect({24 + 2, 24 + 1, 3, 6}, kBody);
    Aligned a = align(pose, d, p);
    CHECK(a.dx == -2);
    CHECK(a.dy == -1);
    const int restored = restoreBody(a.image, d, wings);
    CHECK(restored > 100);
    CHECK(a.image.pixel(10, 16) == kBody);          // the body is the sprite's own again
    const motion::Clip c = assemble(p, d, {a, a, a});
    CHECK(c.frames.size() == 4);
    CHECK(c.frames[0] == d);
    CHECK(c.durations[1] < c.durations[3]);          // the downstroke is quicker than the recovery
}
