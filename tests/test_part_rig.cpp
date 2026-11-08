// The part rig itself (core/src/PartRig.h, ADR-ANIM-001 steps 1-4) and its
// species modules on real sprites (golden fixtures in tests/data/anim).

#include "PartRig.h"

#include "acestudio/core/PngCodec.h"
#include "acestudio/core/SpriteMotion.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace acestudio;
using namespace acestudio::motion;

namespace {

const Rgba8 kA{200, 60, 50, 255}, kB{40, 90, 200, 255}, kInk{20, 20, 30, 255}, kSole1{250, 200, 0, 255},
    kSole2{0, 200, 120, 255}, kSkin{230, 180, 140, 255}, kCloth{90, 60, 140, 255};

int countColour(const Image& img, Rgba8 c) {
    int n = 0;
    for (const Rgba8& p : img.pixels()) n += p == c ? 1 : 0;
    return n;
}

std::set<std::uint32_t> colours(const Image& img) {
    std::set<std::uint32_t> s;
    for (const Rgba8& c : img.pixels())
        if (c.a) s.insert((std::uint32_t(c.r) << 24) | (std::uint32_t(c.g) << 16) | (std::uint32_t(c.b) << 8) | c.a);
    return s;
}

Mask rectMask(int w, int h, IntRect r) {
    Mask m(w, h);
    m.fillRect(r, true);
    return m;
}

// A sprite of two blocks: an outlined bar (kA, inked edge) and a block (kB).
Image twoBlocks() {
    Image img(32, 24);
    img.fillRect({4, 4, 8, 16}, kInk);
    img.fillRect({5, 5, 6, 14}, kA);
    img.fillRect({16, 8, 10, 8}, kB);
    return img;
}

Image load(const char* name) {
    auto img = readPng(std::string(ACESTUDIO_SOURCE_DIR "/tests/data/anim/") + name);
    REQUIRE(img);
    return img.value();
}

} // namespace

TEST_CASE("The rig's rest pose is the sprite, pixel for pixel", "[rig]") {
    const Image img = twoBlocks();
    rig::Rig r;
    r.width = img.width();
    r.height = img.height();
    rig::addPart(r, img, "bar", rectMask(32, 24, {4, 4, 8, 16}), {8, 20}, 0);
    rig::addPart(r, img, "block", rectMask(32, 24, {16, 8, 10, 8}), {16, 12}, 1);
    rig::inpaintHidden(r, 6);
    rig::prepare(r);
    CHECK(rig::renderPose(r, r.rest(), {0, 0, 32, 24}) == img);
}

TEST_CASE("A part that only moves is copied by whole pixels", "[rig]") {
    const Image img = twoBlocks();
    rig::Rig r;
    r.width = img.width();
    r.height = img.height();
    rig::addPart(r, img, "all", rig::opaqueMask(img), {16, 12}, 0);
    rig::prepare(r);
    auto pose = r.rest();
    pose[0].dx = 3;
    pose[0].dy = -2;
    const Image out = rig::renderPose(r, pose, {0, 0, 32, 24});
    for (int y = 0; y < 24; ++y)
        for (int x = 0; x < 32; ++x) {
            INFO(x << "," << y);
            CHECK(out.pixel(x, y) == img.pixel(x - 3, y + 2));
        }
}

TEST_CASE("A turned part keeps the sprite's colours, binary alpha and its area", "[rig]") {
    const Image img = twoBlocks();
    rig::Rig r;
    r.width = img.width();
    r.height = img.height();
    rig::addPart(r, img, "bar", rectMask(32, 24, {4, 4, 8, 16}), {8, 20}, 0);
    rig::prepare(r);
    const int area0 = static_cast<int>(rig::opaqueMask(img).count()) - countColour(img, kB);
    for (double a : {0.2, 0.5, -0.4, 1.0}) {
        INFO("angle " << a);
        auto pose = r.rest();
        pose[0].angle = a;
        const Image out = rig::renderPose(r, pose, {-16, -16, 64, 56});
        int area = 0;
        for (const Rgba8& p : out.pixels()) {
            CHECK((p.a == 0 || p.a == 255));
            CHECK((p.a == 0 || p == kA || p == kInk));
            area += p.a ? 1 : 0;
        }
        CHECK(area * 10 >= area0 * 9);
        CHECK(area * 10 <= area0 * 11);
        // Clockwise on screen: a bar standing on its pivot leans right.
        if (a > 0.3) CHECK(out.opaqueBounds().x + out.opaqueBounds().w > 16 + 12);
    }
}

TEST_CASE("Pixels a front part hides are filled in on the part behind", "[rig]") {
    // A leg (kA) whose middle is drawn over by a body (kB). Moved apart, the
    // leg must not show a hole where the body was.
    Image img(24, 24);
    img.fillRect({8, 2, 4, 20}, kA);
    img.fillRect({2, 9, 20, 4}, kB);
    rig::Rig r;
    r.width = 24;
    r.height = 24;
    rig::addPart(r, img, "leg", rectMask(24, 24, {8, 2, 4, 20}), {10, 2}, 0);
    rig::addPart(r, img, "body", rectMask(24, 24, {2, 9, 20, 4}), {12, 11}, 1);
    const int leg = r.find("leg");
    REQUIRE(leg >= 0);
    CHECK_FALSE(r.parts[static_cast<std::size_t>(leg)].own.get(9, 10));   // the newest claim won
    rig::inpaintHidden(r, 6);
    rig::prepare(r);
    auto pose = r.rest();
    pose[static_cast<std::size_t>(r.find("body"))].visible = false;
    const Image out = rig::renderPose(r, pose, {0, 0, 24, 24});
    for (int y = 2; y < 22; ++y)
        for (int x = 8; x < 12; ++x) {
            INFO(x << "," << y);
            CHECK(out.pixel(x, y) == kA);
        }
    CHECK(countColour(out, kB) == 0);
}

TEST_CASE("Two-bone IK keeps both bone lengths and bends to the asked side", "[rig][ik]") {
    const rig::Vec root{0, 0}, target{3, 4};
    for (int bend : {+1, -1}) {
        INFO("bend " << bend);
        const rig::Vec j = rig::solveTwoBone(root, target, 4, 3, bend);
        CHECK(std::abs(std::hypot(j.x - root.x, j.y - root.y) - 4) < 1e-6);
        CHECK(std::abs(std::hypot(target.x - j.x, target.y - j.y) - 3) < 1e-6);
        // Right of root->target (screen coordinates): (-uy, ux).
        const double side = (j.x - root.x) * -0.8 + (j.y - root.y) * 0.6;
        CHECK(side * bend > 0);
    }
    // Out of reach: the leg straightens toward the target.
    const rig::Vec j = rig::solveTwoBone(root, {100, 0}, 4, 3, +1);
    CHECK(std::abs(j.x - 4) < 1e-6);
    CHECK(std::abs(j.y) < 0.01);   // a hair of bend keeps the side
}

TEST_CASE("A depth swap is reported only where the parts overlap", "[rig]") {
    const Image img = twoBlocks();
    rig::Rig r;
    r.width = img.width();
    r.height = img.height();
    rig::addPart(r, img, "bar", rectMask(32, 24, {4, 4, 8, 16}), {8, 20}, 0);
    rig::addPart(r, img, "block", rectMask(32, 24, {16, 8, 10, 8}), {16, 12}, 1);
    rig::prepare(r);
    auto f0 = r.rest(), f1 = r.rest();
    f0[1].dx = f1[1].dx = -10;   // the block over the bar in both frames
    f0[0].z = 0;
    f0[1].z = 1;
    f1[0].z = 1;
    f1[1].z = 0;
    const IntRect box{0, 0, 32, 24};
    // Once each way round the loop (the clip wraps from f1 to f0).
    CHECK(rig::zPops(r, {f0, f1}, box).size() == 2);
    // The same swap while they are apart is fine.
    auto g0 = f0, g1 = f1;
    g0[1].dx = g1[1].dx = 0;
    CHECK(rig::zPops(r, {g0, g1}, box).empty());
}

TEST_CASE("A walking side view plants a foot that moves at the walk's speed", "[rig][gait]") {
    // A side-view walker facing right, each foot a colour of its own.
    Image img(28, 34);
    img.fillRect({10, 2, 7, 7}, kSkin);    // head
    img.fillRect({16, 5, 2, 2}, kSkin);    // nose: faces right
    img.fillRect({9, 9, 9, 11}, kCloth);   // body
    img.fillRect({9, 20, 3, 10}, kB);      // far leg
    img.fillRect({15, 20, 3, 10}, kA);     // near leg
    img.fillRect({9, 30, 5, 2}, kSole1);   // far foot, toes right
    img.fillRect({15, 30, 5, 2}, kSole2);  // near foot
    Options o;
    o.action = Action::Walk;
    o.body = Body::Biped;
    o.frames = 8;
    auto clip = animate(img, o);
    REQUIRE(clip);
    const auto& fr = clip.value().frames;
    REQUIRE(fr.size() == 8);
    // Each frame: the lowest row and the leftmost column of each foot colour.
    struct Foot {
        int bottom = -1, left = 1 << 20, n = 0;
    };
    auto foot = [](const Image& f, Rgba8 c) {
        Foot ft;
        for (int y = 0; y < f.height(); ++y)
            for (int x = 0; x < f.width(); ++x)
                if (f.pixel(x, y) == c) {
                    ft.bottom = std::max(ft.bottom, y);
                    ft.left = std::min(ft.left, x);
                    ++ft.n;
                }
        return ft;
    };
    int ground = 0;
    for (const Image& f : fr) ground = std::max({ground, foot(f, kSole1).bottom, foot(f, kSole2).bottom});
    std::map<int, int> steps;   // per-frame move of a foot on the ground in both frames
    for (std::size_t i = 0; i < fr.size(); ++i) {
        INFO("frame " << i);
        const Foot a = foot(fr[i], kSole1), b = foot(fr[i], kSole2);
        REQUIRE(a.bottom >= 0);
        REQUIRE(b.bottom >= 0);
        // A foot is on the ground in every frame, and none goes below it.
        CHECK((a.bottom == ground || b.bottom == ground));
        const std::size_t j = (i + 1) % fr.size();
        for (Rgba8 c : {kSole1, kSole2}) {
            const Foot p = foot(fr[i], c), q = foot(fr[j], c);
            // Only where the whole foot shows (the near shin can hide the far foot).
            if (p.bottom == ground && q.bottom == ground && p.n == 10 && q.n == 10) ++steps[q.left - p.left];
        }
    }
    // Planted feet do not slide forward or jitter: every frame on the ground
    // the foot moves back by one speed (the body walks over it).
    REQUIRE_FALSE(steps.empty());
    INFO("steps: " << steps.size());
    for (auto [dx, n] : steps) UNSCOPED_INFO("dx " << dx << " x" << n);
    CHECK(steps.size() == 1);
    CHECK(steps.begin()->first < 0);   // backwards: the walker faces right
    CHECK(checkClip(clip.value()).issues.empty());
}

// ---- Golden sprites (ADR-ANIM-001 section 6) ---------------------------------

TEST_CASE("Golden: the dragon flaps and flies with clean clips", "[rig][golden]") {
    const Image img = load("dragon128.png");
    for (Action a : {Action::Flap, Action::Fly}) {
        INFO(actionInfo(a).key);
        Options o;
        o.action = a;
        auto clip = animate(img, o);
        REQUIRE(clip);
        const auto& fr = clip.value().frames;
        std::set<std::size_t> unique;
        for (std::size_t i = 0; i < fr.size(); ++i) {
            std::size_t first = i;
            for (std::size_t j = 0; j < i; ++j)
                if (fr[j] == fr[i]) {
                    first = j;
                    break;
                }
            unique.insert(first);
            for (auto c : colours(fr[i])) CHECK(colours(img).count(c) == 1);
        }
        CHECK(unique.size() == fr.size());
        const ClipCheck chk = checkClip(clip.value());
        for (const auto& i : chk.issues) UNSCOPED_INFO(i);
        CHECK(chk.issues.empty());
    }
}

TEST_CASE("Golden: the fish keeps its length in idle, swim and dash", "[rig][golden]") {
    const Image img = load("fish57.png");
    const int w0 = img.opaqueBounds().w;
    for (Action a : {Action::Idle, Action::Swim, Action::Dash}) {
        INFO(actionInfo(a).key);
        Options o;
        o.action = a;
        o.body = Body::Swimmer;
        auto clip = animate(img, o);
        REQUIRE(clip);
        for (const Image& f : clip.value().frames) {
            CHECK(f.opaqueBounds().w * 5 >= w0 * 4);
            // The tail fin turns out a little further at the dash's surge.
            CHECK(f.opaqueBounds().w <= w0 + std::max(2, w0 / 12));
            for (auto c : colours(f)) CHECK(colours(img).count(c) == 1);
        }
        const ClipCheck chk = checkClip(clip.value());
        for (const auto& i : chk.issues) UNSCOPED_INFO(i);
        CHECK(chk.issues.empty());
    }
}

TEST_CASE("Golden: the rogue and the knight walk, run and sprint with clean clips", "[rig][golden]") {
    // The rogue is a three-quarter view (its feet stand a row apart); the
    // knight is seen from the front. No foot leaves the ground, no shin pokes
    // through it, and only the sprite's colours appear.
    for (const char* name : {"rogue64.png", "knight32.png"}) {
        const Image img = load(name);
        for (Action a : {Action::Walk, Action::Run, Action::Sprint}) {
            INFO(name << " " << actionInfo(a).key);
            Options o;
            o.action = a;
            auto clip = animate(img, o);
            REQUIRE(clip);
            for (const Image& f : clip.value().frames)
                for (auto c : colours(f)) CHECK(colours(img).count(c) == 1);
            const ClipCheck chk = checkClip(clip.value());
            for (const auto& i : chk.issues) UNSCOPED_INFO(i);
            CHECK(chk.issues.empty());
        }
    }
}
