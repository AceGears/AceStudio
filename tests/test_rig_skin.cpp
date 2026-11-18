// Parts that bend across a joint (rig skins).
#include "acestudio/core/Document.h"
#include "acestudio/core/Rig.h"
#include "acestudio/core/RigTools.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <set>
#include <vector>

using namespace acestudio;

namespace {

constexpr double kPi = 3.14159265358979323846;

Bone makeBone(const char* name, BoneId parent, double x, double y, double length, double angle) {
    Bone b;
    b.name = name;
    b.parent = parent;
    b.x = x;
    b.y = y;
    b.length = length;
    b.angle = angle;
    return b;
}

// A 40x48 canvas with one arm part: a strip 4 pixels wide from y 5 to 34,
// striped in two colours every 3 rows, on an upper arm (20,5)->(20,20) and a
// forearm (20,20)->(20,35).
struct Arm {
    Document doc = Document::create(40, 48).value();
    LayerId layer = 0;
    BoneId upper = 0, fore = 0;
};

const Rgba8 kRed{200, 40, 40, 255}, kBlue{40, 60, 200, 255};

Arm makeArm(bool skinned) {
    Arm a;
    a.layer = a.doc.addLayer("Arm").value();
    Image strip(4, 30);
    for (int y = 0; y < 30; ++y)
        for (int x = 0; x < 4; ++x) strip.setPixel(x, y, (y / 3) % 2 ? kBlue : kRed);
    REQUIRE(a.doc.setCel(a.layer, 0, Cel{strip, 18, 5}));
    REQUIRE(a.doc.insertFrame(1));
    Rig rig;
    a.upper = rigs::addBone(rig, makeBone("upper", kNoBone, 20, 5, 15, kPi / 2)).value();
    a.fore = rigs::addBone(rig, makeBone("fore", a.upper, 20, 20, 15, kPi / 2)).value();
    REQUIRE(rigs::bindLayer(rig, a.layer, a.upper));
    if (skinned) REQUIRE(rigs::skinLayer(rig, a.layer, a.fore));
    a.doc.setRig(rig);
    return a;
}

// Opaque pixels of an image as (x, y) pairs.
std::set<std::pair<int, int>> opaque(const Image& im) {
    std::set<std::pair<int, int>> s;
    for (int y = 0; y < im.height(); ++y)
        for (int x = 0; x < im.width(); ++x)
            if (im.pixel(x, y).a) s.insert({x, y});
    return s;
}

// The number of 4-connected pieces.
int pieces(const std::set<std::pair<int, int>>& px) {
    std::set<std::pair<int, int>> seen;
    int n = 0;
    for (const auto& p : px) {
        if (seen.count(p)) continue;
        ++n;
        std::vector<std::pair<int, int>> stack{p};
        seen.insert(p);
        while (!stack.empty()) {
            const auto [x, y] = stack.back();
            stack.pop_back();
            for (const auto& q : {std::pair{x + 1, y}, std::pair{x - 1, y}, std::pair{x, y + 1}, std::pair{x, y - 1}})
                if (px.count(q) && seen.insert(q).second) stack.push_back(q);
        }
    }
    return n;
}

// Transparent pixels enclosed by the part (not reachable from the border).
int holes(const Image& im) {
    std::set<std::pair<int, int>> outside;
    std::vector<std::pair<int, int>> stack;
    for (int x = 0; x < im.width(); ++x) stack.insert(stack.end(), {{x, 0}, {x, im.height() - 1}});
    for (int y = 0; y < im.height(); ++y) stack.insert(stack.end(), {{0, y}, {im.width() - 1, y}});
    while (!stack.empty()) {
        const auto [x, y] = stack.back();
        stack.pop_back();
        if (x < 0 || y < 0 || x >= im.width() || y >= im.height() || im.pixel(x, y).a || !outside.insert({x, y}).second) continue;
        stack.insert(stack.end(), {{x + 1, y}, {x - 1, y}, {x, y + 1}, {x, y - 1}});
    }
    int n = 0;
    for (int y = 0; y < im.height(); ++y)
        for (int x = 0; x < im.width(); ++x)
            if (!im.pixel(x, y).a && !outside.count({x, y})) ++n;
    return n;
}

} // namespace

TEST_CASE("A part bends across a joint without tearing", "[rig][skin]") {
    Arm skinned = makeArm(true);
    const Image rest = skinned.doc.composite(0);

    SECTION("at rest, and with both bones turned together, it is drawn exactly as a rigid part") {
        auto baked = rigs::bakePose(skinned.doc, 0, 1, RigPose{});
        REQUIRE(baked);
        CHECK(baked.value().composite(1) == rest);
        Arm rigid = makeArm(false);
        RigPose whole;
        whole.turn[skinned.upper] = 0.4;   // the forearm turns with it
        auto a = rigs::bakePose(skinned.doc, 0, 1, whole);
        auto b = rigs::bakePose(rigid.doc, 0, 1, whole);
        REQUIRE(a);
        REQUIRE(b);
        CHECK(a.value().composite(1) == b.value().composite(1));
    }

    for (double bend : {0.5, 1.2, -0.9}) {
        {   // not a SECTION: Catch2 runs a section in a loop only once
            INFO("bend " << bend);
            RigPose pose;
            pose.turn[skinned.fore] = bend;
            auto baked = rigs::bakePose(skinned.doc, 0, 1, pose);
            REQUIRE(baked);
            const Image out = baked.value().composite(1);
            const auto px = opaque(out);
            const auto src = opaque(rest);
            // One piece, no holes, about as many pixels as drawn.
            CHECK(pieces(px) == 1);
            CHECK(holes(out) == 0);
            CHECK(px.size() >= src.size() * 8 / 10);
            CHECK(px.size() <= src.size() * 13 / 10);
            // Only the sprite's colours.
            for (const auto& [x, y] : px) {
                const Rgba8 c = out.pixel(x, y);
                CHECK((c == kRed || c == kBlue));
            }
            // The upper arm, away from the joint, has not moved.
            for (int y = 5; y < 15; ++y)
                for (int x = 18; x < 22; ++x) CHECK(out.pixel(x, y) == rest.pixel(x, y));
            // The hand end follows the forearm: pixels lie near its tail.
            const double tx = 20 - 15 * std::sin(bend), ty = 20 + 15 * std::cos(bend);
            double nearest = 1e9;
            for (const auto& [x, y] : px) nearest = std::min(nearest, std::hypot(x + 0.5 - tx, y + 0.5 - ty));
            CHECK(nearest < 2.5);
            // And none lies far past it along the forearm.
            for (const auto& [x, y] : px) CHECK(std::hypot(x + 0.5 - 20, y + 0.5 - 20) < 17.5);
        }
    }

    SECTION("a rigid part on the upper arm stays straight") {
        Arm rigid = makeArm(false);
        RigPose pose;
        pose.turn[rigid.fore] = 1.2;
        auto baked = rigs::bakePose(rigid.doc, 0, 1, pose);
        REQUIRE(baked);
        CHECK(baked.value().composite(1) == rest);
    }
}

TEST_CASE("Skins are checked, kept in step with edits and saved", "[rig][skin]") {
    Rig rig;
    const BoneId hip = rigs::addBone(rig, makeBone("hip", kNoBone, 10, 5, 5, kPi / 2)).value();
    const BoneId thigh = rigs::addBone(rig, makeBone("thigh", hip, 10, 10, 10, kPi / 2)).value();
    const BoneId shin = rigs::addBone(rig, makeBone("shin", thigh, 10, 20, 10, kPi / 2)).value();
    const BoneId foot = rigs::addBone(rig, makeBone("foot", shin, 10, 30, 5, 0)).value();

    CHECK_FALSE(rigs::skinLayer(rig, 3, shin));   // not bound yet
    REQUIRE(rigs::bindLayer(rig, 3, thigh));
    CHECK_FALSE(rigs::skinLayer(rig, 3, foot));   // a grandchild
    CHECK_FALSE(rigs::skinLayer(rig, 3, hip));    // the parent
    CHECK_FALSE(rigs::skinLayer(rig, 3, shin, -1));
    CHECK(rigs::toJson(rig)["version"] == 1);
    REQUIRE(rigs::skinLayer(rig, 3, shin, 2.5));
    CHECK(rig.skins.at(3) == Skin{shin, 2.5});
    CHECK(rigs::validate(rig));

    SECTION("saved as version 3 and read back") {
        const auto j = rigs::toJson(rig);
        CHECK(j["version"] == 3);
        auto back = rigs::fromJson(j);
        REQUIRE(back);
        CHECK(back.value() == rig);
        auto broken = j;
        broken["skins"][0]["bone"] = foot;
        CHECK(rigs::fromJson(broken).error().code == ErrorCode::CorruptData);
        broken = j;
        broken["skins"].push_back(j["skins"][0]);
        CHECK(rigs::fromJson(broken).error().code == ErrorCode::CorruptData);
        broken = j;
        broken["version"] = 2;   // a version 2 reader knows no skins
        CHECK(rigs::fromJson(broken).value().skins.empty());
    }
    SECTION("removed with its child bone, its binding or by kNoBone") {
        Rig r = rig;
        REQUIRE(rigs::removeBone(r, shin));   // foot becomes the thigh's child, but the skin goes
        CHECK(r.skins.empty());
        r = rig;
        REQUIRE(rigs::bindLayer(r, 3, kNoBone));
        CHECK(r.skins.empty());
        r = rig;
        REQUIRE(rigs::bindLayer(r, 3, shin));   // rebound: the shin is no longer a child
        CHECK(r.skins.empty());
        r = rig;
        REQUIRE(rigs::reparentBone(r, shin, hip));
        CHECK(r.skins.empty());
        r = rig;
        REQUIRE(rigs::skinLayer(r, 3, kNoBone));
        CHECK(r.skins.empty());
    }
    SECTION("removed with its layer") {
        Arm a = makeArm(true);
        REQUIRE(a.doc.rig()->skins.size() == 1);
        REQUIRE(a.doc.removeLayer(a.layer));
        CHECK(a.doc.rig()->skins.empty());
        CHECK(rigs::validate(*a.doc.rig(), &a.doc));
    }
}
