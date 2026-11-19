#include "acestudio/core/MotionLibrary.h"
#include "acestudio/core/PngCodec.h"
#include "acestudio/core/RigMotion.h"
#include "acestudio/core/RigTools.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <string>

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
    for (int x : {24, 33, 48, 57}) s.fillRect({x, 34, 7, 20}, coat);  // legs (a foot is at least a twelfth of the width)
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
