#include "acestudio/core/Effects.h"

#include <catch2/catch_test_macros.hpp>

#include <set>

using namespace acestudio;
using namespace acestudio::fx;

namespace {
const Rgba8 kLog{110, 70, 40, 255}, kStone{90, 90, 100, 255};
const Rgba8 kRed{200, 40, 20, 255}, kOrange{240, 130, 30, 255}, kYellow{250, 220, 80, 255};

// A campfire: stones and logs at the bottom, a three-colour flame above.
Image campfire() {
    Image img(32, 32);
    img.fillRect({6, 26, 20, 3}, kStone);
    img.fillRect({9, 23, 14, 3}, kLog);
    for (int y = 8; y < 23; ++y) {
        const int half = (y - 8) * 6 / 15;
        for (int x = 16 - half; x <= 16 + half; ++x) {
            const int d = std::abs(x - 16) + (23 - y) / 3;
            img.setPixel(x, y, d < 2 ? kYellow : d < 4 ? kOrange : kRed);
        }
    }
    return img;
}

std::set<std::uint32_t> colours(const Image& img) {
    std::set<std::uint32_t> s;
    for (const Rgba8& c : img.pixels())
        if (c.a) s.insert((std::uint32_t(c.r) << 24) | (std::uint32_t(c.g) << 16) | (std::uint32_t(c.b) << 8) | c.a);
    return s;
}
} // namespace

TEST_CASE("fire flickers in its own colours and leaves the logs alone", "[fx]") {
    const Image src = campfire();
    const Mask region = autoRegion(src, Effect::Fire);
    CHECK(region.get(16, 15));        // flame
    CHECK_FALSE(region.get(12, 24));  // log
    EffectOptions o;
    o.effect = Effect::Fire;
    o.frames = 8;
    const EffectClip c = animateEffect(src, o);
    REQUIRE(c.frames.size() == 8);
    const auto palette = colours(src);
    int changedFrames = 0;
    for (const Image& f : c.frames) {
        CHECK(f.width() == src.width());
        for (std::uint32_t col : colours(f)) CHECK(palette.count(col) == 1);   // palette locked
        for (int y = 23; y < 29; ++y)                                           // logs and stones untouched
            for (int x = 0; x < 32; ++x) CHECK(f.pixel(x, y) == src.pixel(x, y));
        changedFrames += !(f == src);
    }
    CHECK(changedFrames >= 6);        // it moves
    CHECK_FALSE(c.frames[0] == c.frames[4]);
}

TEST_CASE("effect loops are seamless: the cycle is periodic", "[fx]") {
    const Image src = campfire();
    for (Effect e : {Effect::Fire, Effect::Sway, Effect::Pulse, Effect::Flag}) {
        INFO(toString(e));
        EffectOptions o;
        o.effect = e;
        o.frames = 8;
        const auto eight = animateEffect(src, o);
        o.frames = 4;
        const auto four = animateEffect(src, o);
        // Frame 2 of 4 and frame 4 of 8 are the same moment of the loop.
        CHECK(four.frames[2] == eight.frames[4]);
        CHECK(four.frames[0] == eight.frames[0]);
    }
}

TEST_CASE("water flows along each row and wraps once per loop", "[fx]") {
    Image pool(24, 6);
    const Rgba8 a{30, 90, 200, 255}, b{60, 140, 230, 255};
    for (int y = 0; y < 6; ++y)
        for (int x = 0; x < 24; ++x) pool.setPixel(x, y, (x / 3) % 2 ? a : b);
    EffectOptions o;
    o.effect = Effect::Water;
    o.frames = 8;
    o.amount = 1.0;
    const auto c = animateEffect(pool, o);
    CHECK_FALSE(c.frames[0] == c.frames[1]);
    for (const Image& f : c.frames) CHECK(colours(f) == colours(pool));
}

TEST_CASE("a tree sways from the top and keeps its trunk", "[fx]") {
    Image tree(20, 30);
    tree.fillRect({8, 18, 4, 12}, kLog);                         // trunk
    tree.fillRect({2, 2, 16, 16}, Rgba8{40, 140, 50, 255});       // canopy
    EffectOptions o;
    o.effect = Effect::Sway;
    o.frames = 8;
    const auto c = animateEffect(tree, o);
    bool topMoved = false;
    for (const Image& f : c.frames) {
        for (int y = 26; y < 30; ++y)
            for (int x = 0; x < 20; ++x) CHECK(f.pixel(x, y) == tree.pixel(x, y));   // the base stays
        for (int x = 0; x < 20; ++x) topMoved = topMoved || !(f.pixel(x, 2) == tree.pixel(x, 2));
    }
    CHECK(topMoved);
}

TEST_CASE("generated fire changes every frame and rises", "[fx][fire]") {
    const Image src = campfire();
    for (FireStyle st : {FireStyle::Campfire, FireStyle::Torch, FireStyle::Furnace, FireStyle::Raging, FireStyle::Magic}) {
        INFO(toString(st));
        EffectOptions o;
        o.effect = Effect::Fire;
        o.fire = st;
        const EffectClip c = animateEffect(src, o);
        REQUIRE(c.frames.size() >= 8);
        const auto palette = colours(src);
        int upward = 0, pairs = 0;
        for (std::size_t i = 0; i < c.frames.size(); ++i) {
            const Image& a = c.frames[i];
            const Image& b = c.frames[(i + 1) % c.frames.size()];
            CHECK_FALSE(a == b);   // never a held frame
            for (std::uint32_t col : colours(a)) CHECK(palette.count(col) == 1);
            // Buoyancy: the flame pattern of the next frame matches this one
            // best when shifted up (features rise), not down.
            auto score = [&](int dy) {
                int same = 0;
                for (int y = 2; y < 22; ++y)
                    for (int x = 0; x < a.width(); ++x) {
                        const int ty = y + dy;
                        if (ty < 0 || ty >= b.height()) continue;
                        same += (a.pixel(x, y).a && a.pixel(x, y) == b.pixel(x, ty)) ? 1 : 0;
                    }
                return same;
            };
            int best = 0, bestScore = -1;
            for (int dy = -3; dy <= 3; ++dy)
                if (const int s = score(dy); s > bestScore) { bestScore = s; best = dy; }
            ++pairs;
            upward += best <= 0 ? 1 : 0;
        }
        CHECK(upward * 4 >= pairs * 3);   // mostly rising (or still), rarely sinking
    }
}

TEST_CASE("raging fire reaches higher than a torch", "[fx][fire]") {
    // The test campfire with room above it.
    Image src(32, 64);
    src.blit(campfire(), 0, 32);
    auto tallest = [&](FireStyle st) {
        EffectOptions o;
        o.effect = Effect::Fire;
        o.fire = st;
        int top = src.height();
        for (const Image& f : animateEffect(src, o).frames) top = std::min(top, f.opaqueBounds().y);
        return top;
    };
    CHECK(tallest(FireStyle::Raging) < tallest(FireStyle::Torch));
}
