#include "acestudio/core/SpriteMotion.h"

#include <catch2/catch_test_macros.hpp>

#include <set>

using namespace acestudio;
using namespace acestudio::motion;

namespace {

const Rgba8 kSkin{230, 180, 140, 255}, kShirt{40, 90, 200, 255}, kPants{60, 50, 40, 255}, kInk{20, 20, 30, 255};

// 16 x 24 figure: head, torso, two legs with a gap between them.
Image biped() {
    Image img(20, 28);
    img.fillRect({6, 2, 6, 6}, kSkin);
    img.fillRect({5, 8, 8, 8}, kShirt);
    img.fillRect({5, 16, 3, 9}, kPants);
    img.fillRect({10, 16, 3, 9}, kPants);
    img.fillRect({5, 24, 3, 1}, kInk);
    img.fillRect({10, 24, 3, 1}, kInk);
    return img;
}

Image quadruped() {
    Image img(40, 24);
    img.fillRect({4, 4, 32, 10}, kShirt);    // body
    img.fillRect({32, 0, 6, 6}, kSkin);      // head
    for (int x : {6, 12, 26, 32}) img.fillRect({x, 14, 3, 8}, kPants);
    return img;
}

Image blob() {
    Image img(24, 24);
    for (int y = 0; y < 24; ++y)
        for (int x = 0; x < 24; ++x)
            if ((x - 11.5) * (x - 11.5) + (y - 13.5) * (y - 13.5) <= 100) img.setPixel(x, y, kShirt);
    for (int x = 3; x < 21; ++x) img.setPixel(x, 23, kShirt);   // flat bottom
    return img;
}

std::set<std::uint32_t> colours(const Image& img) {
    std::set<std::uint32_t> s;
    for (const Rgba8& c : img.pixels())
        if (c.a) s.insert((std::uint32_t(c.r) << 24) | (std::uint32_t(c.g) << 16) | (std::uint32_t(c.b) << 8) | c.a);
    return s;
}

} // namespace

TEST_CASE("subjects are classified from the silhouette", "[motion]") {
    const Subject b = analyzeSubject(biped());
    CHECK(b.body == Body::Biped);
    REQUIRE(b.feet.size() == 2);
    CHECK(b.legTop >= 14);
    CHECK(b.legTop <= 17);

    const Subject q = analyzeSubject(quadruped());
    CHECK(q.body == Body::Quadruped);
    CHECK(q.feet.size() == 4);

    CHECK(analyzeSubject(blob()).body == Body::Blob);
}

TEST_CASE("every action renders with the sprite's own colours", "[motion]") {
    const Image src = biped();
    const auto palette = colours(src);
    for (const ActionInfo& info : actions()) {
        Options o;
        o.action = info.action;
        auto clip = animate(src, o);
        REQUIRE(clip);
        const Clip& c = clip.value();
        INFO(info.key);
        REQUIRE(static_cast<int>(c.frames.size()) == info.frames);
        CHECK(c.durations.size() == c.frames.size());
        CHECK(c.loops == info.loops);
        for (const Image& f : c.frames) {
            CHECK(f.width() == c.frames[0].width());
            CHECK(f.height() == c.frames[0].height());
            CHECK_FALSE(f.isFullyTransparent());
            if (info.action != Action::Hurt)
                for (std::uint32_t col : colours(f)) CHECK(palette.count(col) == 1);
        }
    }
}

TEST_CASE("a walk moves the legs and keeps the body", "[motion]") {
    Options o;
    o.action = Action::Walk;
    auto clip = animate(biped(), o);
    REQUIRE(clip);
    const Clip& c = clip.value();
    REQUIRE(c.frames.size() == 8);
    CHECK(c.body == Body::Biped);
    CHECK_FALSE(c.frames[0] == c.frames[2]);   // contact vs passing pose differ
    const ClipCheck chk = checkClip(c);
    CHECK(chk.issues.empty());
    CHECK(chk.maxAreaChange < 0.2);
    CHECK(chk.minNeighbourIoU > 0.5);
}

TEST_CASE("a footless blob hops instead of walking", "[motion]") {
    Options o;
    o.action = Action::Walk;
    auto clip = animate(blob(), o);
    REQUIRE(clip);
    CHECK(clip.value().body == Body::Blob);
    // At least one frame is in the air: its lowest row is above the ground of frame 0.
    const auto ground = [](const Image& f) { const IntRect b = f.opaqueBounds(); return b.y + b.h; };
    const int g0 = ground(clip.value().frames[0]);
    bool airborne = false;
    for (const Image& f : clip.value().frames) airborne = airborne || ground(f) < g0;
    CHECK(airborne);
}

TEST_CASE("death ends lying down and the body override is honoured", "[motion]") {
    Options o;
    o.action = Action::Death;
    auto clip = animate(biped(), o);
    REQUIRE(clip);
    const IntRect last = clip.value().frames.back().opaqueBounds();
    CHECK(last.w > last.h);

    o.action = Action::Walk;
    o.body = Body::Object;
    auto rocked = animate(biped(), o);
    REQUIRE(rocked);
    CHECK(rocked.value().body == Body::Object);
}

TEST_CASE("packs share one frame size and mirroring faces left", "[motion]") {
    Options o;
    auto pack = animatePack(biped(), {Action::Idle, Action::Walk, Action::Jump, Action::Attack1}, o);
    REQUIRE(pack);
    REQUIRE(pack.value().size() == 4);
    for (const Clip& c : pack.value()) {
        CHECK(c.frames[0].width() == pack.value()[0].frames[0].width());
        CHECK(c.frames[0].height() == pack.value()[0].frames[0].height());
    }
    o.action = Action::Idle;
    auto right = animate(biped(), o);
    o.faceLeft = true;
    auto left = animate(biped(), o);
    REQUIRE(right);
    REQUIRE(left);
    const Image& r = right.value().frames[0];
    const Image& l = left.value().frames[0];
    REQUIRE(r.width() == l.width());
    for (int y = 0; y < r.height(); ++y)
        for (int x = 0; x < r.width(); ++x) REQUIRE(r.pixel(x, y) == l.pixel(r.width() - 1 - x, y));
}

TEST_CASE("an empty sprite is refused", "[motion]") {
    CHECK_FALSE(animate(Image(8, 8), Options{}));
    CHECK(actionFromKey("attack2") == Action::Attack2);
    CHECK_FALSE(actionFromKey("moonwalk"));
}

TEST_CASE("a wing flap folds the part above the wing line", "[motion]") {
    Image dragon(30, 30);
    dragon.fillRect({8, 16, 16, 10}, kShirt);   // body
    dragon.fillRect({10, 2, 8, 14}, kSkin);     // wing, above the line
    Options o;
    o.action = Action::Flap;
    o.wingLine = 16.0 / 24.0;                    // the body's top, as a fraction of the opaque height (2..26)
    auto clip = animate(dragon, o);
    REQUIRE(clip);
    const auto& fr = clip.value().frames;
    REQUIRE(fr.size() == 6);
    // Frame 0: wing up (skin above the body). Frame 3: folded below the line, so skin appears under the body's top.
    auto skinBelowRow = [&](const Image& f, int row) {
        for (int y = row; y < f.height(); ++y)
            for (int x = 0; x < f.width(); ++x)
                if (f.pixel(x, y) == kSkin) return true;
        return false;
    };
    // All frames share one canvas; in frame 0 the wing's top is the opaque top and the body starts 14 rows lower.
    const int line = fr[0].opaqueBounds().y + 14;
    CHECK_FALSE(skinBelowRow(fr[0], line));
    bool folded = false;
    for (const Image& f : fr) folded = folded || skinBelowRow(f, line + 2);
    CHECK(folded);
    const ClipCheck chk = checkClip(clip.value());
    INFO(chk.maxAreaChange << " " << chk.minNeighbourIoU << " " << chk.loopIoU);
    for (const auto& i : chk.issues) INFO(i);
    CHECK(chk.issues.empty());
}
