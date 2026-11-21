#include "acestudio/core/MotionLibrary.h"
#include "acestudio/core/PngCodec.h"
#include "acestudio/core/RigMotion.h"
#include "acestudio/core/RigTools.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <set>
#include <map>
#include <string>
#include <utility>
#include <vector>

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

TEST_CASE("A four-legged motion walks an auto-rigged four-legged sprite", "[rig][mocap][creature]") {
    const Document doc = rigged("dog72.png");
    rigs::MotionKeysOptions o;
    o.fps = 10;
    auto r = rigs::keysFromMotion(doc, preset("quad-walk"), o);
    REQUIRE(r);
    const rigs::MotionKeysResult& res = r.value();
    for (const auto& n : res.notes) UNSCOPED_INFO(n);
    CHECK(res.count == 12);   // 1.2 s at 10 fps
    // The dog faces right (its head is the highest art), so the legs on
    // the left are hind legs; front pairs with front, hind with hind.
    CHECK(res.camera.turn == 90);
    int hind = 0, front = 0;
    bool body = false;
    for (const auto& [bone, what] : res.matched) {
        CAPTURE(bone, what);
        if (bone.find("thigh") != std::string::npos) {
            const bool h = what.rfind("hind", 0) == 0;
            (h ? hind : front) += 1;
            CHECK((bone == "leg0 thigh" || bone == "leg1 thigh") == h);
        }
        body = body || (bone == "body" && what.rfind("back", 0) == 0);
    }
    CHECK(hind == 2);
    CHECK(front == 2);
    CHECK(body);

    const Document& d = res.document;
    REQUIRE(d.validate());
    const Rig& rig = *d.rig();
    const auto rest = rigs::solve(rig, RigPose{});
    double ground = -1e9;
    for (std::size_t i = 0; i < rig.bones.size(); ++i)
        if (rig.bones[i].name.rfind("leg", 0) == 0) ground = std::max({ground, rest[i].tailY, rest[i].headY});
    // Every leg swings, no foot goes below the ground, and each foot is
    // lifted in some key and on the ground in another.
    for (const char* leg : {"leg0", "leg1", "leg2", "leg3"}) {
        INFO(leg);
        const std::string thigh = std::string(leg) + " thigh", shin = std::string(leg) + " shin";
        double lo = 1e9, hi = -1e9, footHi = 1e9, footLo = -1e9;
        std::size_t shinIdx = 0;
        for (std::size_t i = 0; i < rig.bones.size(); ++i)
            if (rig.bones[i].name == shin) shinIdx = i;
        for (const RigKey& k : rig.keys) {
            const double t = turnOf(rig, k.pose, thigh.c_str());
            lo = std::min(lo, t);
            hi = std::max(hi, t);
            const auto w = rigs::solve(rig, k.pose);
            CHECK(w[shinIdx].tailY <= ground + 1.0);
            footHi = std::min(footHi, w[shinIdx].tailY);
            footLo = std::max(footLo, w[shinIdx].tailY);
        }
        CHECK(hi - lo > 0.2);
        CHECK(footLo - footHi >= 1.5);
    }
    // The body stays near its drawn height (no crouch): the root moves at
    // most a sixth of the legs' length.
    for (const RigKey& k : rig.keys) CHECK(std::abs(k.pose.dy) <= 3.0);
    auto baked = rigs::bakeKeys(d);
    REQUIRE(baked);

    // Bone pairs name humanoid joints: refused with a four-legged motion.
    rigs::MotionKeysOptions pairs = o;
    pairs.bones = {{"leg0 thigh", mocap::Bone::LThigh}};
    CHECK_FALSE(rigs::keysFromMotion(doc, preset("quad-walk"), pairs));
}

TEST_CASE("Four-legged rig motion: level feet, facing and leg pairs by hand", "[rig][mocap][creature]") {
    const Document doc = rigged("dog72.png");
    rigs::MotionKeysOptions o;
    o.fps = 10;
    auto r = rigs::keysFromMotion(doc, preset("quad-walk"), o);
    REQUIRE(r);
    const Rig& rig = *r.value().document.rig();
    // Every foot keeps its drawn angle in every key.
    for (const RigKey& k : rig.keys) {
        const auto w = rigs::solve(rig, k.pose);
        for (std::size_t i = 0; i < rig.bones.size(); ++i)
            if (rig.bones[i].name.find(" foot") != std::string::npos) {
                CAPTURE(rig.bones[i].name, k.frame);
                CHECK(std::abs(std::remainder(w[i].turn, 2 * 3.14159265358979)) < 1e-6);
            }
    }

    auto pairOf = [](const rigs::MotionKeysResult& res, const std::string& bone) {
        for (const auto& [b, what] : res.matched)
            if (b == bone) return what;
        return std::string();
    };
    // Facing left: the legs on the left are now the front legs.
    rigs::MotionKeysOptions left = o;
    left.facing = -1;
    auto rl = rigs::keysFromMotion(doc, preset("quad-walk"), left);
    REQUIRE(rl);
    CHECK(rl.value().camera.turn == -90);
    CHECK(pairOf(rl.value(), "leg0 thigh").rfind("front", 0) == 0);
    CHECK(pairOf(rl.value(), "leg3 thigh").rfind("hind", 0) == 0);

    // Leg pairs by hand win over the pairing by position.
    using QL = rigs::MotionKeysOptions::QuadLeg;
    rigs::MotionKeysOptions hand = o;
    hand.legs = {{0, QL::FrontFar}, {3, QL::HindNear}};
    auto rh = rigs::keysFromMotion(doc, preset("quad-walk"), hand);
    REQUIRE(rh);
    CHECK(pairOf(rh.value(), "leg0 thigh").rfind("front", 0) == 0);
    CHECK(pairOf(rh.value(), "leg3 thigh").rfind("hind", 0) == 0);
    CHECK(pairOf(rh.value(), "leg1 thigh").rfind("hind", 0) == 0);   // unchanged
    // Near/far: the near and far front legs follow different source legs.
    hand.legs = {{2, QL::FrontNear}, {3, QL::FrontFar}};
    auto rn = rigs::keysFromMotion(doc, preset("quad-walk"), hand);
    REQUIRE(rn);
    CHECK(pairOf(rn.value(), "leg2 thigh") != pairOf(rn.value(), "leg3 thigh"));
    hand.legs = {{2, QL::FrontFar}, {3, QL::FrontNear}};
    auto rf = rigs::keysFromMotion(doc, preset("quad-walk"), hand);
    REQUIRE(rf);
    CHECK(pairOf(rf.value(), "leg2 thigh") == pairOf(rn.value(), "leg3 thigh"));

    // A leg the rig does not have, and leg pairs with a humanoid motion, are refused.
    hand.legs = {{7, QL::HindNear}};
    CHECK_FALSE(rigs::keysFromMotion(doc, preset("quad-walk"), hand));
    hand.legs = {{0, QL::HindNear}};
    CHECK_FALSE(rigs::keysFromMotion(doc, preset("walk"), hand));
}

TEST_CASE("Auto-rig cuts a four-legged sprite's head and tail, and a motion nods and swings them", "[rig][mocap][creature]") {
    const Document doc = rigged("dog72.png");
    const Rig& rig = *doc.rig();
    const Bone* head = rig.bone("head");
    const Bone* tail = rig.bone("tail");
    REQUIRE(head);
    REQUIRE(tail);
    CHECK(head->parent == rig.bones.front().id);   // on the body
    CHECK(tail->parent == rig.bones.front().id);
    CHECK(head->x > tail->x);                      // the dog faces right
    // The nose (the darkest colour) is all in the head's layer, the tail's
    // colour in the tail's; the body keeps neither.
    const Rgba8 nose{30, 20, 20, 255}, tailColour{90, 60, 35, 255};
    auto layerOf = [&](BoneId b) {
        for (const auto& [l, bone] : rig.bindings)
            if (bone == b) return l;
        return kNoLayer;
    };
    auto count = [&](LayerId l, Rgba8 c) {
        const Cel* cel = doc.cel(l, 0);
        int n = 0;
        if (cel)
            for (const Rgba8& p : cel->image.pixels()) n += p == c ? 1 : 0;
        return n;
    };
    const LayerId headL = layerOf(head->id), tailL = layerOf(tail->id), bodyL = layerOf(rig.bones.front().id);
    REQUIRE(headL != kNoLayer);
    REQUIRE(tailL != kNoLayer);
    CHECK(count(headL, nose) == 16);       // the 4x4 nose
    CHECK(count(bodyL, nose) == 0);
    CHECK(count(tailL, tailColour) >= 14);  // the 7x3 tail, its root column may stay on the body
    // Nothing is lost: the parts together hold the sprite's pixels.
    auto sprite = readPng(fixture("dog72.png"));
    REQUIRE(sprite);
    int drawn = 0, parts = 0;
    for (const Rgba8& p : sprite.value().pixels()) drawn += p.a ? 1 : 0;
    for (const auto& [l, b] : rig.bindings)
        if (const Cel* c = doc.cel(l, 0))
            for (const Rgba8& p : c->image.pixels()) parts += p.a ? 1 : 0;
    CHECK(parts >= drawn);

    // A four-legged motion turns them by the motion's head and tail.
    rigs::MotionKeysOptions o;
    o.fps = 10;
    auto r = rigs::keysFromMotion(doc, preset("quad-walk"), o);
    REQUIRE(r);
    bool headMatched = false, tailMatched = false;
    for (const auto& [b, what] : r.value().matched) {
        headMatched = headMatched || (b == "head" && what.rfind("head", 0) == 0);
        tailMatched = tailMatched || (b == "tail" && what.rfind("tail", 0) == 0);
    }
    CHECK(headMatched);
    CHECK(tailMatched);
    const Rig& keyed = *r.value().document.rig();
    for (const char* bone : {"head", "tail"}) {
        INFO(bone);
        double lo = 1e9, hi = -1e9;
        for (const RigKey& k : keyed.keys) {
            const double t = turnOf(keyed, k.pose, bone);
            lo = std::min(lo, t);
            hi = std::max(hi, t);
        }
        CHECK(hi - lo > 0.05);   // it moves
        CHECK(hi - lo < 1.0);    // a nod and a swing, not a spin
    }
    CHECK(rigs::bakeKeys(r.value().document));
}

namespace {

// A four-legged animal with a long neck and a long tail, facing right.
Image longEnds() {
    Image s(96, 56);
    const Rgba8 coat{150, 110, 60, 255}, dark{70, 45, 25, 255};
    s.fillRect({24, 22, 40, 12}, coat);                              // body
    for (int x : {26, 33, 52, 59}) s.fillRect({x, 34, 4, 20}, coat);  // thin legs (a twelfth of the height, not of the width)
    s.fillRect({6, 22, 18, 3}, dark);                                // tail
    for (int i = 0; i < 12; ++i) s.fillRect({64 + i, 22 - i, 1, 6}, coat);   // neck, rising
    s.fillRect({74, 6, 10, 7}, coat);                                // head
    return s;
}

} // namespace

TEST_CASE("A long neck and a long tail get a second bone and curve with a four-legged motion", "[rig][mocap][creature]") {
    const Image sprite = longEnds();
    auto base = Document::create(sprite.width() + 40, sprite.height() + 20).value();
    const LayerId hero = base.addLayer("Hero").value();
    REQUIRE(base.setCel(hero, 0, Cel{sprite, 20, 10}));
    auto rigRes = rigs::autoRig(base, hero, 0, rigs::AutoRigBody::Legs);
    INFO((rigRes ? std::string() : rigRes.error().message));
    REQUIRE(rigRes);
    const Document doc = rigRes.value().document;
    const Rig& rig = *doc.rig();
    const Bone* neck = rig.bone("neck");
    const Bone* head = rig.bone("head");
    const Bone* tail = rig.bone("tail");
    const Bone* tip = rig.bone("tail tip");
    REQUIRE(neck);
    REQUIRE(head);
    REQUIRE(tail);
    REQUIRE(tip);
    CHECK(head->parent == neck->id);
    CHECK(tip->parent == tail->id);
    CHECK(neck->x > tail->x);   // it faces right
    // Each end is one part, bound to the near bone and bending with the far one.
    int skinned = 0;
    for (const auto& [layer, skin] : rig.skins) {
        const BoneId own = rig.bindings.at(layer);
        if ((own == neck->id && skin.bone == head->id) || (own == tail->id && skin.bone == tip->id)) ++skinned;
    }
    CHECK(skinned == 2);
    // Nothing is lost.
    int drawn = 0, parts = 0;
    for (const Rgba8& p : sprite.pixels()) drawn += p.a ? 1 : 0;
    for (const auto& [l, b] : rig.bindings)
        if (const Cel* c = doc.cel(l, 0))
            for (const Rgba8& p : c->image.pixels()) parts += p.a ? 1 : 0;
    CHECK(parts == drawn);

    rigs::MotionKeysOptions o;
    o.fps = 10;
    auto r = rigs::keysFromMotion(doc, preset("quad-walk"), o);
    REQUIRE(r);
    std::map<std::string, std::string> what(r.value().matched.begin(), r.value().matched.end());
    CHECK(what["neck"].rfind("neck", 0) == 0);
    CHECK(what["head"].find("against the neck") != std::string::npos);
    CHECK(what["tail tip"].find("trails") != std::string::npos);
    const Rig& keyed = *r.value().document.rig();
    REQUIRE(keyed.keys.size() >= 6);
    double tipLo = 1e9, tipHi = -1e9, tailLo = 1e9, tailHi = -1e9;
    bool sameShape = true;
    for (const RigKey& k : keyed.keys) {
        const double n = turnOf(keyed, k.pose, "neck"), h = turnOf(keyed, k.pose, "head");
        CHECK(std::abs(h + 0.5 * n) < 1e-6);   // the head goes half-way back
        const double t = turnOf(keyed, k.pose, "tail"), u = turnOf(keyed, k.pose, "tail tip");
        tipLo = std::min(tipLo, u);
        tipHi = std::max(tipHi, u);
        tailLo = std::min(tailLo, t);
        tailHi = std::max(tailHi, t);
        if (std::abs(u - 0.8 * t) > 0.01) sameShape = false;
    }
    CHECK(tailHi - tailLo > 0.05);
    CHECK(tipHi - tipLo > 0.04);        // the tip swings too
    CHECK(tipHi - tipLo < 1.0);
    CHECK_FALSE(sameShape);             // later than the tail, not with it
    CHECK(rigs::bakeKeys(r.value().document));
    // dog72's short ends stay one bone each.
    CHECK(rigged("dog72.png").rig()->bone("neck") == nullptr);
    CHECK(rigged("dog72.png").rig()->bone("tail tip") == nullptr);
}

TEST_CASE("A tail swung sideways is drawn shorter while it points at the viewer", "[rig][mocap][creature]") {
    const Image sprite = longEnds();
    auto base = Document::create(sprite.width() + 40, sprite.height() + 20).value();
    const LayerId hero = base.addLayer("Hero").value();
    REQUIRE(base.setCel(hero, 0, Cel{sprite, 20, 10}));
    auto rigRes = rigs::autoRig(base, hero, 0, rigs::AutoRigBody::Legs);
    REQUIRE(rigRes);
    const Document doc = rigRes.value().document;
    const Rig& rig = *doc.rig();
    const Bone* tail = rig.bone("tail");
    REQUIRE(tail);
    LayerId tailLayer = kNoLayer;
    for (const auto& [layer, bone] : rig.bindings)
        if (bone == tail->id) tailLayer = layer;
    REQUIRE(tailLayer != kNoLayer);

    // The walk with a wide wag: the tail's own sideways turn, once a stride.
    mocap::Motion wag = preset("quad-walk");
    const int tj = wag.find("Tail");
    REQUIRE(tj >= 0);
    REQUIRE(wag.joints[static_cast<std::size_t>(tj)].channels.size() == 3);
    REQUIRE(wag.joints[static_cast<std::size_t>(tj)].channels[2] == mocap::Channel::Yrotation);
    for (int f = 0; f < wag.frameCount; ++f)
        wag.values[static_cast<std::size_t>(f * wag.channelCount + wag.joints[static_cast<std::size_t>(tj)].firstChannel + 2)] =
            60 * std::sin(2 * 3.14159265358979323846 * f / wag.frameCount);

    rigs::MotionKeysOptions o;
    o.fps = 10;
    auto r = rigs::keysFromMotion(doc, wag, o);
    REQUIRE(r);
    const Rig& keyed = *r.value().document.rig();
    double lo = 1e9, hi = -1e9;
    int shortest = -1;
    for (const RigKey& k : keyed.keys) {
        const auto s = k.pose.stretch.find(tail->id);
        const double v = s == k.pose.stretch.end() ? 1.0 : s->second;
        if (v < lo) {
            lo = v;
            shortest = k.frame;
        }
        hi = std::max(hi, v);
        CHECK(k.pose.stretch.count(rig.bone("tail tip")->id) == 0);   // the tip follows its tail
        for (const auto& [id, t] : k.pose.stretch) CHECK((id == tail->id || id == rig.bone("neck")->id));
    }
    CHECK(lo < 0.9);     // pointing well toward or away from the viewer
    CHECK(lo >= 0.25);
    CHECK(hi > 0.97);    // side-on at some moment: as drawn
    CHECK(hi <= 1.0);
    bool said = false;
    for (const std::string& n : r.value().notes) said = said || n.find("tail swings toward or away") != std::string::npos;
    CHECK(said);
    CHECK(rigs::toJson(keyed)["version"] == 4);

    // Baked: the tail (drawn level, 18 px) is narrower on its shortest frame.
    auto baked = rigs::bakeKeys(r.value().document);
    REQUIRE(baked);
    const Cel* rest = doc.cel(tailLayer, 0);
    const Cel* drawn = baked.value().cel(tailLayer, shortest);
    REQUIRE(rest);
    REQUIRE(drawn);
    CHECK(drawn->image.opaqueBounds().w + 2 <= rest->image.opaqueBounds().w);

    // Half the amount, half the shortening; none of it with the plain walk's small swing held under the 2% floor.
    o.amount = 0.5;
    auto h = rigs::keysFromMotion(doc, wag, o);
    REQUIRE(h);
    double lo2 = 1;
    for (const RigKey& k : h.value().document.rig()->keys)
        if (auto s = k.pose.stretch.find(tail->id); s != k.pose.stretch.end()) lo2 = std::min(lo2, s->second);
    CHECK(lo2 > lo);
    CHECK(lo2 < 1);
    CHECK(std::abs((1 - lo2) - 0.5 * (1 - lo)) < 0.02);
}

TEST_CASE("A six-legged motion walks an auto-rigged six-legged sprite", "[rig][mocap][creature]") {
    // An ant-like side view facing right: abdomen, body, head and six legs.
    const Rgba8 shell{60, 42, 30, 255}, face{122, 74, 42, 255}, legCol{34, 22, 14, 255};
    Image img(96, 44);
    img.fillRect({20, 10, 50, 10}, shell);
    img.fillRect({70, 6, 12, 10}, face);
    img.fillRect({6, 8, 16, 8}, shell);
    for (int x : {24, 31, 40, 47, 56, 63}) img.fillRect({x, 20, 3, 18}, legCol);
    auto doc0 = Document::create(136, 64).value();
    const LayerId hero = doc0.addLayer("Hero").value();
    REQUIRE(doc0.setCel(hero, 0, Cel{img, 20, 10}));
    auto ar = rigs::autoRig(doc0, hero, 0, rigs::AutoRigBody::Legs);
    REQUIRE(ar);
    const Document doc = ar.value().document;
    int thighs = 0;
    for (const Bone& b : doc.rig()->bones) thighs += b.name.find("thigh") != std::string::npos ? 1 : 0;
    REQUIRE(thighs == 6);

    auto r = rigs::keysFromMotion(doc, preset("hex-walk"), {});
    INFO((r ? std::string() : r.error().message));
    REQUIRE(r);
    const rigs::MotionKeysResult& res = r.value();
    for (const auto& n : res.notes) UNSCOPED_INFO(n);
    CHECK(res.count == 7);   // 0.6 s at 12 fps
    const Rig& rig = *res.document.rig();
    const auto rest = rigs::solve(rig, RigPose{});
    // Each pair of the motion drives the two drawn legs standing at its
    // place: sorted from the back, hind, hind, middle, middle, front, front.
    std::vector<std::pair<double, std::string>> byX;
    for (const auto& [bone, what] : res.matched) {
        CAPTURE(bone, what);
        if (bone.find("thigh") == std::string::npos) continue;
        for (std::size_t i = 0; i < rig.bones.size(); ++i)
            if (rig.bones[i].name == bone) byX.emplace_back(rest[i].headX, what.substr(0, what.find(' ')));
    }
    std::sort(byX.begin(), byX.end());
    REQUIRE(byX.size() == 6);
    const char* want[6] = {"hind", "hind", "middle", "middle", "front", "front"};
    for (std::size_t i = 0; i < 6; ++i) CHECK(byX[i].second == want[i]);
    bool paired = false;
    for (const auto& n : res.notes) paired = paired || n.find("6 legs and the rig 6") != std::string::npos;
    CHECK(paired);
    // Every thigh swings, and the keys bake.
    for (const Bone& b : rig.bones) {
        if (b.name.find("thigh") == std::string::npos) continue;
        INFO(b.name);
        double lo = 1e9, hi = -1e9;
        for (const RigKey& k : rig.keys) {
            const double t = turnOf(rig, k.pose, b.name.c_str());
            lo = std::min(lo, t);
            hi = std::max(hi, t);
        }
        CHECK(hi - lo > 0.2);
    }
    REQUIRE(res.document.validate());
    REQUIRE(rigs::bakeKeys(res.document));

    // A four-legged walk on the six-legged rig: every leg is driven by the
    // hind or front pair, and a note says so. A six-legged walk on a
    // four-legged rig leaves the middle pair out.
    auto q = rigs::keysFromMotion(doc, preset("quad-walk"), {});
    REQUIRE(q);
    bool halves = false;
    int driven = 0;
    for (const auto& n : q.value().notes) halves = halves || n.find("6 legs and the motion four") != std::string::npos;
    for (const auto& [bone, what] : q.value().matched) driven += bone.find("thigh") != std::string::npos ? 1 : 0;
    CHECK(halves);
    CHECK(driven == 6);
    auto d4 = rigs::keysFromMotion(rigged("dog72.png"), preset("hex-walk"), {});
    INFO((d4 ? std::string() : d4.error().message));
    REQUIRE(d4);
    bool unused = false;
    for (const auto& n : d4.value().notes) unused = unused || n.find("middle legs are not used") != std::string::npos;
    CHECK(unused);
    for (const auto& [bone, what] : d4.value().matched) CHECK(what.rfind("middle", 0) != 0);
}

TEST_CASE("A wing motion beats and folds an auto-rigged flyer's wings", "[rig][mocap][wings]") {
    auto sprite = readPng(fixture("dragon128.png"));
    REQUIRE(sprite);
    auto doc0 = Document::create(sprite.value().width() + 40, sprite.value().height() + 40).value();
    const LayerId hero = doc0.addLayer("Dragon").value();
    REQUIRE(doc0.setCel(hero, 0, Cel{sprite.value(), 20, 20}));
    auto ar = rigs::autoRig(doc0, hero, 0, rigs::AutoRigBody::Wings);
    REQUIRE(ar);
    const Document doc = ar.value().document;
    std::vector<std::string> wingNames;
    for (const Bone& b : doc.rig()->bones)
        if (b.name.find("wing") != std::string::npos) wingNames.push_back(b.name);
    REQUIRE_FALSE(wingNames.empty());

    auto r = rigs::keysFromMotion(doc, preset("wingbeat"), {});
    INFO((r ? std::string() : r.error().message));
    REQUIRE(r);
    const rigs::MotionKeysResult& res = r.value();
    for (const auto& n : res.notes) UNSCOPED_INFO(n);
    CHECK(res.count == 7);   // 0.6 s at 12 fps
    CHECK(res.matched.size() == wingNames.size());
    bool own = false, folds = false;
    for (const auto& n : res.notes) {
        own = own || n.find("own wing bones") != std::string::npos;
        folds = folds || n.find("fold on the way up") != std::string::npos;
    }
    CHECK(own);
    CHECK(folds);
    const Rig& rig = *res.document.rig();
    REQUIRE(rig.keys.size() == 7);
    for (const std::string& name : wingNames) {
        INFO(name);
        const Bone* b = rig.bone(name);
        REQUIRE(b);
        double lo = 1e9, hi = -1e9, shortest = 1;
        int asDrawn = 0;
        for (const RigKey& k : rig.keys) {
            const double t = turnOf(rig, k.pose, name.c_str());
            lo = std::min(lo, t);
            hi = std::max(hi, t);
            const auto st = k.pose.stretch.find(b->id);
            const double s = st == k.pose.stretch.end() ? 1.0 : st->second;
            shortest = std::min(shortest, s);
            asDrawn += t == 0.0 && s == 1.0 ? 1 : 0;
        }
        CHECK(hi - lo > 0.6);        // it beats
        CHECK(asDrawn >= 1);         // the top of the stroke is the drawing
        CHECK(shortest < 0.95);      // folded on the way up
        CHECK(shortest >= 0.55);
        // The tip goes down on screen from the drawing, and toward the tail
        const auto rest = rigs::solve(rig, RigPose{});
        std::size_t bi = 0;
        for (std::size_t i = 0; i < rig.bones.size(); ++i)
            if (rig.bones[i].id == b->id) bi = i;
        // (the dragon faces right: its tail is on the left).
        double lowest = -1e9, leftMost = 1e9;
        for (const RigKey& k : rig.keys) {
            const auto w = rigs::solve(rig, k.pose);
            lowest = std::max(lowest, w[bi].tailY - w[bi].headY);
            leftMost = std::min(leftMost, w[bi].tailX - w[bi].headX);
        }
        CHECK(lowest > (rest[bi].tailY - rest[bi].headY) + 0.25 * b->length);
        CHECK(leftMost < (rest[bi].tailX - rest[bi].headX) - 0.25 * b->length);   // toward the tail
    }
    REQUIRE(res.document.validate());
    auto baked = rigs::bakeKeys(res.document);
    REQUIRE(baked);

    // Arms drive the wings too (no folding); half the amount beats less; pairs are refused.
    auto fl = rigs::keysFromMotion(doc, preset("flap"), {});
    INFO((fl ? std::string() : fl.error().message));
    REQUIRE(fl);
    bool arms = false;
    for (const auto& n : fl.value().notes) arms = arms || n.find("motion's arms") != std::string::npos;
    CHECK(arms);
    for (const RigKey& k : fl.value().document.rig()->keys) CHECK(k.pose.stretch.empty());
    auto range = [&](const rigs::MotionKeysResult& kr) {
        double lo = 1e9, hi = -1e9;
        for (const RigKey& k : kr.document.rig()->keys) {
            const double t = turnOf(*kr.document.rig(), k.pose, wingNames.front().c_str());
            lo = std::min(lo, t);
            hi = std::max(hi, t);
        }
        return hi - lo;
    };
    rigs::MotionKeysOptions half;
    half.amount = 0.5;
    auto hr = rigs::keysFromMotion(doc, preset("wingbeat"), half);
    REQUIRE(hr);
    CHECK(range(hr.value()) < 0.7 * range(res));
    rigs::MotionKeysOptions pairs;
    pairs.bones = {{wingNames.front(), mocap::Bone::LUpperArm}};
    CHECK_FALSE(rigs::keysFromMotion(doc, preset("wingbeat"), pairs));
}

TEST_CASE("A swimming motion turns an auto-rigged fish's tail fin and paddles its fins", "[rig][mocap][swim]") {
    // A fish seen from the side, head on the left: a tapering body, a narrow
    // stalk, a tall tail fin, a fin under the belly and one on the back.
    const Rgba8 bodyCol{60, 130, 170, 255}, eye{20, 20, 30, 255}, finCol{220, 150, 50, 255}, lowerCol{200, 90, 60, 255}, upperCol{90, 200, 120, 255};
    Image img(72, 40);
    for (int x = 6; x < 44; ++x) {
        const double u = (x - 6) / 37.0;
        const int half = static_cast<int>(std::lround(2 + 8 * std::sin(3.14159265 * std::min(1.0, u * 0.9 + 0.08))));
        img.fillRect({x, 20 - half, 1, 2 * half}, bodyCol);
    }
    img.fillRect({44, 18, 6, 4}, bodyCol);
    img.fillRect({50, 10, 9, 20}, finCol);
    img.fillRect({11, 17, 2, 2}, eye);
    img.fillRect({18, 30, 5, 7}, lowerCol);
    img.fillRect({26, 4, 6, 6}, upperCol);
    auto doc0 = Document::create(112, 80).value();
    const LayerId hero = doc0.addLayer("Fish").value();
    REQUIRE(doc0.setCel(hero, 0, Cel{img, 20, 20}));
    auto ar = rigs::autoRig(doc0, hero, 0, rigs::AutoRigBody::Fins);
    INFO((ar ? std::string() : ar.error().message));
    REQUIRE(ar);
    const Document doc = ar.value().document;
    std::vector<std::string> finNames;
    for (const Bone& b : doc.rig()->bones) {
        UNSCOPED_INFO(b.name);
        if (b.name.find("fin") != std::string::npos) finNames.push_back(b.name);
    }
    REQUIRE(doc.rig()->bone("tail fin"));
    REQUIRE(finNames.size() >= 2);

    auto r = rigs::keysFromMotion(doc, preset("swim"), {});
    INFO((r ? std::string() : r.error().message));
    REQUIRE(r);
    const rigs::MotionKeysResult& res = r.value();
    for (const auto& n : res.notes) UNSCOPED_INFO(n);
    for (const auto& [bone, what] : res.matched) UNSCOPED_INFO(bone + ": " + what);
    CHECK(res.count >= 6);
    CHECK(res.camera.turn == -90);   // the head is at screen left
    const Rig& rig = *res.document.rig();
    REQUIRE(rig.keys.size() == static_cast<std::size_t>(res.count));
    // The tail fin swings both ways about the stalk; every matched fin moves.
    double lo = 1e9, hi = -1e9;
    for (const RigKey& k : rig.keys) {
        const double t = turnOf(rig, k.pose, "tail fin");
        lo = std::min(lo, t);
        hi = std::max(hi, t);
    }
    CHECK(lo < -0.05);
    CHECK(hi > 0.05);
    CHECK(hi - lo <= 1.2 + 1e-9);
    bool tailMatched = false;
    int paddling = 0;
    for (const auto& [bone, what] : res.matched) {
        if (bone == "tail fin") {
            tailMatched = what.rfind("spine", 0) == 0;
            continue;
        }
        if (bone == "body") continue;   // the wave down the spine, checked below
        ++paddling;
        CHECK(what.rfind("fin ", 0) == 0);
        double a = 1e9, b = -1e9;
        for (const RigKey& k : rig.keys) {
            const double t = turnOf(rig, k.pose, bone.c_str());
            a = std::min(a, t);
            b = std::max(b, t);
        }
        INFO(bone);
        CHECK(b - a > 0.02);
        CHECK(std::max(std::abs(a), std::abs(b)) <= 0.35 + 1e-9);
    }
    CHECK(tailMatched);
    CHECK(paddling >= 1);
    // The body bends along its spine: the bones below the head swing both ways.
    REQUIRE(rig.bone("spine 1"));
    int spineBones = 1;
    while (rig.bone("spine " + std::to_string(spineBones + 1))) ++spineBones;
    CHECK(spineBones >= 1);
    const LayerId bodyLayer = [&] {
        for (const auto& [layer, bone] : rig.bindings)
            if (bone == rig.bone("body")->id) return layer;
        return kNoLayer;
    }();
    REQUIRE(bodyLayer != kNoLayer);
    REQUIRE(rig.skins.count(bodyLayer) == 1);
    CHECK(rig.skins.at(bodyLayer).bone == rig.bone("spine " + std::to_string(spineBones))->id);
    double slo = 1e9, shi = -1e9;
    for (const RigKey& k : rig.keys) {
        const double t = turnOf(rig, k.pose, ("spine " + std::to_string(spineBones)).c_str());
        slo = std::min(slo, t);
        shi = std::max(shi, t);
    }
    CHECK(slo < -0.02);
    CHECK(shi > 0.02);
    bool bends = false;
    for (const auto& n : res.notes) bends = bends || n.find("the body bends along") != std::string::npos;
    CHECK(bends);
    // Baked, the fish stays in one piece in every frame and keeps its colours.
    {
        auto baked = rigs::bakeKeys(res.document);
        REQUIRE(baked);
        const Image restImg = baked.value().composite(0);
        std::set<std::uint32_t> drawn;
        std::size_t restCount = 0;
        for (const Rgba8& p : restImg.pixels())
            if (p.a) {
                drawn.insert((std::uint32_t(p.r) << 16) | (std::uint32_t(p.g) << 8) | p.b);
                ++restCount;
            }
        std::set<std::size_t> distinct;
        for (int f = res.first; f < res.first + res.count; ++f) {
            const Image im = baked.value().composite(f);
            std::size_t count = 0, hash = 0;
            for (const Rgba8& p : im.pixels()) {
                hash = hash * 31 + p.a + p.r * 7u;
                if (!p.a) continue;
                ++count;
                CHECK(drawn.count((std::uint32_t(p.r) << 16) | (std::uint32_t(p.g) << 8) | p.b) == 1);
            }
            distinct.insert(hash);
            CHECK(count * 10 >= restCount * 8);
            CHECK(count * 10 <= restCount * 12);
        }
        CHECK(distinct.size() >= 4);
    }
    REQUIRE(res.document.validate());
    REQUIRE(rigs::bakeKeys(res.document));

    // A walk has a spine too (its longest chain) but no fin joints: the tail
    // fin is still matched, the other fins stay as drawn and a note says so.
    auto w = rigs::keysFromMotion(doc, preset("walk"), {});
    INFO((w ? std::string() : w.error().message));
    REQUIRE(w);
    bool stay = false;
    for (const auto& n : w.value().notes) stay = stay || n.find("stay as drawn") != std::string::npos;
    CHECK(stay);
    CHECK(w.value().matched.size() == 2);   // the body's spine and the tail fin
    // Bone pairs do not apply.
    rigs::MotionKeysOptions pairs;
    pairs.bones = {{"tail fin", mocap::Bone::Head}};
    CHECK_FALSE(rigs::keysFromMotion(doc, preset("swim"), pairs));
}

TEST_CASE("A wing rig takes wing joints given by hand", "[rig][mocap][wings]") {
    auto sprite = readPng(fixture("dragon128.png"));
    REQUIRE(sprite);
    auto doc0 = Document::create(sprite.value().width() + 40, sprite.value().height() + 40).value();
    const LayerId hero = doc0.addLayer("Dragon").value();
    REQUIRE(doc0.setCel(hero, 0, Cel{sprite.value(), 20, 20}));
    auto ar = rigs::autoRig(doc0, hero, 0, rigs::AutoRigBody::Wings);
    REQUIRE(ar);
    const Document doc = ar.value().document;
    mocap::Motion m = preset("wingbeat");
    for (mocap::Joint& j : m.joints)
        if (const auto at = j.name.find("Wing"); at != std::string::npos) j.name.replace(at, 4, "Limb");
    auto named = rigs::keysFromMotion(doc, preset("wingbeat"), {});
    REQUIRE(named);
    rigs::MotionKeysOptions o;
    o.wingRoots = {"LeftLimb", "RightLimb"};
    auto r = rigs::keysFromMotion(doc, m, o);
    INFO((r ? std::string() : r.error().message));
    REQUIRE(r);
    bool own = false;
    for (const auto& n : r.value().notes) own = own || n.find("own wing bones") != std::string::npos;
    CHECK(own);
    // The same keys as with the wings named.
    const Rig& a = *r.value().document.rig();
    const Rig& b = *named.value().document.rig();
    REQUIRE(a.keys.size() == b.keys.size());
    for (std::size_t i = 0; i < a.keys.size(); ++i) {
        CHECK(a.keys[i].pose.turn == b.keys[i].pose.turn);
        CHECK(a.keys[i].pose.stretch == b.keys[i].pose.stretch);
    }
    o.wingRoots = {"LeftLimb", "Feather"};
    CHECK_FALSE(rigs::keysFromMotion(doc, m, o));
}
