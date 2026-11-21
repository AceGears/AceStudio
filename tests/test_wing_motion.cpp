// Wings from a 3D motion (ADR-ANIM-002): the flap preset, and a humanoid
// motion's arms driving a winged sprite's wings (retarget --body flyer).

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

const Rgba8 kBody{120, 60, 40, 255}, kHead{200, 150, 90, 255}, kWing{60, 120, 180, 255}, kTail{90, 40, 30, 255};

// A dragon seen from the side, head on the right, one wing up over its back.
Image dragon() {
    Image img(90, 60);
    img.fillRect({30, 30, 30, 14}, kBody);
    img.fillRect({60, 22, 10, 12}, kHead);
    img.fillRect({34, 44, 5, 10}, kBody);   // legs
    img.fillRect({52, 44, 5, 10}, kBody);
    img.fillRect({36, 6, 14, 24}, kWing);
    img.fillRect({6, 34, 24, 4}, kTail);
    return img;
}

Mask wingMask() {
    Mask m(90, 60);
    m.fillRect({36, 6, 14, 24}, true);
    return m;
}

Motion preset(const char* name) {
    auto m = builtInPreset(name);
    REQUIRE(m);
    return std::move(m).value();
}

int count(const Image& f, Rgba8 c) {
    int n = 0;
    for (const Rgba8& p : f.pixels()) n += p == c ? 1 : 0;
    return n;
}

// The wing's lowest and highest rows relative to the head's top row (the
// head only rides the body's heave).
std::pair<int, int> wingRows(const Image& f) {
    int headTop = -1, top = f.height(), bottom = -1;
    for (int y = 0; y < f.height(); ++y)
        for (int x = 0; x < f.width(); ++x) {
            const Rgba8 p = f.pixel(x, y);
            if (p == kHead && headTop < 0) headTop = y;
            if (p == kWing) {
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
        }
    return {top - headTop, bottom - headTop};
}

RetargetOptions flyer() {
    RetargetOptions o;
    o.body = motion::Body::Flyer;
    o.wingMask = wingMask();
    return o;
}

} // namespace

TEST_CASE("The flap preset beats the arms up and down", "[mocap][wings]") {
    bool listed = false;
    for (const PresetInfo& p : builtInPresets())
        if (p.name == "flap") listed = p.loops && !p.fourLegged && p.builtIn;
    CHECK(listed);
    const Motion m = preset("flap");
    const BoneMap map = autoMap(m);
    REQUIRE(map[Bone::LUpperArm] >= 0);
    REQUIRE(map[Bone::LHand] >= 0);
    double hi = -10, lo = 10;
    for (int f = 0; f < m.frameCount; ++f) {
        const auto w = jointPositions(m, f * m.frameTime);
        const Vec3 p = w[static_cast<std::size_t>(map[Bone::LForearm])], q = w[static_cast<std::size_t>(map[Bone::LUpperArm])];
        const double e = std::atan2(p.y - q.y, std::hypot(p.x - q.x, p.z - q.z));
        hi = std::max(hi, e);
        lo = std::min(lo, e);
    }
    // Raised well above the shoulder and beaten well below it.
    CHECK(hi > 0.8);
    CHECK(lo < -0.6);
}

TEST_CASE("A humanoid motion's arms beat a winged sprite's wings", "[mocap][wings]") {
    const Image d = dragon();
    auto r = retarget(d, preset("flap"), flyer());
    REQUIRE(r);
    const RetargetResult& res = r.value();
    const auto& fr = res.clip.frames;
    REQUIRE(fr.size() == 6);   // 0.5 s at 12 fps
    CHECK(res.clip.loops);
    CHECK(res.clip.body == motion::Body::Flyer);
    CHECK(res.source.frames.size() == fr.size());
    CHECK(res.camera.turn == 90);   // the head is on the right
    CHECK_FALSE(res.approximateView);

    const int wing0 = count(d, kWing), head0 = count(d, kHead);
    const auto [restTop, restBottom] = wingRows(d);
    int lowest = -1000;
    std::set<std::uint32_t> drawn;
    for (const Rgba8& p : d.pixels()) drawn.insert(p.a ? (std::uint32_t(p.r) << 16 | std::uint32_t(p.g) << 8 | p.b) : 0u);
    for (const Image& f : fr) {
        // The wing turns (keeps its pixels), the head is untouched, and
        // only the sprite's colours appear.
        CHECK(count(f, kWing) * 10 >= wing0 * 7);
        CHECK(count(f, kHead) == head0);
        for (const Rgba8& p : f.pixels())
            if (p.a) CHECK(drawn.count(std::uint32_t(p.r) << 16 | std::uint32_t(p.g) << 8 | p.b));
        lowest = std::max(lowest, wingRows(f).second);
    }
    // The first frame is the top of the stroke: the wing as drawn.
    CHECK(wingRows(fr[0]).first == restTop);
    // At the bottom of the stroke the wing reaches well below where it was drawn.
    CHECK(lowest >= restBottom + 8);

    // The tail waves with the stroke: its tip's top row, relative to the
    // head, moves by at least a pixel over the beat.
    std::set<int> tipRows;
    for (const Image& f : fr) {
        int headTop = -1, tipX = f.width(), tipTop = -1;
        for (int y = 0; y < f.height(); ++y)
            for (int x = 0; x < f.width(); ++x) {
                if (f.pixel(x, y) == kHead && headTop < 0) headTop = y;
                if (f.pixel(x, y) == kTail && x < tipX) tipX = x;
            }
        for (int y = 0; y < f.height() && tipTop < 0; ++y)
            if (f.pixel(tipX, y) == kTail) tipTop = y;
        tipRows.insert(tipTop - headTop);
    }
    CHECK(tipRows.size() >= 2);
    // At most the wave's reach either side (0.15 of the tail's 24 columns).
    CHECK(*tipRows.rbegin() - *tipRows.begin() <= 2 * 4 + 1);

    // Less swing with a smaller amount.
    RetargetOptions half = flyer();
    half.amount = 0.5;
    auto r2 = retarget(d, preset("flap"), half);
    REQUIRE(r2);
    int lowest2 = -1000;
    for (const Image& f : r2.value().clip.frames) lowest2 = std::max(lowest2, wingRows(f).second);
    CHECK(lowest2 < lowest);
}

TEST_CASE("Wings from a motion whose arms hardly lift barely move, and say so", "[mocap][wings]") {
    auto r = retarget(dragon(), preset("idle"), flyer());
    REQUIRE(r);
    bool noted = false;
    for (const auto& n : r.value().notes) noted = noted || n.find("hardly rise") != std::string::npos;
    CHECK(noted);
    const auto [restTop, restBottom] = wingRows(dragon());
    for (const Image& f : r.value().clip.frames) CHECK(std::abs(wingRows(f).second - restBottom) <= 3);
}

TEST_CASE("Wings need a winged sprite and a motion with arms", "[mocap][wings]") {
    // A four-legged motion has no humanoid arms.
    auto noArms = retarget(dragon(), preset("quad-walk"), flyer());
    REQUIRE_FALSE(noArms);
    CHECK(noArms.error().message.find("arms") != std::string::npos);
    // A plain block has nothing above its wing line to flap... unless asked:
    // an empty wing mask selection finds no wings.
    Image block(40, 40);
    block.fillRect({10, 10, 20, 20}, kBody);
    RetargetOptions o = flyer();
    o.wingMask = Mask(40, 40);
    auto none = retarget(block, preset("flap"), o);
    REQUIRE_FALSE(none);
    CHECK(none.error().message.find("wings") != std::string::npos);
}

namespace {

// The wing's mean column and its row count, relative to the head's left column.
std::pair<double, int> wingShape(const Image& f) {
    int headLeft = f.width(), top = f.height(), bottom = -1, n = 0;
    double sum = 0;
    for (int y = 0; y < f.height(); ++y)
        for (int x = 0; x < f.width(); ++x) {
            const Rgba8 p = f.pixel(x, y);
            if (p == kHead) headLeft = std::min(headLeft, x);
            if (p == kWing) {
                sum += x;
                ++n;
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
        }
    return {n ? sum / n - headLeft : 0.0, bottom - top + 1};
}

} // namespace

TEST_CASE("The wingbeat preset is a bird's skeleton whose wings fold on the way up", "[mocap][wings]") {
    bool listed = false;
    for (const PresetInfo& p : builtInPresets())
        if (p.name == "wingbeat") listed = p.loops && p.winged && !p.fourLegged && !p.swimmer && p.builtIn;
    CHECK(listed);
    const Motion m = preset("wingbeat");
    const int sh = m.find("LeftWing"), el = m.find("LeftWing1"), tip = m.find("LeftWing2_end");
    REQUIRE(sh >= 0);
    REQUIRE(el >= 0);
    REQUIRE(tip >= 0);
    double hi = -10, lo = 10, longest = 0, shortest = 1e9, back = 0;
    for (int f = 0; f < m.frameCount; ++f) {
        const auto w = jointPositions(m, f * m.frameTime);
        const Vec3 s0 = w[static_cast<std::size_t>(sh)], e = w[static_cast<std::size_t>(el)], t = w[static_cast<std::size_t>(tip)];
        const double up = std::atan2(e.y - s0.y, std::hypot(e.x - s0.x, e.z - s0.z));
        hi = std::max(hi, up);
        lo = std::min(lo, up);
        const double span = std::sqrt((t.x - s0.x) * (t.x - s0.x) + (t.y - s0.y) * (t.y - s0.y) + (t.z - s0.z) * (t.z - s0.z));
        longest = std::max(longest, span);
        shortest = std::min(shortest, span);
        back = std::min(back, e.z - s0.z);
    }
    CHECK(hi > 0.8);
    CHECK(lo < -0.5);
    CHECK(shortest < 0.85 * longest);   // folded
    CHECK(back < -2.0);                 // swept back (the bird faces +Z)
}

TEST_CASE("A skeleton's own wing bones beat, fold and sweep a winged sprite's wings", "[mocap][wings]") {
    const Image d = dragon();
    auto r = retarget(d, preset("wingbeat"), flyer());
    INFO((r ? std::string() : r.error().message));
    REQUIRE(r);
    const RetargetResult& res = r.value();
    const auto& fr = res.clip.frames;
    REQUIRE(fr.size() == 7);   // 0.6 s at 12 fps
    CHECK(res.clip.body == motion::Body::Flyer);
    CHECK(res.camera.turn == 90);
    bool said = false, guessed = false;
    for (const std::string& n : res.notes) {
        said = said || n.rfind("Wings: LeftWing and RightWing", 0) == 0 || n.rfind("Wings: RightWing and LeftWing", 0) == 0;
        guessed = guessed || n.find("guess") != std::string::npos;
    }
    CHECK(said);
    CHECK_FALSE(guessed);   // the skeleton has a head

    const int wing0 = count(d, kWing), head0 = count(d, kHead);
    const auto [restTop, restBottom] = wingRows(d);
    const auto [restCol, restRows] = wingShape(d);
    std::set<std::uint32_t> drawn;
    for (const Rgba8& p : d.pixels()) drawn.insert(p.a ? (std::uint32_t(p.r) << 16 | std::uint32_t(p.g) << 8 | p.b) : 0u);
    int lowest = -1000, fewest = wing0;
    double leftMost = 1e9;
    for (const Image& f : fr) {
        CHECK(count(f, kHead) == head0);
        CHECK(count(f, kWing) * 10 >= wing0 * 4);
        for (const Rgba8& p : f.pixels())
            if (p.a) CHECK(drawn.count(std::uint32_t(p.r) << 16 | std::uint32_t(p.g) << 8 | p.b));
        lowest = std::max(lowest, wingRows(f).second);
        fewest = std::min(fewest, count(f, kWing));
        leftMost = std::min(leftMost, wingShape(f).first);
    }
    // The first frame is the top of the stroke, spread: the wing as drawn.
    CHECK(wingRows(fr[0]).first == restTop);
    CHECK(wingShape(fr[0]).second == restRows);
    CHECK(std::abs(wingShape(fr[0]).first - restCol) < 0.01);
    CHECK(lowest >= restBottom + 5);          // it beats down (a 90 degree stroke)
    CHECK(fewest * 100 <= wing0 * 90);        // folded on the way up: shorter
    // Swept back: the wing leans toward the tail (the head is on the right).
    // A plain turn about the shoulder moves it that way too, so compare with
    // the same beat on arms, which neither fold nor sweep the wing: the
    // frames differ.
    auto arms = retarget(d, preset("flap"), flyer());
    REQUIRE(arms);
    bool noNote = true;
    for (const std::string& n : arms.value().notes) noNote = noNote && n.rfind("Wings: ", 0) != 0;
    CHECK(noNote);
    CHECK(leftMost < restCol - 1.0);

    // Half the amount: less folding.
    RetargetOptions half = flyer();
    half.amount = 0.5;
    auto r2 = retarget(d, preset("wingbeat"), half);
    REQUIRE(r2);
    int fewest2 = wing0;
    for (const Image& f : r2.value().clip.frames) fewest2 = std::min(fewest2, count(f, kWing));
    CHECK(fewest2 > fewest);
}

TEST_CASE("Wing bones are found by name on any skeleton, and arms still work without them", "[mocap][wings]") {
    // The wingbeat skeleton with its head renamed: forward is then a guess, and a note says so.
    Motion m = preset("wingbeat");
    for (Joint& j : m.joints)
        if (j.name == "Head") j.name = "Skull";
        else if (j.name == "Head_end") j.name = "Skull_end";
    auto r = retarget(dragon(), m, flyer());
    REQUIRE(r);
    bool guessed = false;
    for (const std::string& n : r.value().notes) guessed = guessed || n.find("which way it faces is a guess") != std::string::npos;
    CHECK(guessed);
    // One wing chain only is not a pair: the skeleton is then read as any
    // other (here its wings pass for arms by their shape), without fold or sweep.
    Motion one = preset("wingbeat");
    for (Joint& j : one.joints)
        if (j.name.rfind("Right", 0) == 0) j.name = "R" + std::to_string(&j - one.joints.data());
    auto plain = retarget(dragon(), one, flyer());
    REQUIRE(plain);
    for (const std::string& n : plain.value().notes) CHECK(n.rfind("Wings: ", 0) != 0);
}

TEST_CASE("Wing joints with other names are given by hand", "[mocap][wings]") {
    // The wingbeat skeleton with its wings renamed: nothing is named wing.
    Motion m = preset("wingbeat");
    for (Joint& j : m.joints)
        if (const auto at = j.name.find("Wing"); at != std::string::npos) j.name.replace(at, 4, "Limb");
    const Image d = dragon();
    auto named = retarget(d, preset("wingbeat"), flyer());
    REQUIRE(named);
    // Unaided, the renamed wings pass for arms: no fold, no sweep.
    auto unaided = retarget(d, m, flyer());
    REQUIRE(unaided);
    for (const std::string& n : unaided.value().notes) CHECK(n.rfind("Wings: ", 0) != 0);

    // Given the two joints the wings start at (case does not matter), the
    // frames are those of the skeleton with its wings named.
    RetargetOptions o = flyer();
    o.wingRoots = {"leftlimb", "RightLimb"};
    auto r = retarget(d, m, o);
    INFO((r ? std::string() : r.error().message));
    REQUIRE(r);
    bool said = false;
    for (const std::string& n : r.value().notes) said = said || n.rfind("Wings: LeftLimb and RightLimb", 0) == 0 || n.rfind("Wings: RightLimb and LeftLimb", 0) == 0;
    CHECK(said);
    REQUIRE(r.value().clip.frames.size() == named.value().clip.frames.size());
    for (std::size_t i = 0; i < r.value().clip.frames.size(); ++i) CHECK(r.value().clip.frames[i] == named.value().clip.frames[i]);
    // wingSource reads the same chains.
    const WingSource ws = wingSource(m, autoMap(m), o.wingRoots);
    REQUIRE(ws.found());
    CHECK(ws.own);
    CHECK(m.joints[static_cast<std::size_t>(ws.shoulder[0])].name == "LeftLimb");
    CHECK(m.joints[static_cast<std::size_t>(ws.tip[1])].name.rfind("RightLimb2", 0) == 0);

    // Refusals: not two names, an unknown joint, the same joint twice, a joint with nothing below it.
    for (const std::vector<std::string>& bad : {std::vector<std::string>{"LeftLimb"}, {"LeftLimb", "Feather"}, {"LeftLimb", "leftlimb"},
                                                {"LeftLimb", "RightLimb2_end"}}) {
        RetargetOptions b = flyer();
        b.wingRoots = bad;
        CHECK_FALSE(retarget(d, m, b));
        CHECK_FALSE(wingSource(m, autoMap(m), bad).found());
    }
}
