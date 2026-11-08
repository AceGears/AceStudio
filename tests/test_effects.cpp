#include "acestudio/core/Effects.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <utility>
#include <vector>

using namespace acestudio;
using namespace acestudio::fx;

namespace {
const Rgba8 kLog{110, 70, 40, 255}, kStone{90, 90, 100, 255};
const Rgba8 kRed{200, 40, 20, 255}, kOrange{240, 130, 30, 255}, kYellow{250, 220, 80, 255};

// A campfire: stones and logs at the bottom, a three-colour flame above,
// its tip on row `flameTop` (2 = almost no headroom, as in a generated
// 64 px campfire whose flame fills the canvas).
Image campfire(int flameTop = 8) {
    Image img(32, 32);
    img.fillRect({6, 26, 20, 3}, kStone);
    img.fillRect({9, 23, 14, 3}, kLog);
    for (int y = flameTop; y < 23; ++y) {
        const int half = (y - flameTop) * 6 / (23 - flameTop);
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

namespace {
EffectClip fireClip(const Image& src, FireStyle st, unsigned seed = 1) {
    EffectOptions o;
    o.effect = Effect::Fire;
    o.fire = st;
    o.seed = seed;
    return animateEffect(src, o);
}

bool isFlame(Rgba8 c) { return c == kRed || c == kOrange || c == kYellow; }

// Separate pieces (4-connected, at least 2 px; single sparks do not count)
// of flame in the top third of the flame: from its highest pixel down to
// the logs (row 22 is the base of the flame).
int tonguesAtTop(const Image& f) {
    const int w = f.width();
    int top = -1;
    for (int y = 0; y < 23 && top < 0; ++y)
        for (int x = 0; x < w; ++x)
            if (isFlame(f.pixel(x, y))) { top = y; break; }
    if (top < 0) return 0;
    const int limit = top + std::max(1, (22 - top + 1) / 3);
    std::vector<char> seen(static_cast<std::size_t>(w) * 23, 0);
    auto at = [w](int x, int y) { return static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x); };
    int pieces = 0;
    for (int y = top; y < limit; ++y)
        for (int x = 0; x < w; ++x) {
            if (seen[at(x, y)] || !isFlame(f.pixel(x, y))) continue;
            int size = 0;
            std::vector<std::pair<int, int>> stack{{x, y}};
            seen[at(x, y)] = 1;
            while (!stack.empty()) {
                const auto [px, py] = stack.back();
                stack.pop_back();
                ++size;
                const int nx[4] = {px + 1, px - 1, px, px};
                const int ny[4] = {py, py, py + 1, py - 1};
                for (int k = 0; k < 4; ++k)
                    if (nx[k] >= 0 && nx[k] < w && ny[k] >= top && ny[k] < limit && !seen[at(nx[k], ny[k])] && isFlame(f.pixel(nx[k], ny[k]))) {
                        seen[at(nx[k], ny[k])] = 1;
                        stack.emplace_back(nx[k], ny[k]);
                    }
            }
            pieces += size >= 2 ? 1 : 0;
        }
    return pieces;
}

// anim_qa.py's diff_irregularity: the spread (coefficient of variation) of
// the frame-to-frame change. A constant-rate rise scores about 0.08-0.10.
double diffIrregularity(const std::vector<Image>& frames) {
    std::vector<double> d;
    for (std::size_t i = 0; i + 1 < frames.size(); ++i) {
        const Image& a = frames[i];
        const Image& b = frames[i + 1];
        double changed = 0, either = 0;
        for (int y = 0; y < a.height(); ++y)
            for (int x = 0; x < a.width(); ++x) {
                const Rgba8 p = a.pixel(x, y), q = b.pixel(x, y);
                if (!p.a && !q.a) continue;
                either += 1;
                changed += p == q ? 0 : 1;
            }
        d.push_back(either > 0 ? changed / either : 0.0);
    }
    if (d.empty()) return 0.0;
    double mean = 0;
    for (double v : d) mean += v;
    mean /= static_cast<double>(d.size());
    double var = 0;
    for (double v : d) var += (v - mean) * (v - mean);
    var /= static_cast<double>(d.size());
    return mean > 0 ? std::sqrt(var) / mean : 0.0;
}
} // namespace

TEST_CASE("fire tips are never cut off by the top of the canvas", "[fx][fire]") {
    for (int flameTop : {8, 2}) {
        const Image src = campfire(flameTop);
        for (FireStyle st : {FireStyle::Campfire, FireStyle::Raging, FireStyle::Torch, FireStyle::Furnace, FireStyle::Magic}) {
            INFO(toString(st) << " flame top " << flameTop);
            const EffectClip c = fireClip(src, st);
            REQUIRE(c.frames.size() >= 8);
            int highest = src.height();
            for (std::size_t i = 0; i < c.frames.size(); ++i) {
                INFO("frame " << i);
                const Image& f = c.frames[i];
                for (int x = 0; x < f.width(); ++x) CHECK(f.pixel(x, 0).a == 0);   // nothing on row 0
                highest = std::min(highest, f.opaqueBounds().y);
            }
            // Fitted, not shrunk: the flame still reaches into the headroom.
            CHECK(highest <= flameTop + 2);
        }
    }
}

TEST_CASE("fire splits into separate tongues at the top", "[fx][fire]") {
    for (int flameTop : {8, 2}) {
        for (FireStyle st : {FireStyle::Campfire, FireStyle::Raging}) {
            INFO(toString(st) << " flame top " << flameTop);
            const EffectClip c = fireClip(campfire(flameTop), st);
            int split = 0;
            for (const Image& f : c.frames) split += tonguesAtTop(f) >= 2 ? 1 : 0;
            CHECK(split >= 2);   // not one teardrop: some frames show two or more tongues
            if (flameTop == 2) CHECK(split * 4 >= static_cast<int>(c.frames.size()));
        }
    }
}

TEST_CASE("fire changes at an irregular rate", "[fx][fire]") {
    // ADR-ANIM-001 3.6: diff_irregularity >= 0.15 (the constant-rate rise of
    // the earlier flame measured 0.08-0.10 on the audit campfire).
    for (FireStyle st : {FireStyle::Campfire, FireStyle::Raging}) {
        INFO(toString(st));
        CHECK(diffIrregularity(fireClip(campfire(2), st).frames) >= 0.15);
    }
}

TEST_CASE("fire keeps the logs, the palette and no lone pale pixels", "[fx][fire]") {
    for (int flameTop : {8, 2}) {
        const Image src = campfire(flameTop);
        const auto palette = colours(src);
        for (FireStyle st : {FireStyle::Campfire, FireStyle::Raging}) {
            INFO(toString(st) << " flame top " << flameTop);
            const EffectClip c = fireClip(src, st);
            int changedLogs = 0, lonePale = 0, offPalette = 0;
            for (const Image& f : c.frames) {
                for (std::uint32_t col : colours(f)) offPalette += palette.count(col) == 1 ? 0 : 1;
                for (int y = 0; y < src.height(); ++y)
                    for (int x = 0; x < src.width(); ++x) {
                        const Rgba8 s = src.pixel(x, y);
                        if (s == kLog || s == kStone) changedLogs += f.pixel(x, y) == s ? 0 : 1;
                        // The palest flame colour only inside the flame: next
                        // to another pale or a mid (orange) pixel, never alone
                        // on the dark rim or floating free.
                        if (f.pixel(x, y) != kYellow) continue;
                        bool supported = false;
                        for (int dy = -1; dy <= 1; ++dy)
                            for (int dx = -1; dx <= 1; ++dx) {
                                const Rgba8 n = f.pixel(x + dx, y + dy);   // transparent outside
                                supported = supported || ((dx || dy) && (n == kYellow || n == kOrange));
                            }
                        lonePale += supported ? 0 : 1;
                    }
            }
            CHECK(changedLogs == 0);
            CHECK(lonePale == 0);
            CHECK(offPalette == 0);
        }
    }
}

TEST_CASE("fire is deterministic for a seed", "[fx][fire]") {
    const Image src = campfire(2);
    for (FireStyle st : {FireStyle::Campfire, FireStyle::Raging}) {
        INFO(toString(st));
        const EffectClip a = fireClip(src, st, 7), b = fireClip(src, st, 7), other = fireClip(src, st, 8);
        REQUIRE(a.frames.size() == b.frames.size());
        bool differs = false;
        for (std::size_t i = 0; i < a.frames.size(); ++i) {
            CHECK(a.frames[i] == b.frames[i]);
            differs = differs || !(a.frames[i] == other.frames[i]);
        }
        CHECK(differs);   // the seed is the pattern
    }
}
