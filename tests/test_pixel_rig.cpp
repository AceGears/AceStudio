// The whole-pixel rig (core/src/PixelRig.cpp), driven through animate():
// the dragon flap's z-order and fold limits, and the humanoid walk's front
// view, leg gap and rigid held items (audit 2026-11-08).

#include "acestudio/core/SpriteMotion.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>
#include <vector>

using namespace acestudio;
using namespace acestudio::motion;

namespace {

const Rgba8 kSkin{230, 180, 140, 255}, kShirt{40, 90, 200, 255}, kPants{60, 50, 40, 255}, kInk{20, 20, 30, 255},
    kShield{200, 60, 50, 255};

int countColour(const Image& img, Rgba8 c) {
    int n = 0;
    for (const Rgba8& p : img.pixels()) n += p == c ? 1 : 0;
    return n;
}

// Rows of `img` holding at least one pixel of colour `c`, top to bottom.
std::vector<int> rowsWith(const Image& img, Rgba8 c) {
    std::vector<int> rows;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x)
            if (img.pixel(x, y) == c) {
                rows.push_back(y);
                break;
            }
    return rows;
}

std::vector<std::pair<int, int>> pixelsOf(const Image& img, Rgba8 c) {
    std::vector<std::pair<int, int>> v;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x)
            if (img.pixel(x, y) == c) v.push_back({x, y});
    return v;
}

// Side-view dragon, 64 x 64: a body with legs, a raised wing (a painted
// mask, rows 6-35, root on the body's top row 36), the head and neck drawn on
// the body under the wing (where the old fold covered it) and a horn above
// the root beside the wing (where a spread wing drawn on top would cover it).
struct Dragon {
    Image img{64, 64};
    Mask wing{64, 64};
};
Dragon sideDragon() {
    Dragon d;
    d.img.fillRect({16, 36, 30, 14}, kShirt);   // body
    d.img.fillRect({20, 50, 4, 6}, kShirt);     // legs
    d.img.fillRect({38, 50, 4, 6}, kShirt);
    for (int y = 6; y < 36; ++y)
        for (int x = 18; x <= std::min(41, 18 + (y - 6) * 24 / 29); ++x) {
            d.img.setPixel(x, y, kSkin);
            d.wing.set(x, y, true);
        }
    d.img.fillRect({30, 36, 12, 10}, kPants);   // head and neck
    d.img.fillRect({42, 30, 3, 6}, kPants);     // horn
    return d;
}

// Front-facing figure, 32 x 48, legs apart (a 2 px gap), a wide shield on
// its left arm and a thin bare arm on the right: the whole sprite is far
// from its mirror image (about 48 %), the legs are exactly symmetric.
Image shieldKnight() {
    Image img(32, 48);
    img.fillRect({12, 4, 8, 8}, kSkin);      // head
    img.fillRect({11, 12, 10, 14}, kShirt);  // torso
    img.fillRect({5, 14, 5, 10}, kShield);   // shield, held on the left
    img.fillRect({21, 13, 2, 12}, kSkin);    // right arm
    img.fillRect({12, 26, 3, 16}, kPants);   // legs
    img.fillRect({17, 26, 3, 16}, kPants);
    img.fillRect({12, 42, 3, 2}, kInk);      // boots, as wide as the legs
    img.fillRect({17, 42, 3, 2}, kInk);
    return img;
}

} // namespace

TEST_CASE("A flapping dragon keeps its head while its wing sweeps down", "[motion][rig][flap]") {
    // The part rig (ADR-ANIM-001 5b): the wing is a part that turns about its
    // shoulder, in front of the body, sweeping away from the head; the head
    // is never covered (ADR section 6, dragon tests 2, 5, 7).
    const Dragon d = sideDragon();
    Options o;
    o.action = Action::Flap;
    o.wingMask = d.wing;
    auto clip = animate(d.img, o);
    REQUIRE(clip);
    const auto& fr = clip.value().frames;
    REQUIRE(fr.size() == 8);

    const int head0 = countColour(d.img, kPants);
    const int wing0 = countColour(d.img, kSkin);
    REQUIRE(head0 == 138);
    // Frame 0 is the pose as drawn.
    CHECK(countColour(fr[0], kPants) == head0);
    // The wing's middle (mean row of its pixels), relative to the body.
    auto wingMid = [&](const Image& f) {
        const auto px = pixelsOf(f, kSkin);
        double sum = 0;
        for (const auto& p : px) sum += p.second;
        return px.empty() ? 0.0 : sum / static_cast<double>(px.size());
    };
    auto bodyTop = [&](const Image& f) { return rowsWith(f, kShirt).front(); };
    const double mid0 = wingMid(fr[0]) - bodyTop(fr[0]);
    double lowest = 0;
    int lowestFrame = 0;
    bool rose = false;
    for (std::size_t i = 0; i < fr.size(); ++i) {
        const Image& f = fr[i];
        INFO("frame " << i);
        // The head and horn: every pixel in every frame (never covered).
        CHECK(countColour(f, kPants) == head0);
        // The wing turns, it is not folded flat: its pixels stay (rotation
        // keeps the area) and it is never a line of a few rows.
        const int wing = countColour(f, kSkin);
        CHECK(wing * 10 >= wing0 * 7);
        CHECK(rowsWith(f, kSkin).size() >= 3);
        // How far the wing has come down, relative to the body.
        const double drop = (wingMid(f) - bodyTop(f)) - mid0;
        if (drop > lowest) {
            lowest = drop;
            lowestFrame = static_cast<int>(i);
        }
        rose = rose || bodyTop(f) < bodyTop(fr[0]);
        // Only the sprite's colours.
        for (const Rgba8& p : f.pixels()) CHECK((p.a == 0 || p == kSkin || p == kShirt || p == kPants));
    }
    // A real downstroke: the wing's middle comes down by at least a third of
    // the wing's 30 rows, quickly (the lowest pose by the middle), and the
    // body rises with it.
    CHECK(lowest >= 10);
    CHECK(lowestFrame >= 2);
    CHECK(lowestFrame <= 5);   // the outer wing trails the arm by under a frame
    CHECK(rose);
    // Eight different poses (no ping-pong), and the app's checks pass.
    int unique = 0;
    for (std::size_t i = 0; i < fr.size(); ++i) {
        bool seen = false;
        for (std::size_t j = 0; j < i && !seen; ++j) seen = fr[j] == fr[i];
        unique += seen ? 0 : 1;
    }
    CHECK(unique == 8);
    const ClipCheck chk = checkClip(clip.value());
    for (const auto& i : chk.issues) UNSCOPED_INFO(i);
    CHECK(chk.issues.empty());
}

TEST_CASE("A dragon's two wings: the far one behind the body, trailing the near one", "[motion][rig][flap]") {
    // Two wings rising from a body whose head (and horn) is on the right: the
    // left wing is the near one, the right wing (behind the neck) the far one.
    Image img(80, 64);
    const Rgba8 kNear{90, 30, 30, 255}, kFar{60, 20, 60, 255};
    img.fillRect({20, 34, 36, 14}, kShirt);   // body
    img.fillRect({24, 48, 5, 8}, kShirt);     // legs
    img.fillRect({44, 48, 5, 8}, kShirt);
    img.fillRect({50, 26, 12, 12}, kPants);   // head, rising above the back on the right
    for (int y = 6; y < 34; ++y) {
        const int k = (y - 6) / 3;
        for (int x = 14 - k; x <= 24 + k / 2; ++x) img.setPixel(x, y, kNear);   // left wing
        for (int x = 38 - k / 2; x <= 44 + k / 2; ++x) img.setPixel(x, y, kFar);   // right wing
    }
    Options o;
    o.action = Action::Flap;
    o.wingLine = (26.0 - 6.0) / (56.0 - 6.0);   // above the head
    o.wingFrom = 0.0;
    o.wingTo = (48.0 - 4.0) / 66.0;             // left of the head
    auto clip = animate(img, o);
    REQUIRE(clip);
    const auto& fr = clip.value().frames;
    const int head0 = countColour(img, kPants);
    bool farHidden = false, nearOverBody = false;
    for (std::size_t i = 0; i < fr.size(); ++i) {
        INFO("frame " << i);
        // The head is in front of both wings.
        CHECK(countColour(fr[i], kPants) == head0);
        // The far wing is a part of its own: it moves in every frame.
        if (i > 0) CHECK_FALSE(pixelsOf(fr[i], kFar) == pixelsOf(fr[i - 1], kFar));
        farHidden = farHidden || countColour(fr[i], kFar) * 10 < countColour(fr[0], kFar) * 9;
        nearOverBody = nearOverBody || countColour(fr[i], kShirt) < countColour(fr[0], kShirt);
    }
    // The far wing passes behind the body or head; the near wing in front.
    CHECK(farHidden);
    CHECK(nearOverBody);
}

TEST_CASE("A front-facing walker holding a shield steps in place", "[motion][rig][walk]") {
    const Image src = shieldKnight();
    const Subject s = analyzeSubject(src);
    REQUIRE(s.body == Body::Biped);
    REQUIRE(s.feet.size() == 2);
    CHECK(s.facing == 0);
    CHECK_FALSE(s.frontView);   // legs apart: the analysis alone does not call it a front view
    for (Action a : {Action::Walk, Action::Run}) {
        Options o;
        o.action = a;
        o.frames = 8;
        auto clip = animate(src, o);
        REQUIRE(clip);
        const auto& fr = clip.value().frames;
        REQUIRE(fr.size() == 8);

        // Front view detected: the boots never move sideways (no stride),
        // and each foot is lifted in turn.
        auto bootColumns = [](const Image& f) {
            std::set<int> c;
            for (const auto& p : pixelsOf(f, kInk)) c.insert(p.first);
            return c;
        };
        const std::set<int> cols0 = bootColumns(fr[0]);
        REQUIRE(cols0.size() == 6);
        const int mid = (*cols0.begin() + *cols0.rbegin()) / 2;
        int ground = 0;
        for (const Image& f : fr) ground = std::max(ground, rowsWith(f, kInk).back());
        bool leftLifted = false, rightLifted = false;
        for (const Image& f : fr) {
            CHECK(bootColumns(f) == cols0);
            int left = -1, right = -1;   // lowest boot row on each side
            for (const auto& p : pixelsOf(f, kInk)) {
                if (p.first <= mid) left = std::max(left, p.second);
                else right = std::max(right, p.second);
            }
            leftLifted = leftLifted || left < ground;
            rightLifted = rightLifted || right < ground;
        }
        CHECK(leftLifted);
        CHECK(rightLifted);

        // The legs never merge: the columns between the boots stay empty in
        // every leg row (below the torso's lowest reach) of every frame.
        std::vector<int> gap;
        for (int x = *cols0.begin(); x <= *cols0.rbegin(); ++x)
            if (!cols0.count(x)) gap.push_back(x);
        REQUIRE(gap.size() == 2);
        const int legTop0 = rowsWith(fr[0], kPants).front();
        for (std::size_t i = 0; i < fr.size(); ++i) {
            INFO("frame " << i);
            int filled = 0;
            for (int y = legTop0 + 2; y < fr[i].height(); ++y)
                for (int x : gap) filled += fr[i].pixel(x, y).a ? 1 : 0;
            CHECK(filled == 0);
        }

        // The shield moves rigidly: every frame holds the same shape, only
        // translated.
        const auto shield0 = pixelsOf(fr[0], kShield);
        REQUIRE(shield0.size() == 50);
        for (std::size_t i = 0; i < fr.size(); ++i) {
            INFO("frame " << i);
            const auto sh = pixelsOf(fr[i], kShield);
            REQUIRE(sh.size() == shield0.size());
            const int dx = sh.front().first - shield0.front().first, dy = sh.front().second - shield0.front().second;
            bool same = true;
            for (std::size_t k = 0; k < sh.size(); ++k)
                same = same && sh[k].first == shield0[k].first + dx && sh[k].second == shield0[k].second + dy;
            CHECK(same);
        }

        const ClipCheck chk = checkClip(clip.value());
        for (const auto& issue : chk.issues) UNSCOPED_INFO(issue);
        CHECK(chk.issues.empty());
    }
}
