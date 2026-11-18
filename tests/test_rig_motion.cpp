#include "acestudio/core/MotionLibrary.h"
#include "acestudio/core/PngCodec.h"
#include "acestudio/core/RigMotion.h"
#include "acestudio/core/RigTools.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>

using namespace acestudio;

namespace {

std::filesystem::path fixture(const char* name) {
    return std::filesystem::path(ACESTUDIO_SOURCE_DIR) / "tests" / "data" / "anim" / name;
}

// The fixture sprite on a one-frame document, auto-rigged.
Document rigged(const char* name) {
    auto sprite = readPng(fixture(name));
    REQUIRE(sprite);
    auto doc = Document::create(sprite.value().width() + 40, sprite.value().height() + 20).value();
    const LayerId hero = doc.addLayer("Hero").value();
    REQUIRE(doc.setCel(hero, 0, Cel{sprite.value(), 20, 10}));
    auto r = rigs::autoRig(doc, hero, 0, rigs::AutoRigBody::Legs);
    REQUIRE(r);
    return r.value().document;
}

mocap::Motion preset(const char* name) {
    auto m = mocap::builtInPreset(name);
    REQUIRE(m);
    return m.value();
}

const RigKey* keyOn(const Rig& rig, int frame) {
    for (const RigKey& k : rig.keys)
        if (k.frame == frame) return &k;
    return nullptr;
}

double turnOf(const Rig& rig, const RigPose& p, const char* bone) {
    const Bone* b = rig.bone(bone);
    REQUIRE(b);
    const auto t = p.turn.find(b->id);
    return t == p.turn.end() ? 0.0 : t->second;
}

} // namespace

TEST_CASE("A walk becomes keys on an auto-rigged sprite", "[rig][mocap]") {
    for (const char* sprite : {"rogue64.png", "knight32.png"}) {
        INFO(sprite);
        const Document doc = rigged(sprite);
        rigs::MotionKeysOptions o;
        o.fps = 12;
        auto r = rigs::keysFromMotion(doc, preset("walk"), o);
        REQUIRE(r);
        const rigs::MotionKeysResult& res = r.value();
        for (const auto& n : res.notes) UNSCOPED_INFO(n);
        CHECK(res.first == 1);
        CHECK(res.count == 12);
        CHECK(res.framesAdded == 12);
        const Document& d = res.document;
        CHECK(d.frameCount() == 13);
        CHECK(d.validate());
        const Rig& rig = *d.rig();
        CHECK(rig.keys.size() == 12);
        CHECK(rig.restFrame == 0);
        // Both legs and the body are driven; the leg chains reach.
        auto matched = [&](const std::string& name) {
            return std::any_of(res.matched.begin(), res.matched.end(), [&](const auto& m) { return m.first == name; });
        };
        CHECK(matched("leg0 thigh"));
        CHECK(matched("leg1 foot"));   // the rogue's legs are thigh and boot
        CHECK(matched("body"));
        CHECK(matched(rig.chains[0].name));

        // The rig's lowest leg point at rest is its ground: no key goes below it.
        const auto rest = rigs::solve(rig, RigPose{});
        double ground = -1e9;
        for (std::size_t i = 0; i < rig.bones.size(); ++i)
            if (rig.bones[i].name.rfind("leg", 0) == 0) ground = std::max({ground, rest[i].tailY, rest[i].headY});
        double lo = 1e9, hi = -1e9;
        for (const RigKey& k : rig.keys) {
            const auto w = rigs::solve(rig, k.pose);
            for (const IkChain& c : rig.chains)
                for (std::size_t i = 0; i < rig.bones.size(); ++i)
                    if (rig.bones[i].id == c.tip) CHECK(w[i].tailY <= ground + 1.0);
            const double t = turnOf(rig, k.pose, "leg0 thigh");
            lo = std::min(lo, t);
            hi = std::max(hi, t);
        }
        // Both are drawn from the front (their legs stand apart).
        CHECK(res.camera.turn == 0);
        (void)lo;
        (void)hi;

        // The keys bake into pictures.
        auto baked = rigs::bakeKeys(d);
        REQUIRE(baked);
        CHECK(baked.value().composite(5).width() == d.width());
    }
}

TEST_CASE("Seen from the side, the thighs swing", "[rig][mocap]") {
    const Document doc = rigged("knight32.png");
    rigs::MotionKeysOptions o;
    o.camera = mocap::cameraPreset("right");
    auto r = rigs::keysFromMotion(doc, preset("walk"), o);
    REQUIRE(r);
    for (const auto& n : r.value().notes) UNSCOPED_INFO(n);
    CHECK(r.value().notes.size() >= 1);   // the camera is far from the drawn view
    const Rig& rig = *r.value().document.rig();
    double lo0 = 1e9, hi0 = -1e9, lo1 = 1e9, hi1 = -1e9;
    int opposite = 0;
    for (const RigKey& k : rig.keys) {
        const double a = turnOf(rig, k.pose, "leg0 thigh"), b = turnOf(rig, k.pose, "leg1 thigh");
        lo0 = std::min(lo0, a);
        hi0 = std::max(hi0, a);
        lo1 = std::min(lo1, b);
        hi1 = std::max(hi1, b);
        if ((a - b) * (a - b) > 0.04) ++opposite;
    }
    CHECK(hi0 - lo0 > 0.3);
    CHECK(hi1 - lo1 > 0.3);
    CHECK(opposite >= 4);   // the legs are out of step
}

TEST_CASE("A jump lifts the root of a side-view rig", "[rig][mocap]") {
    const Document doc = rigged("knight32.png");
    rigs::MotionKeysOptions o;
    o.loop = false;
    o.fps = 15;
    auto r = rigs::keysFromMotion(doc, preset("jump"), o);
    REQUIRE(r);
    const Rig& rig = *r.value().document.rig();
    double minDy = 0, maxDy = 0;
    for (const RigKey& k : rig.keys) {
        minDy = std::min(minDy, k.pose.dy);
        maxDy = std::max(maxDy, k.pose.dy);
    }
    CHECK(minDy < -2);   // up in the air
    CHECK(maxDy > 0.5);  // the crouch before it
    CHECK(rig.keys.size() == 15);
}

TEST_CASE("Motion keys replace keys only in their range", "[rig][mocap]") {
    Document doc = rigged("rogue64.png");
    Rig rig = *doc.rig();
    for (int i = 0; i < 30; ++i) REQUIRE(doc.insertFrame(doc.frameCount()));
    RigPose p;
    p.dx = 3;
    REQUIRE(rigs::setKey(rig, 2, p));
    REQUIRE(rigs::setKey(rig, 25, p));
    doc.setRig(rig);
    rigs::MotionKeysOptions o;
    o.first = 1;
    o.frames = 6;
    auto r = rigs::keysFromMotion(doc, preset("idle"), o);
    REQUIRE(r);
    const Rig& out = *r.value().document.rig();
    CHECK(r.value().framesAdded == 0);
    CHECK(out.keys.size() == 7);   // 6 new, the one on frame 25 kept
    REQUIRE(keyOn(out, 25));
    CHECK(keyOn(out, 25)->pose.dx == 3);
    REQUIRE(keyOn(out, 2));
    CHECK(keyOn(out, 2)->pose.dx == 0);   // in place
    CHECK_FALSE(keyOn(out, 7));
}

TEST_CASE("Motion keys refuse what they cannot do", "[rig][mocap]") {
    const mocap::Motion walk = preset("walk");
    auto plain = Document::create(16, 16).value();
    CHECK_FALSE(rigs::keysFromMotion(plain, walk));   // no rig

    Document doc = rigged("rogue64.png");
    rigs::MotionKeysOptions o;
    o.first = 0;   // the rest frame
    CHECK_FALSE(rigs::keysFromMotion(doc, walk, o));
    o.first = -1;
    o.bones = {{"no such bone", mocap::Bone::LThigh}};
    CHECK_FALSE(rigs::keysFromMotion(doc, walk, o));

    // A rig whose bones match nothing.
    Rig tail;
    Bone b;
    b.name = "tail";
    b.length = 5;
    REQUIRE(rigs::addBone(tail, b));
    auto d2 = Document::create(16, 16).value();
    d2.setRig(tail);
    auto none = rigs::keysFromMotion(d2, walk);
    REQUIRE_FALSE(none);
    CHECK(none.error().message.find("No bone of the rig") != std::string::npos);
}

TEST_CASE("Hand-made rigs are matched by joint names or explicit pairs", "[rig][mocap]") {
    auto doc = Document::create(64, 64).value();
    Rig rig;
    auto bone = [&](const char* name, BoneId parent, double x, double y, double length, double angle) {
        Bone b;
        b.name = name;
        b.parent = parent;
        b.x = x;
        b.y = y;
        b.length = length;
        b.angle = angle;
        auto id = rigs::addBone(rig, b);
        REQUIRE(id);
        return id.value();
    };
    constexpr double kDown = 3.14159265358979323846 / 2;
    const BoneId hips = bone("pelvis", kNoBone, 32, 30, 10, -kDown);
    const BoneId thigh = bone("LThigh", hips, 32, 30, 12, kDown);
    bone("knee down", thigh, 32, 42, 12, kDown);
    REQUIRE(rigs::addChain(rig, IkChain{"leg", rig.bone("knee down")->id, 1}));
    doc.setRig(rig);

    rigs::MotionKeysOptions o;
    o.camera = mocap::cameraPreset("right");
    auto byName = rigs::keysFromMotion(doc, preset("walk"), o);
    REQUIRE(byName);
    CHECK(byName.value().matched.size() == 1);   // LThigh only: no chain without both bones

    o.bones = {{"pelvis", mocap::Bone::Hips}, {"knee down", mocap::Bone::LShin}};
    auto paired = rigs::keysFromMotion(doc, preset("walk"), o);
    REQUIRE(paired);
    CHECK(paired.value().matched.size() == 4);   // three bones and the chain
    double lo = 1e9, hi = -1e9;
    const Rig& out = *paired.value().document.rig();
    for (const RigKey& k : out.keys) {
        const double t = turnOf(out, k.pose, "LThigh");
        lo = std::min(lo, t);
        hi = std::max(hi, t);
    }
    CHECK(hi - lo > 0.3);
}

TEST_CASE("Arm boxes rig arms the analysis missed", "[rig]") {
    auto sprite = readPng(fixture("rogue64.png"));
    REQUIRE(sprite);
    auto doc = Document::create(sprite.value().width() + 40, sprite.value().height() + 20).value();
    const LayerId hero = doc.addLayer("Hero").value();
    REQUIRE(doc.setCel(hero, 0, Cel{sprite.value(), 20, 10}));
    const Image before = doc.composite(0);

    // The rogue holds a dagger up in each hand, away from the legs: the
    // analysis finds no arms.
    auto plain = rigs::autoRig(doc, hero, 0);
    REQUIRE(plain);
    CHECK_FALSE(plain.value().document.rig()->bone("arm0"));

    rigs::AutoRigOptions o;
    o.arms = {{31, 16, 14, 22}, {58, 26, 16, 22}};   // sprite (11,6) and (38,16), moved by (20,10)
    auto r = rigs::autoRig(doc, hero, 0, o);
    REQUIRE(r);
    const Document& d = r.value().document;
    const Rig& rig = *d.rig();
    CHECK(d.composite(0) == before);
    const Bone* a0 = rig.bone("arm0");
    const Bone* a1 = rig.bone("arm1");
    REQUIRE(a0);
    REQUIRE(a1);
    // Shoulders where the boxes meet the body: the inner edge of each box.
    CHECK(a0->x > 38);
    CHECK(a0->x < 46);
    CHECK(a1->x > 57);
    CHECK(a1->x < 66);
    // The left arm's blade points up and out: its bone does too.
    CHECK(std::sin(a0->angle) < 0);
    CHECK(rig.parts(a0->id).size() == 1);

    RigPose p;
    p.turn[a0->id] = 0.5;
    auto posed = rigs::bakePose(d, 0, 0, p);
    REQUIRE(posed);
    CHECK_FALSE(posed.value().composite(0) == before);

    // A motion now swings the arms too.
    auto keys = rigs::keysFromMotion(d, preset("walk"), {});
    REQUIRE(keys);
    int arms = 0;
    for (const auto& m : keys.value().matched) arms += m.first.rfind("arm", 0) == 0 ? 1 : 0;
    CHECK(arms == 2);
    // The arms are drawn raised; the walk hangs its arms. From the drawing
    // (the default) they swing about the raised pose; absolute, they drop.
    auto meanArmTurn = [&](const rigs::MotionKeysResult& res) {
        const Rig& kr = *res.document.rig();
        double sum = 0;
        for (const RigKey& k : kr.keys) sum += std::abs(turnOf(kr, k.pose, "arm0"));
        return sum / static_cast<double>(kr.keys.size());
    };
    CHECK(meanArmTurn(keys.value()) < 0.6);
    rigs::MotionKeysOptions absolute;
    absolute.armsFromDrawing = false;
    auto dropped = rigs::keysFromMotion(d, preset("walk"), absolute);
    REQUIRE(dropped);
    CHECK(meanArmTurn(dropped.value()) > 1.5);

    // Refusals.
    rigs::AutoRigOptions bad = o;
    bad.arms = {{31, 16, 14, 22}, {40, 20, 10, 10}};
    CHECK_FALSE(rigs::autoRig(doc, hero, 0, bad));   // overlapping
    bad.arms = {{0, 0, 5, 5}};
    CHECK_FALSE(rigs::autoRig(doc, hero, 0, bad));   // no pixels there
    bad.arms = {{1, 1, 1, 1}, {3, 3, 1, 1}, {5, 5, 1, 1}, {7, 7, 1, 1}, {9, 9, 1, 1}};
    CHECK_FALSE(rigs::autoRig(doc, hero, 0, bad));   // too many
    bad.arms = {{31, 16, 14, 22}, {58, 26, 0, 22}};
    CHECK_FALSE(rigs::autoRig(doc, hero, 0, bad));   // empty
    bad = o;
    bad.body = rigs::AutoRigBody::Wings;
    CHECK_FALSE(rigs::autoRig(doc, hero, 0, bad));
}
