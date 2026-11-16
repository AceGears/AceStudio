#include "acestudio/core/Editor.h"
#include "acestudio/core/PngCodec.h"
#include "acestudio/core/ProjectIO.h"
#include "acestudio/core/Rig.h"
#include "acestudio/core/RigTools.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <set>

using namespace acestudio;
using Catch::Approx;

namespace {

constexpr double kPi = 3.14159265358979323846;

std::filesystem::path fixture(const char* name) {
    return std::filesystem::path(ACESTUDIO_SOURCE_DIR) / "tests" / "data" / "anim" / name;
}

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

// A leg pointing down: hip (10,10) -> knee (10,20) -> ankle (10,30) -> toe (15,30).
struct Leg {
    Rig rig;
    BoneId hip = 0, thigh = 0, shin = 0, foot = 0;
};
Leg makeLeg() {
    Leg l;
    l.hip = rigs::addBone(l.rig, makeBone("hip", kNoBone, 10, 5, 5, kPi / 2)).value();
    l.thigh = rigs::addBone(l.rig, makeBone("thigh", l.hip, 10, 10, 10, kPi / 2)).value();
    l.shin = rigs::addBone(l.rig, makeBone("shin", l.thigh, 10, 20, 10, kPi / 2)).value();
    l.foot = rigs::addBone(l.rig, makeBone("foot", l.shin, 10, 30, 5, 0)).value();
    return l;
}

std::size_t indexOf(const Rig& rig, BoneId id) {
    for (std::size_t i = 0; i < rig.bones.size(); ++i)
        if (rig.bones[i].id == id) return i;
    FAIL("no bone");
    return 0;
}

} // namespace

TEST_CASE("Rig edits keep the tree valid", "[rig]") {
    Leg l = makeLeg();
    Rig& rig = l.rig;
    REQUIRE(rigs::validate(rig));
    CHECK(rig.bones.size() == 4);
    CHECK(rig.subtree(l.thigh).size() == 3);

    SECTION("names are unique and non-empty") {
        CHECK_FALSE(rigs::addBone(rig, makeBone("shin", kNoBone, 0, 0, 1, 0)));
        CHECK_FALSE(rigs::renameBone(rig, l.foot, ""));
        CHECK_FALSE(rigs::renameBone(rig, l.foot, "thigh"));
        REQUIRE(rigs::renameBone(rig, l.foot, "boot"));
        CHECK(rig.bone("boot"));
    }
    SECTION("a parent must exist") {
        CHECK_FALSE(rigs::addBone(rig, makeBone("x", 99, 0, 0, 1, 0)));
        CHECK(rig.bones.size() == 4);
    }
    SECTION("reparenting keeps the bone where it is and refuses loops") {
        const Bone before = *rig.bone(l.foot);
        REQUIRE(rigs::reparentBone(rig, l.foot, l.hip));
        CHECK(rig.bone(l.foot)->parent == l.hip);
        CHECK(rig.bone(l.foot)->x == before.x);
        CHECK(rig.bone(l.foot)->angle == before.angle);
        CHECK_FALSE(rigs::reparentBone(rig, l.thigh, l.shin));   // below itself
        CHECK_FALSE(rigs::reparentBone(rig, l.thigh, l.thigh));
        // Making a root a child of a later bone re-sorts parents first.
        Rig r2 = makeLeg().rig;
        const BoneId other = rigs::addBone(r2, makeBone("other", kNoBone, 0, 0, 1, 0)).value();
        REQUIRE(rigs::reparentBone(r2, 1, other));
        CHECK(rigs::validate(r2));
        CHECK(indexOf(r2, other) < indexOf(r2, 1));
    }
    SECTION("removing a bone hands its children and parts to its parent") {
        REQUIRE(rigs::bindLayer(rig, 7, l.shin));
        REQUIRE(rigs::addChain(rig, IkChain{"leg", l.shin, 1}));
        REQUIRE(rigs::removeBone(rig, l.shin));
        CHECK_FALSE(rig.bone(l.shin));
        CHECK(rig.bone(l.foot)->parent == l.thigh);
        CHECK(rig.bindings.at(7) == l.thigh);
        CHECK(rig.chains.empty());
        // A root's parts are unbound.
        REQUIRE(rigs::bindLayer(rig, 8, l.hip));
        REQUIRE(rigs::removeBone(rig, l.hip));
        CHECK_FALSE(rig.bindings.count(8));
        CHECK(rig.bone(l.thigh)->parent == kNoBone);
        CHECK(rigs::validate(rig));
    }
    SECTION("moving a bone can carry its children") {
        REQUIRE(rigs::setBoneGeometry(rig, l.thigh, 10, 10, 10, 0, true));   // now points right
        CHECK(rig.bone(l.shin)->x == Approx(20));
        CHECK(rig.bone(l.shin)->y == Approx(10));
        CHECK(rig.bone(l.shin)->angle == Approx(0));
        REQUIRE(rigs::setBoneGeometry(rig, l.thigh, 12, 10, 10, 0, false));
        CHECK(rig.bone(l.shin)->x == Approx(20));
        CHECK_FALSE(rigs::setBoneGeometry(rig, l.thigh, 12, 10, -1, 0, false));
        CHECK_FALSE(rigs::setBoneGeometry(rig, l.thigh, NAN, 10, 1, 0, false));
    }
    SECTION("limits, chains") {
        CHECK_FALSE(rigs::setLimits(rig, l.shin, true, 1, -1));
        REQUIRE(rigs::setLimits(rig, l.shin, true, -1, 0.5));
        CHECK_FALSE(rigs::addChain(rig, IkChain{"root", l.hip, 1}));   // needs a parent
        CHECK_FALSE(rigs::addChain(rig, IkChain{"bad", l.shin, 0}));
        REQUIRE(rigs::addChain(rig, IkChain{"leg", l.shin, 1}));
        CHECK_FALSE(rigs::addChain(rig, IkChain{"again", l.shin, 1}));
        // A chain whose tip becomes a root goes.
        REQUIRE(rigs::reparentBone(rig, l.shin, kNoBone));
        CHECK(rig.chains.empty());
    }
}

TEST_CASE("Forward kinematics: children follow, limits and locks hold", "[rig]") {
    Leg l = makeLeg();
    RigPose pose;
    pose.turn[l.thigh] = kPi / 2;   // clockwise: the leg swings to point left
    auto w = rigs::solve(l.rig, pose);
    const auto& shin = w[indexOf(l.rig, l.shin)];
    CHECK(shin.headX == Approx(0));
    CHECK(shin.headY == Approx(10));
    CHECK(shin.tailX == Approx(-10));
    CHECK(shin.turn == Approx(kPi / 2));
    const auto& foot = w[indexOf(l.rig, l.foot)];
    CHECK(foot.headX == Approx(-10));
    CHECK(foot.tailY == Approx(15));   // the foot (pointing right) now points down

    pose.dx = 3;
    pose.dy = -2;
    w = rigs::solve(l.rig, pose);
    CHECK(w[indexOf(l.rig, l.hip)].headX == Approx(13));
    CHECK(w[indexOf(l.rig, l.shin)].headY == Approx(8));

    REQUIRE(rigs::setLimits(l.rig, l.thigh, true, -0.5, 0.25));
    w = rigs::solve(l.rig, pose);
    CHECK(w[indexOf(l.rig, l.thigh)].turn == Approx(0.25));
    REQUIRE(rigs::setLocked(l.rig, l.thigh, true));
    w = rigs::solve(l.rig, pose);
    CHECK(w[indexOf(l.rig, l.thigh)].turn == Approx(0));
}

TEST_CASE("Two-bone IK reaches, bends to its side and respects limits", "[rig]") {
    Leg l = makeLeg();
    REQUIRE(rigs::addChain(l.rig, IkChain{"leg", l.shin, 1}));
    auto ankle = [&](const RigPose& p) {
        const auto w = rigs::solve(l.rig, p);
        return w[indexOf(l.rig, l.shin)];
    };
    RigPose pose;
    REQUIRE(rigs::reach(l.rig, pose, 0, 18, 24));
    auto a = ankle(pose);
    CHECK(a.tailX == Approx(18).margin(1e-6));
    CHECK(a.tailY == Approx(24).margin(1e-6));
    // bend +1: the knee lies clockwise of hip->target (to its left here).
    const double cross = (18 - 10) * (a.headY - 10) - (24 - 10) * (a.headX - 10);
    CHECK(cross > 0);
    RigPose other;
    l.rig.chains[0].bend = -1;
    REQUIRE(rigs::reach(l.rig, other, 0, 18, 24));
    a = ankle(other);
    CHECK(a.tailX == Approx(18).margin(1e-6));
    CHECK((18 - 10) * (a.headY - 10) - (24 - 10) * (a.headX - 10) < 0);

    // Out of reach: the leg stretches straight toward the target.
    RigPose far;
    REQUIRE(rigs::reach(l.rig, far, 0, 10, 100));
    a = ankle(far);
    CHECK(a.tailX == Approx(10).margin(1e-3));
    CHECK(a.tailY == Approx(30).margin(1e-3));
    // The hip's own turn is kept and followed.
    RigPose turned;
    turned.turn[l.hip] = 0.3;
    REQUIRE(rigs::reach(l.rig, turned, 0, 12, 26));
    CHECK(turned.turn[l.hip] == 0.3);
    a = ankle(turned);
    CHECK(a.tailX == Approx(12).margin(1e-6));
    CHECK(a.tailY == Approx(26).margin(1e-6));
    // A limited knee stops short.
    REQUIRE(rigs::setLimits(l.rig, l.shin, true, -0.1, 0.1));
    RigPose limited;
    REQUIRE(rigs::reach(l.rig, limited, 0, 18, 18));
    CHECK(std::abs(limited.turn[l.shin]) <= 0.1 + 1e-12);
    CHECK_FALSE(rigs::reach(l.rig, limited, 5, 0, 0));
    CHECK_FALSE(rigs::reach(l.rig, limited, 0, NAN, 0));
}

TEST_CASE("Rig JSON round trip and refusals", "[rig]") {
    Leg l = makeLeg();
    REQUIRE(rigs::setLimits(l.rig, l.shin, true, -1, 0.5));
    REQUIRE(rigs::setLocked(l.rig, l.hip, true));
    REQUIRE(rigs::bindLayer(l.rig, 3, l.thigh));
    REQUIRE(rigs::addChain(l.rig, IkChain{"leg", l.shin, -1}));
    const auto j = rigs::toJson(l.rig);
    auto back = rigs::fromJson(j);
    REQUIRE(back);
    CHECK(back.value() == l.rig);

    auto broken = j;
    SECTION("newer version") {
        broken["version"] = Rig::kVersion + 1;
        CHECK(rigs::fromJson(broken).error().code == ErrorCode::UnsupportedVersion);
    }
    SECTION("a loop") {
        broken["bones"][0]["parent"] = l.foot;
        CHECK(rigs::fromJson(broken).error().code == ErrorCode::CorruptData);
    }
    SECTION("ids at or past nextId") {
        broken["nextId"] = 2;
        CHECK(rigs::fromJson(broken).error().code == ErrorCode::CorruptData);
    }
    SECTION("wrong types") {
        broken["bones"][1]["x"] = "ten";
        CHECK_FALSE(rigs::fromJson(broken));
        broken = j;
        broken["chains"][0]["bend"] = 2;
        CHECK_FALSE(rigs::fromJson(broken));
        broken = j;
        broken["bindings"].push_back({{"layer", 3}, {"bone", l.foot}});
        CHECK_FALSE(rigs::fromJson(broken));
    }
}

TEST_CASE("Projects save a rig as schema 4 and only then", "[rig][project]") {
    auto doc = Document::create(16, 16).value();
    const LayerId arm = doc.addLayer("Arm").value();
    const LayerId g = doc.addLayer("G", LayerKind::Group).value();
    auto plain = serializeProject(doc).value();
    ProjectInfo info;
    REQUIRE(deserializeProject(plain, &info));
    CHECK(info.schemaVersion < 4);

    Rig rig;
    const BoneId b = rigs::addBone(rig, makeBone("arm", kNoBone, 4, 4, 6, 0)).value();
    REQUIRE(rigs::bindLayer(rig, arm, b));
    doc.setRig(rig);
    REQUIRE(doc.validate());
    auto bytes = serializeProject(doc).value();
    auto back = deserializeProject(bytes, &info);
    REQUIRE(back);
    CHECK(info.schemaVersion == 4);
    REQUIRE(back.value().rig());
    CHECK(*back.value().rig() == rig);

    // Bindings must name raster layers of the document.
    Rig onGroup = rig;
    REQUIRE(rigs::bindLayer(onGroup, g, b));
    doc.setRig(onGroup);
    CHECK_FALSE(doc.validate());
    doc.setRig(rig);
    // Removing a part layer drops its binding.
    REQUIRE(doc.removeLayer(arm));
    CHECK(doc.rig()->bindings.empty());
    CHECK(doc.validate());
}

TEST_CASE("Editor rig changes undo, redo and merge", "[rig][editor]") {
    Editor ed(Document::create(16, 16).value());
    Rig rig;
    const BoneId b = rigs::addBone(rig, makeBone("arm", kNoBone, 4, 4, 6, 0)).value();
    REQUIRE(ed.setRig(rig, "Add Bone"));
    REQUIRE(ed.document().rig());
    // Unchanged: no step.
    const auto steps = ed.history().index();
    REQUIRE(ed.setRig(rig, "Same"));
    CHECK(ed.history().index() == steps);
    // A drag merges.
    for (int i = 1; i <= 5; ++i) {
        Rig r = *ed.document().rig();
        REQUIRE(rigs::setBoneGeometry(r, b, 4 + i, 4, 6, 0, true));
        REQUIRE(ed.setRig(r, "Move Bone", 42));
    }
    CHECK(ed.history().index() == steps + 1);
    CHECK(ed.document().rig()->bone(b)->x == 9);
    REQUIRE(ed.undo());
    CHECK(ed.document().rig()->bone(b)->x == 4);
    REQUIRE(ed.undo());
    CHECK_FALSE(ed.document().rig());
    REQUIRE(ed.redo());
    CHECK(ed.document().rig());
    // Deleting a part layer unbinds it; undo binds it again.
    const LayerId part = ed.addLayer("Arm").value();
    Rig bound = *ed.document().rig();
    REQUIRE(rigs::bindLayer(bound, part, b));
    REQUIRE(ed.setRig(bound, "Bind"));
    REQUIRE(ed.removeLayer(part));
    CHECK(ed.document().rig()->bindings.empty());
    REQUIRE(ed.undo());
    CHECK(ed.document().rig()->bindings.at(part) == b);
    REQUIRE(ed.redo());
    CHECK(ed.document().rig()->bindings.empty());
    // Invalid rigs are refused.
    Rig bad = rig;
    REQUIRE(rigs::bindLayer(bad, 77, b));
    CHECK_FALSE(ed.setRig(bad, "Bind"));
    REQUIRE(ed.setRig(std::nullopt, "Remove Rig"));
    CHECK_FALSE(ed.document().rig());
}

TEST_CASE("Auto-rig cuts a sprite into bound part layers", "[rig]") {
    auto sprite = readPng(fixture("rogue64.png"));
    REQUIRE(sprite);
    auto doc = Document::create(sprite.value().width() + 20, sprite.value().height() + 10).value();
    const LayerId bg = doc.addLayer("Background").value();
    REQUIRE(doc.setCel(bg, 0, Cel{Image(4, 4, Rgba8::opaque(9, 9, 9)), 0, 0}));
    const LayerId hero = doc.addLayer("Hero").value();
    REQUIRE(doc.setCel(hero, 0, Cel{sprite.value(), 12, 6}));
    const Image before = doc.composite(0);

    auto r = rigs::autoRig(doc, hero, 0);
    REQUIRE(r);
    for (const auto& n : r.value().notes) UNSCOPED_INFO(n);
    const Document& d = r.value().document;
    REQUIRE(d.rig());
    const Rig& rig = *d.rig();
    // The parts put back together are the sprite, pixel for pixel.
    CHECK(d.composite(0) == before);
    CHECK_FALSE(d.layer(hero)->visible);
    CHECK(d.layer(r.value().group)->kind == LayerKind::Group);
    CHECK(*d.layerIndex(r.value().group) == *d.layerIndex(hero) + 1);
    CHECK(rig.bone("body"));
    CHECK(rig.bone("leg0 thigh"));
    CHECK(rig.bone("leg1 thigh"));
    CHECK(rig.chains.size() == 2);
    CHECK(rig.bindings.size() >= 5);
    for (const auto& [layer, bone] : rig.bindings) CHECK(d.layer(layer)->parent == r.value().group);
    CHECK(d.validate());

    // Again: refused, there is a rig.
    CHECK_FALSE(rigs::autoRig(d, hero, 0));
    // Saved and loaded intact.
    auto back = deserializeProject(serializeProject(d).value());
    REQUIRE(back);
    CHECK(back.value().composite(0) == before);

    SECTION("baking the rest pose changes nothing") {
        auto baked = rigs::bakePose(d, 0, 0, RigPose{});
        REQUIRE(baked);
        CHECK(baked.value().composite(0) == before);
    }
    SECTION("a turned thigh moves its leg and nothing else") {
        RigPose pose;
        const BoneId thigh = rig.bone("leg0 thigh")->id;
        pose.turn[thigh] = 0.6;
        auto baked = rigs::bakePose(d, 0, 0, pose);
        REQUIRE(baked);
        CHECK_FALSE(baked.value().composite(0) == before);
        const LayerId bodyLayer = rig.parts(rig.bone("body")->id).front();
        const Cel* a = d.cel(bodyLayer, 0);
        const Cel* b = baked.value().cel(bodyLayer, 0);
        REQUIRE(a);
        REQUIRE(b);
        CHECK(a->image == b->image);
        CHECK(a->x == b->x);
        // The thigh's own pixels are kept (no colours made up).
        const LayerId thighLayer = rig.parts(thigh).front();
        std::set<std::uint32_t> colours, turned;
        auto key = [](Rgba8 c) { return std::uint32_t(c.r) << 24 | std::uint32_t(c.g) << 16 | std::uint32_t(c.b) << 8 | c.a; };
        const Image& src = d.cel(thighLayer, 0)->image;
        const Image& dst = baked.value().cel(thighLayer, 0)->image;
        for (int y = 0; y < src.height(); ++y)
            for (int x = 0; x < src.width(); ++x) colours.insert(key(src.pixel(x, y)));
        for (int y = 0; y < dst.height(); ++y)
            for (int x = 0; x < dst.width(); ++x) turned.insert(key(dst.pixel(x, y)));
        for (std::uint32_t c : turned) CHECK(colours.count(c));
    }
    SECTION("baking into another frame reads the rest frame, so it never compounds") {
        Document two = d;
        REQUIRE(two.insertFrame(1));
        RigPose pose;
        pose.turn[rig.bone("leg1 thigh")->id] = -0.5;
        auto once = rigs::bakePose(two, 0, 1, pose);
        REQUIRE(once);
        auto twice = rigs::bakePose(once.value(), 0, 1, pose);
        REQUIRE(twice);
        CHECK(once.value().composite(0) == before);   // the rest frame is untouched
        CHECK_FALSE(once.value().composite(1) == before);
        CHECK(twice.value().composite(1) == once.value().composite(1));
        // The rest pose copies the rest cels.
        auto rest = rigs::bakePose(two, 0, 1, RigPose{});
        REQUIRE(rest);
        for (const auto& [layer, bone] : rig.bindings) {
            const Cel* a = two.cel(layer, 0);
            const Cel* b = rest.value().cel(layer, 1);
            REQUIRE(b);
            CHECK(a->image == b->image);
            CHECK(a->x == b->x);
            CHECK(a->y == b->y);
        }
        CHECK_FALSE(rigs::bakePose(two, 0, 2, pose));
        CHECK_FALSE(rigs::bakePose(two, -1, 1, pose));
    }
    SECTION("moving the root moves every bound part") {
        RigPose pose;
        pose.dx = 3;
        auto baked = rigs::bakePose(d, 0, 0, pose);
        REQUIRE(baked);
        for (const auto& [layer, bone] : rig.bindings) {
            const Cel* a = d.cel(layer, 0);
            const Cel* b = baked.value().cel(layer, 0);
            REQUIRE(b);
            CHECK(b->x == a->x + 3);
            CHECK(b->image == a->image);
        }
    }
}

TEST_CASE("Auto-rig refuses bodies without legs", "[rig]") {
    auto doc = Document::create(32, 32).value();
    const LayerId blob = doc.addLayer("Blob").value();
    REQUIRE(doc.setCel(blob, 0, Cel{Image(12, 12, Rgba8::opaque(200, 40, 40)), 10, 10}));
    auto r = rigs::autoRig(doc, blob, 0);
    REQUIRE_FALSE(r);
    CHECK(r.error().message.find("legs") != std::string::npos);
    CHECK_FALSE(rigs::autoRig(doc, blob, 3));
    CHECK_FALSE(rigs::bakePose(doc, 0, 0, RigPose{}));
}
