// Consistency checks and corrections for generated frames (Phase 5).
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "acestudio/core/AnimationQA.h"

using namespace acestudio;
using Catch::Approx;

namespace {
const Rgba8 kSkin{230, 180, 140, 255}, kShirt{40, 90, 200, 255}, kBg{250, 250, 245, 255};

Palette pal() {
    Palette p;
    (void)p.add(kSkin);
    (void)p.add(kShirt);
    (void)p.add({20, 20, 20, 255});
    return p;
}

// A 10x20 "figure" (head + body) standing with its feet on row `ground`.
Image figure(int w, int h, int cx, int ground, int height = 20, Rgba8 body = kShirt) {
    Image img(w, h);
    const int top = ground - height + 1;
    img.fillRect({cx - 2, top, 4, height / 4}, kSkin);                     // head
    img.fillRect({cx - 5, top + height / 4, 10, height - height / 4}, body); // body
    return img;
}
} // namespace

TEST_CASE("Border background removal keeps enclosed pixels", "[qa]") {
    Image img(20, 20, kBg);
    img.fillRect({5, 5, 10, 10}, kShirt);
    img.fillRect({8, 8, 4, 4}, kBg);                // background-coloured hole inside the subject
    const Image out = removeBorderBackground(img);
    CHECK(out.pixel(0, 0).a == 0);
    CHECK(out.pixel(19, 19).a == 0);
    CHECK(out.pixel(6, 6) == kShirt);
    CHECK(out.pixel(9, 9) == kBg);                  // enclosed: kept
    Image noisy(8, 8, kBg);
    noisy.setPixel(0, 0, Rgba8{240, 240, 236, 255});  // within tolerance of the median border
    CHECK(removeBorderBackground(noisy).pixel(0, 0).a == 0);
}

TEST_CASE("Silhouette IoU ignores position, sees shape", "[qa]") {
    const Mask a = silhouette(figure(64, 64, 20, 50));
    const Mask moved = silhouette(figure(64, 64, 40, 60));
    CHECK(silhouetteIoU(a, moved) == Approx(1.0));
    const Mask tall = silhouette(figure(64, 64, 20, 50, 40));
    CHECK(silhouetteIoU(a, tall) < 0.6);
    CHECK(silhouetteIoU(Mask(4, 4), Mask(4, 4)) == 1.0);
    CHECK(silhouetteIoU(a, Mask(64, 64)) == 0.0);
}

TEST_CASE("Palette distance and colour mix", "[qa]") {
    const Image f = figure(32, 32, 16, 30);
    CHECK(paletteDistance(f, pal()) == 0.0);
    Image off = f;
    for (auto& p : off.pixels()) if (p.a) p = {p.r, p.g, static_cast<std::uint8_t>(std::min(255, p.b + 60)), p.a};
    CHECK(paletteDistance(off, pal()) > 0.06);
    CHECK(colorMixDistance(f, f, pal()) == 0.0);
    const Image red = figure(32, 32, 16, 30, 20, Rgba8{20, 20, 20, 255});
    CHECK(colorMixDistance(f, red, pal()) > 0.5);     // shirt colour swapped
}

TEST_CASE("Evaluate flags ground, height, palette and shape problems", "[qa]") {
    const Image ref = figure(64, 64, 32, 55);
    const std::vector<Image> frames = {
        figure(64, 64, 32, 55),                       // fine
        figure(64, 64, 34, 58),                       // floating/sunk by 3 px (and moved: fine)
        figure(64, 64, 32, 55, 26),                   // 30% taller
        Image(64, 64),                                // empty
    };
    const QAReport r = evaluateFrames(frames, ref, pal());
    REQUIRE(r.frames.size() == 4);
    CHECK(r.frames[0].ok());
    CHECK(r.frames[0].score == Approx(1.0));
    CHECK((r.frames[1].flags & QAGround));
    CHECK(r.frames[1].groundOffset == 3);
    CHECK_FALSE(r.frames[1].needsRegeneration());
    CHECK((r.frames[2].flags & QAHeight));
    CHECK(r.frames[2].heightRatio == Approx(1.3));
    CHECK((r.frames[3].flags & QAEmpty));
    CHECK(r.frames[3].needsRegeneration());
    CHECK(r.failing() == std::vector<int>{1, 2, 3});
    CHECK(r.toRegenerate() == std::vector<int>{3});
    CHECK(describe(r.frames[1]).find("ground") != std::string::npos);
    CHECK(r.meanScore() < 1.0);
}

TEST_CASE("Corrections fix ground and height, then the frames pass", "[qa]") {
    const Image ref = figure(64, 64, 32, 55);
    std::vector<Image> frames = {figure(64, 64, 20, 50), figure(64, 64, 40, 55, 26)};
    // Off-palette noise on one frame too.
    frames[1].setPixel(40, 50, Rgba8{45, 95, 205, 255});
    const QAReport before = evaluateFrames(frames, ref, pal());
    CHECK_FALSE(before.frames[0].ok());
    CHECK_FALSE(before.frames[1].ok());
    const auto fixed = correctFrames(frames, before, pal());
    const QAReport after = evaluateFrames(fixed, ref, pal());
    for (const auto& q : after.frames) {
        INFO(describe(q));
        CHECK(q.ok());
    }
    // Horizontal positions are kept.
    CHECK(after.frames[0].bounds.x + after.frames[0].bounds.w / 2 == 20);
    CHECK(paletteDistance(fixed[1], pal()) == 0.0);
}

namespace {
const Rgba8 kCape{200, 30, 30, 255};
// The figure with a cape on its back (left of the body when facing right).
Image caped(int w, int h, int cx, int ground, bool facingRight = true) {
    Image img = figure(w, h, cx, ground);
    const int top = ground - 20 + 1;
    img.fillRect({facingRight ? cx - 9 : cx + 5, top + 5, 4, 12}, kCape);
    return img;
}
Palette capePal() {
    Palette p = pal();
    (void)p.add(kCape);
    return p;
}
} // namespace

TEST_CASE("Facing: a mirrored frame is flagged, and Auto-Correct flips it back", "[qa]") {
    const Image ref = caped(64, 64, 32, 55);
    CHECK(facingAgreement(ref, ref) == Approx(1.0));
    CHECK(facingAgreement(caped(64, 64, 20, 50, false), ref) < -0.9);
    CHECK(facingAgreement(figure(64, 64, 32, 55), figure(64, 64, 32, 55)) == 1.0);   // symmetric: undecidable
    const std::vector<Image> frames = {ref, caped(64, 64, 30, 55, false), caped(64, 64, 34, 55)};
    const QAReport r = evaluateFrames(frames, ref, capePal());
    CHECK(r.frames[0].ok());
    CHECK((r.frames[1].flags & QAFlipped));
    CHECK_FALSE(r.frames[1].needsRegeneration());       // fixable
    CHECK(describe(r.frames[1]).find("facing") != std::string::npos);
    INFO(describe(r.frames[2]));
    CHECK(r.frames[2].ok());                            // compared with the corrected shape of frame 1
    const auto fixed = correctFrames(frames, r, capePal());
    const QAReport after = evaluateFrames(fixed, ref, capePal());
    for (const auto& q : after.frames) {
        INFO(describe(q));
        CHECK(q.ok());
    }
    CHECK(facingAgreement(fixed[1], ref) > 0.9);
}

TEST_CASE("Expectations from poses: jumps leave the ground and are not pulled back", "[qa]") {
    const auto poses = cyclePoses(PoseCycle::Jump, 6);
    const auto box = poseFramingBox(poses, 1.0);
    const auto exp = expectationsFromPoses(poses, box, 64);
    REQUIRE(exp.size() == 6);
    CHECK(exp[0].groundOffset == 0);
    CHECK(exp[0].heightRatio == Approx(1.0));
    int highest = 0;
    for (const auto& e : exp) highest = std::min(highest, e.groundOffset);
    CHECK(highest < -2);                               // some frame is airborne
    const auto stand = expectationsFromPoses({standingPose(), standingPose()}, poseFramingBox({standingPose()}, 1.0), 64);
    CHECK(stand[1].groundOffset == 0);
    CHECK(stand[1].heightRatio == Approx(1.0));

    // A frame 8 px up passes when its pose is a jump, fails when it should stand,
    // and Auto-Correct puts a mis-placed jump frame where its pose says.
    const Image ref = figure(64, 64, 32, 55);
    QAOptions o;
    o.expected = {{0, 1.0}, {-8, 1.0}, {-8, 1.0}};
    const std::vector<Image> frames = {ref, figure(64, 64, 32, 47), figure(64, 64, 32, 52)};
    const QAReport withPose = evaluateFrames(frames, ref, pal(), o);
    CHECK(withPose.frames[1].ok());
    CHECK((withPose.frames[2].flags & QAGround));
    CHECK(withPose.frames[2].groundOffset == 5);
    CHECK((evaluateFrames(frames, ref, pal()).frames[1].flags & QAGround));
    const auto fixed = correctFrames(frames, withPose, pal(), o);
    const QAReport after = evaluateFrames(fixed, ref, pal(), o);
    CHECK(after.frames[2].ok());
    CHECK(after.frames[2].bounds.y + after.frames[2].bounds.h - 1 == 47);
    // Expected height: a crouch at 80% passes when the pose crouches.
    QAOptions crouch;
    crouch.expected = {{0, 1.0}, {0, 0.8}};
    const QAReport c = evaluateFrames({ref, figure(64, 64, 32, 55, 16)}, ref, pal(), crouch);
    CHECK(c.frames[1].ok());
}

TEST_CASE("Colour checks work without a profile palette; identity catches an off-model frame 1", "[qa]") {
    const Image ref = figure(64, 64, 32, 55);
    const Image recoloured = figure(64, 64, 32, 55, 20, Rgba8{30, 160, 60, 255});   // green shirt
    // No palette: colours come from the reference, so the drift is still seen.
    const QAReport r = evaluateFrames({ref, recoloured}, ref, Palette());
    CHECK((r.frames[1].flags & QAColorMix));
    CHECK(r.frames[1].needsRegeneration());
    CHECK_FALSE((r.frames[1].flags & QAPalette));    // no target palette: no off-palette check
    // Identity: the character is the blue-shirted figure (a different pose,
    // smaller); a green-shirted frame 1 is flagged, a blue one is not.
    QAOptions o;
    o.identity = {figure(24, 40, 12, 38, 18)};
    const QAReport good = evaluateFrames({ref, ref}, ref, Palette(), o);
    CHECK(good.frames[0].ok());
    CHECK(good.frames[1].ok());
    const QAReport bad = evaluateFrames({recoloured, recoloured}, recoloured, Palette(), o);
    CHECK((bad.frames[0].flags & QAOffModel));
    CHECK(bad.frames[0].needsRegeneration());
    CHECK(describe(bad.frames[0]).find("reference images") != std::string::npos);
}
