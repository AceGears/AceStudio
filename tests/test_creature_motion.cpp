// Four-legged motion (ADR-ANIM-002 stage 3c): the quad-walk preset, legs
// found by shape, and retargeting onto a four-legged sprite.

#include "acestudio/core/Mocap.h"
#include "acestudio/core/MotionLibrary.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <string>

using namespace acestudio;
using namespace acestudio::mocap;

namespace {

Motion quad() {
    auto m = builtInPreset("quad-walk");
    REQUIRE(m);
    return std::move(m).value();
}

std::size_t uz(int v) { return static_cast<std::size_t>(v); }

// A dog-like side view facing right: body, head, tail and four legs.
Image dogSprite() {
    const Rgba8 coat{150, 100, 60, 255}, dark{90, 60, 35, 255}, nose{30, 20, 20, 255};
    Image img(72, 48);
    img.fillRect({10, 12, 46, 14}, coat);   // body
    img.fillRect({52, 4, 14, 12}, coat);    // head
    img.fillRect({64, 9, 4, 4}, nose);
    img.fillRect({4, 10, 7, 3}, dark);      // tail
    for (int x : {14, 22, 44, 52}) img.fillRect({x, 26, 5, 20}, x == 22 || x == 52 ? dark : coat);
    return img;
}

} // namespace

TEST_CASE("The quad-walk preset walks on four legs", "[mocap][creature]") {
    bool listed = false;
    for (const PresetInfo& p : builtInPresets())
        if (p.name == "quad-walk") listed = p.fourLegged && p.loops;
    CHECK(listed);
    const Motion m = quad();
    const CreatureMap cm = creatureMap(m);
    REQUIRE(cm.hasFourLegs());
    CHECK(cm.forward.z > 0.99);
    CHECK(cm.legs[0].hind);
    CHECK(cm.legs[1].hind);
    CHECK_FALSE(cm.legs[2].hind);
    CHECK_FALSE(cm.legs[3].hind);
    CHECK(m.joints[uz(cm.legs[0].joints.front())].name.find("Hind") != std::string::npos);
    CHECK(m.joints[uz(cm.legs[2].joints.front())].name.find("Front") != std::string::npos);
    // Every foot lifts at some point; at every frame the lowest stands on the floor.
    std::vector<double> highest(4, 0.0);
    for (int f = 0; f < m.frameCount; ++f) {
        const auto w = jointPositions(m, f * m.frameTime);
        double lowest = 1e9;
        for (std::size_t i = 0; i < 4; ++i) {
            const double y = w[uz(cm.legs[i].joints.back())].y;
            lowest = std::min(lowest, y);
            highest[i] = std::max(highest[i], y);
        }
        CHECK(std::abs(lowest) < 1.0);
    }
    for (double h : highest) CHECK(h > 2.0);
    CHECK(jointPositions(m, m.duration()).front().z > jointPositions(m, 0).front().z + 20);
}

TEST_CASE("Legs are found by shape, the front from the head when nothing travels", "[mocap][creature]") {
    // A humanoid has two legs: not a four-legged motion.
    auto walk = builtInPreset("walk");
    REQUIRE(walk);
    const CreatureMap human = creatureMap(walk.value());
    CHECK_FALSE(human.hasFourLegs());
    REQUIRE_FALSE(human.notes.empty());
    CHECK(human.notes[0].find("found 2") != std::string::npos);

    // The walk on the spot: the head still tells the front.
    Motion still = quad();
    for (int f = 0; f < still.frameCount; ++f) still.values[uz(f) * uz(still.channelCount) + 2] = 0;
    const CreatureMap cm = creatureMap(still);
    REQUIRE(cm.hasFourLegs());
    CHECK(cm.forward.z > 0.99);
    CHECK(cm.legs[0].hind);
    CHECK_FALSE(cm.legs[3].hind);

    // Turned a quarter turn about Y, on the spot: the head still tells the
    // front.
    Motion turned = quad();
    const int c = turned.joints[0].firstChannel;   // X, Y, Z position, then Z, X, Y rotation
    for (int f = 0; f < turned.frameCount; ++f) {
        double* v = &turned.values[uz(f) * uz(turned.channelCount) + uz(c)];
        v[0] = 0;       // on the spot: the head tells the front
        v[2] = 0;
        v[5] = -90;     // Y rotation
    }
    const CreatureMap t = creatureMap(turned);
    REQUIRE(t.hasFourLegs());
    // Forward is the way the head now points (whichever way the rotation turned it).
    const auto w = jointPositions(turned, 0);
    std::size_t headJ = 0;
    for (std::size_t j = 0; j < turned.joints.size(); ++j)
        if (turned.joints[j].name == "Head") headJ = j;
    const double hx = w[headJ].x - w[0].x;
    CAPTURE(t.forward.x, t.forward.z, hx);
    CHECK(std::abs(t.forward.x) > 0.99);
    CHECK(t.forward.x * hx > 0);
    CHECK(turned.joints[uz(t.legs[0].joints.front())].name.find("Hind") != std::string::npos);
}

TEST_CASE("A four-legged walk retargets onto a four-legged sprite", "[mocap][creature][anim]") {
    const Image dog = dogSprite();
    REQUIRE(motion::analyzeSubject(dog).body == motion::Body::Quadruped);
    RetargetOptions o;
    o.body = motion::Body::Quadruped;
    o.fps = 10;
    auto r = retarget(dog, quad(), o);
    REQUIRE(r);
    const RetargetResult& res = r.value();
    CHECK(res.clip.body == motion::Body::Quadruped);
    CHECK(res.clip.frames.size() == 12);   // 1.2 s at 10 fps
    CHECK(res.source.frames.size() == res.clip.frames.size());
    CHECK(res.creature.hasFourLegs());
    CHECK(res.camera.turn == 90);          // drawn facing right
    CHECK_FALSE(res.approximateView);
    std::set<std::size_t> distinct;
    for (const Image& f : res.clip.frames) {
        CHECK(f.width() == res.clip.frames[0].width());
        std::size_t h = 0;
        for (const Rgba8& p : f.pixels()) h = h * 31 + p.a + p.r * 7u;
        distinct.insert(h);
    }
    CHECK(distinct.size() >= 6);   // the legs move
    const motion::ClipCheck chk = motion::checkClip(res.clip);
    CAPTURE(chk.issues);
    CHECK(chk.floatingFrames == 0);
    CHECK(chk.mirrorFlips == 0);
    // The sprite keeps its own colours (rigid parts, no blending).
    std::set<std::uint32_t> drawn, made;
    for (const Rgba8& p : dog.pixels())
        if (p.a) drawn.insert((std::uint32_t(p.r) << 16) | (std::uint32_t(p.g) << 8) | p.b);
    for (const Image& f : res.clip.frames)
        for (const Rgba8& p : f.pixels())
            if (p.a) made.insert((std::uint32_t(p.r) << 16) | (std::uint32_t(p.g) << 8) | p.b);
    for (std::uint32_t c : made) CHECK(drawn.count(c) == 1);

    // A humanoid motion has no four legs; a four-legged one is refused on nothing drawn.
    auto walk = builtInPreset("walk");
    REQUIRE(walk);
    auto bad = retarget(dog, walk.value(), o);
    REQUIRE_FALSE(bad);
    CHECK(bad.error().message.find("four legs") != std::string::npos);
    CHECK_FALSE(retarget(Image(8, 8), quad(), o));
}

TEST_CASE("The back, head and tail of a four-legged sprite follow the motion", "[mocap][creature][anim]") {
    const Motion m = quad();
    const CreatureMap cm = creatureMap(m);
    REQUIRE(cm.hasFourLegs());
    REQUIRE(cm.head >= 0);
    REQUIRE(cm.tail >= 0);
    CHECK(m.joints[uz(cm.head)].name.find("Head") != std::string::npos);
    CHECK(m.joints[uz(cm.tail)].name.find("Tail") != std::string::npos);

    // The same walk with a stiff back, neck and tail.
    Motion stiff = m;
    for (const char* name : {"Spine", "Chest", "Neck", "Tail", "Tail1"})
        for (std::size_t j = 0; j < stiff.joints.size(); ++j)
            if (stiff.joints[j].name == name)
                for (int f = 0; f < stiff.frameCount; ++f)
                    for (std::size_t c = 0; c < stiff.joints[j].channels.size(); ++c)
                        stiff.values[uz(f) * uz(stiff.channelCount) + uz(stiff.joints[j].firstChannel) + c] = 0;
    REQUIRE(creatureMap(stiff).hasFourLegs());

    // How far the nose (its own colour) rises and falls over the loop.
    const Image dog = dogSprite();
    auto noseSpread = [&](const Motion& mo) {
        RetargetOptions o;
        o.body = motion::Body::Quadruped;
        o.fps = 10;
        auto r = retarget(dog, mo, o);
        REQUIRE(r);
        int lo = 1 << 20, hi = -1;
        for (const Image& f : r.value().clip.frames) {
            int top = -1;
            for (int y = 0; y < f.height() && top < 0; ++y)
                for (int x = 0; x < f.width(); ++x)
                    if (f.pixel(x, y) == Rgba8{30, 20, 20, 255}) {
                        top = y;
                        break;
                    }
            REQUIRE(top >= 0);   // the nose is never lost
            lo = std::min(lo, top);
            hi = std::max(hi, top);
        }
        return hi - lo;
    };
    const int bobbing = noseSpread(m), still = noseSpread(stiff);
    CAPTURE(bobbing, still);
    CHECK(bobbing >= 2);
    CHECK(bobbing > still);
    CHECK(bobbing <= 12);   // a quarter of the sprite's height at most
}

namespace {

// An ant-like side view facing right: abdomen, body, head and six legs.
Image antSprite() {
    const Rgba8 shell{60, 42, 30, 255}, face{122, 74, 42, 255}, leg{34, 22, 14, 255};
    Image img(96, 44);
    img.fillRect({20, 10, 50, 10}, shell);   // body
    img.fillRect({70, 6, 12, 10}, face);     // head
    img.fillRect({6, 8, 16, 8}, shell);      // abdomen
    for (int x : {24, 31, 40, 47, 56, 63}) img.fillRect({x, 20, 3, 18}, leg);
    return img;
}

} // namespace

TEST_CASE("Six legs are found by shape: a hind, a middle and a front pair", "[mocap][creature]") {
    bool listed = false;
    for (const PresetInfo& p : builtInPresets())
        if (p.name == "hex-walk") listed = p.fourLegged && p.loops && p.builtIn;
    CHECK(listed);
    auto hm = builtInPreset("hex-walk");
    REQUIRE(hm);
    const Motion m = std::move(hm).value();
    const CreatureMap cm = creatureMap(m);
    for (const std::string& n : cm.notes) UNSCOPED_INFO(n);
    REQUIRE(cm.hasFourLegs());
    REQUIRE(cm.middle.size() == 2);
    CHECK(cm.legCount() == 6);
    CHECK(cm.forward.z > 0.9);
    auto top = [&](std::size_t i) { return m.joints[uz(cm.leg(i).joints.front())].name; };
    for (std::size_t i = 0; i < 6; ++i) {
        CAPTURE(i, top(i));
        const char* row = i < 2 ? "Hind" : i < 4 ? "Front" : "Middle";
        CHECK(top(i).find(row) != std::string::npos);
        CHECK(cm.leg(i).joints.size() >= 3);
        CHECK(cm.leg(i).hind == (i < 2));
    }
    CHECK(top(0) != top(1));
    CHECK(top(4) != top(5));
    REQUIRE(cm.head >= 0);
    CHECK(m.joints[uz(cm.head)].name.rfind("Head", 0) == 0);
    REQUIRE(cm.tail >= 0);
    CHECK(m.joints[uz(cm.tail)].name.rfind("Abdomen", 0) == 0);
    bool said = false;
    for (const std::string& n : cm.notes) said = said || n.find("6 legs") != std::string::npos;
    CHECK(said);

    // The tripod: in every frame at least three feet are near the floor, and
    // each foot is lifted at some time.
    for (std::size_t i = 0; i < 6; ++i) {
        double hi = 0;
        for (int f = 0; f < m.frameCount; ++f) hi = std::max(hi, jointPositions(m, f * m.frameTime)[uz(cm.leg(i).joints.back())].y);
        CHECK(hi > 1.0);
    }
    for (int f = 0; f < m.frameCount; ++f) {
        const auto w = jointPositions(m, f * m.frameTime);
        int down = 0;
        for (std::size_t i = 0; i < 6; ++i) down += w[uz(cm.leg(i).joints.back())].y < 1.0 ? 1 : 0;
        CHECK(down >= 3);
    }

    // Five legs are not read: one leg cut short of the floor.
    Motion five = m;
    for (Joint& j : five.joints)
        if (j.name.rfind("LeftMiddle", 0) == 0 && j.name != "LeftMiddleThigh") j.offset = {0, -0.5, 0};
    const CreatureMap bad = creatureMap(five);
    CHECK_FALSE(bad.hasFourLegs());
    REQUIRE_FALSE(bad.notes.empty());
    CHECK(bad.notes.front().find("found 5 leg(s)") != std::string::npos);
}

TEST_CASE("A six-legged walk retargets onto six-legged and four-legged sprites", "[mocap][creature][anim]") {
    auto hm = builtInPreset("hex-walk");
    REQUIRE(hm);
    const Motion m = std::move(hm).value();
    const Image ant = antSprite();
    RetargetOptions o;
    o.body = motion::Body::Quadruped;
    auto r = retarget(ant, m, o);
    INFO((r ? std::string() : r.error().message));
    REQUIRE(r);
    const RetargetResult& res = r.value();
    for (const std::string& n : res.notes) UNSCOPED_INFO(n);
    CHECK(res.clip.frames.size() == 7);   // 0.6 s at 12 fps
    CHECK(res.creature.middle.size() == 2);
    CHECK(res.camera.turn == 90);
    bool paired = false;
    for (const std::string& n : res.notes) paired = paired || n.find("6 legs and the sprite 6") != std::string::npos;
    CHECK(paired);
    std::set<std::size_t> distinct;
    std::set<std::uint32_t> drawn;
    for (const Rgba8& p : ant.pixels())
        if (p.a) drawn.insert((std::uint32_t(p.r) << 16) | (std::uint32_t(p.g) << 8) | p.b);
    for (const Image& f : res.clip.frames) {
        std::size_t h = 0;
        for (const Rgba8& p : f.pixels()) {
            h = h * 31 + p.a + p.r * 7u;
            if (p.a) CHECK(drawn.count((std::uint32_t(p.r) << 16) | (std::uint32_t(p.g) << 8) | p.b) == 1);
        }
        distinct.insert(h);
    }
    CHECK(distinct.size() >= 4);   // the legs move
    const motion::ClipCheck chk = motion::checkClip(res.clip);
    CAPTURE(chk.issues);
    CHECK(chk.floatingFrames == 0);

    // On four drawn legs the hind and front pairs are used, and a note says so.
    auto r4 = retarget(dogSprite(), m, o);
    INFO((r4 ? std::string() : r4.error().message));
    REQUIRE(r4);
    bool unused = false;
    for (const std::string& n : r4.value().notes) unused = unused || n.find("middle legs are not used") != std::string::npos;
    CHECK(unused);
    CHECK(r4.value().clip.frames.size() == 7);
    // A four-legged walk still drives the six drawn legs (hind and front halves).
    auto r6 = retarget(ant, quad(), o);
    INFO((r6 ? std::string() : r6.error().message));
    REQUIRE(r6);
}
