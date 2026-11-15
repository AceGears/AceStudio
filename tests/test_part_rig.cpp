// The part rig itself (core/src/PartRig.h, ADR-ANIM-001 steps 1-4) and its
// species modules on real sprites (golden fixtures in tests/data/anim).

#include "PartRig.h"

#include "acestudio/core/Effects.h"
#include "acestudio/core/PngCodec.h"
#include "acestudio/core/SpriteMotion.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace acestudio;
using namespace acestudio::motion;
using namespace acestudio::fx;

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

TEST_CASE("Golden: every sprite's clips meet the expected results", "[rig][golden]") {
    std::ifstream in(ACESTUDIO_SOURCE_DIR "/tests/data/anim/expected.json");
    REQUIRE(in.good());
    const nlohmann::json table = nlohmann::json::parse(in);
    for (const auto& sprite : table.at("sprites")) {
        const std::string file = sprite.at("file").get<std::string>();
        const Image img = load(file.c_str());
        const auto palette = colours(img);
        for (const auto& want : sprite.at("clips")) {
            const std::string key = want.at("action").get<std::string>();
            INFO(file << " " << key);
            const auto action = actionFromKey(key);
            REQUIRE(action);
            Options o;
            o.action = *action;
            if (sprite.contains("body")) o.body = bodyFromString(sprite.at("body").get<std::string>());
            auto clip = animate(img, o);
            REQUIRE(clip);
            const auto& fr = clip.value().frames;
            CHECK(static_cast<int>(fr.size()) == want.at("frames").get<int>());
            for (const Image& f : fr)
                for (auto c : colours(f)) CHECK(palette.count(c) == 1);
            if (want.contains("uniqueFrames")) {
                int unique = 0;
                for (std::size_t i = 0; i < fr.size(); ++i) {
                    bool seen = false;
                    for (std::size_t j = 0; j < i && !seen; ++j) seen = fr[j] == fr[i];
                    unique += seen ? 0 : 1;
                }
                CHECK(unique == want.at("uniqueFrames").get<int>());
            }
            const ClipCheck chk = checkClip(clip.value());
            for (const auto& i : chk.issues) UNSCOPED_INFO(i);
            CHECK(chk.issues.empty());
            CHECK(clip.value().depthPops == 0);
            CHECK(chk.maxAreaChange <= want.at("maxAreaChange").get<double>());
            CHECK(chk.minNeighbourIoU >= want.at("minNeighbourIoU").get<double>());
        }
    }
}

TEST_CASE("Golden: the campfire's flames move in its colours and stay on the canvas", "[rig][golden][fx]") {
    std::ifstream in(ACESTUDIO_SOURCE_DIR "/tests/data/anim/expected.json");
    REQUIRE(in.good());
    const nlohmann::json table = nlohmann::json::parse(in);
    for (const auto& want : table.at("fire")) {
        const std::string style = want.at("style").get<std::string>();
        INFO(style);
        const Image img = load(want.at("file").get<std::string>().c_str());
        const auto palette = colours(img);
        EffectOptions o;
        o.effect = Effect::Fire;
        const auto parsed = fireStyleFromString(style);
        REQUIRE(parsed);
        o.fire = *parsed;
        const EffectClip c = animateEffect(img, o);
        REQUIRE(static_cast<int>(c.frames.size()) >= want.at("minFrames").get<int>());
        for (std::size_t i = 0; i < c.frames.size(); ++i) {
            const Image& f = c.frames[i];
            for (auto col : colours(f)) CHECK(palette.count(col) == 1);
            for (int x = 0; x < f.width(); ++x) CHECK(f.pixel(x, 0).a == 0);   // never cut off at the top
            CHECK_FALSE(f == c.frames[(i + 1) % c.frames.size()]);              // never a held frame
        }
    }
}

// ---- Clip checks ---------------------------------------------------------------

namespace {

Image flipped(const Image& img) {
    Image out(img.width(), img.height());
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) out.setPixel(img.width() - 1 - x, y, img.pixel(x, y));
    return out;
}

Clip clipOf(std::vector<Image> frames) {
    Clip c;
    c.name = "idle";
    c.loops = true;
    c.frames = std::move(frames);
    c.durations.assign(c.frames.size(), 100);
    return c;
}

} // namespace

TEST_CASE("The clip checks report a frame that is the mirror of the one before", "[rig][qa]") {
    const Image a = twoBlocks();
    const ClipCheck flip = checkClip(clipOf({a, flipped(a), a, flipped(a)}));
    CHECK(flip.mirrorFlips == 4);   // three pairs and the loop seam
    CHECK_FALSE(flip.issues.empty());
    // A held pose is not a flip, even on a symmetric sprite.
    Image symmetric(16, 16);
    symmetric.fillRect({4, 4, 8, 8}, kA);
    CHECK(checkClip(clipOf({symmetric, symmetric, symmetric})).mirrorFlips == 0);
    // A symmetric figure stepping in place: its two steps mirror each other,
    // but only the feet differ, so it does not read as a flip.
    Image left(24, 32), right(24, 32);
    for (Image* f : {&left, &right}) {
        f->fillRect({6, 2, 12, 18}, kCloth);
        f->fillRect({7, 20, 4, 8}, kB);
        f->fillRect({13, 20, 4, 8}, kB);
    }
    left.fillRect({7, 28, 4, 2}, kInk);
    right.fillRect({13, 28, 4, 2}, kInk);
    CHECK(checkClip(clipOf({left, right})).mirrorFlips == 0);
}

TEST_CASE("The clip checks report parts that pop through each other", "[rig][qa]") {
    const Image a = twoBlocks();
    Clip popped = clipOf({a, a});
    popped.depthPops = 1;
    CHECK_FALSE(checkClip(popped).issues.empty());
    CHECK(checkClip(clipOf({a, a})).issues.empty());
}

// ---- Follow-through ------------------------------------------------------------

TEST_CASE("A side-on walker's shoulders and pelvis shift against each other; the head does not", "[rig][gait]") {
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
    const auto leftOf = [](const Image& f, int row, Rgba8 c) {
        for (int x = 0; x < f.width(); ++x)
            if (f.pixel(x, row) == c) return x;
        return -1;
    };
    int headLeft = -1;
    std::set<int> pelvisMoves;
    for (const Image& f : clip.value().frames) {
        int headTop = -1;
        for (int y = 0; y < f.height() && headTop < 0; ++y)
            if (leftOf(f, y, kSkin) >= 0) headTop = y;
        REQUIRE(headTop >= 0);
        // The head only moves up and down.
        const int hx = leftOf(f, headTop, kSkin);
        if (headLeft < 0) headLeft = hx;
        CHECK(hx == headLeft);
        // Rows 11 (shoulders) and 18 (pelvis) of the source, relative to the head.
        const int shoulder = leftOf(f, headTop + 9, kCloth) - (headLeft - 1);
        const int pelvis = leftOf(f, headTop + 16, kCloth) - (headLeft - 1);
        CHECK(shoulder == -pelvis);
        pelvisMoves.insert(pelvis);
    }
    // Both ways over the cycle, by at least a pixel (ADR humanoid test 4).
    CHECK(*pelvisMoves.begin() <= -1);
    CHECK(*pelvisMoves.rbegin() >= 1);
}

TEST_CASE("A flapping dragon's tail tip lags behind the middle of its tail", "[rig][flap]") {
    Image img(90, 60);
    img.fillRect({30, 30, 30, 14}, kCloth);   // body
    img.fillRect({60, 22, 10, 12}, kSkin);    // head, on the right
    img.fillRect({34, 44, 5, 10}, kCloth);    // legs
    img.fillRect({52, 44, 5, 10}, kCloth);
    img.fillRect({36, 6, 14, 24}, kA);        // wing
    img.fillRect({6, 34, 24, 4}, kB);         // tail, thin, on the left
    Mask wing(90, 60);
    wing.fillRect({36, 6, 14, 24}, true);
    Options o;
    o.action = Action::Flap;
    o.wingMask = wing;
    auto clip = animate(img, o);
    REQUIRE(clip);
    const auto& fr = clip.value().frames;
    REQUIRE(fr.size() == 8);
    // The tail's top row in a column, relative to the head (which only rides
    // the body's heave).
    int tipX = 1 << 20;
    for (const Image& f : fr)
        for (int y = 0; y < f.height(); ++y)
            for (int x = 0; x < f.width(); ++x)
                if (f.pixel(x, y) == kB) tipX = std::min(tipX, x);
    const auto waveAt = [&](int column) {
        std::vector<double> v;
        for (const Image& f : fr) {
            int headTop = -1, top = -1;
            for (int y = 0; y < f.height(); ++y)
                for (int x = 0; x < f.width(); ++x) {
                    if (headTop < 0 && f.pixel(x, y) == kSkin) headTop = y;
                    if (top < 0 && x == column && f.pixel(x, y) == kB) top = y;
                }
            REQUIRE(top >= 0);
            v.push_back(top - headTop);
        }
        return v;
    };
    // Each column's timing: the phase of its once-per-cycle wave, in frames.
    const auto phaseOf = [](const std::vector<double>& v) {
        double c = 0, s = 0;
        for (std::size_t i = 0; i < v.size(); ++i) {
            const double a = 2 * 3.14159265358979 * static_cast<double>(i) / static_cast<double>(v.size());
            c += v[i] * std::cos(a);
            s += v[i] * std::sin(a);
        }
        return std::atan2(s, c) * static_cast<double>(v.size()) / (2 * 3.14159265358979);
    };
    const auto tip = waveAt(tipX), middle = waveAt(tipX + 12);
    double lag = phaseOf(tip) - phaseOf(middle);
    while (lag < -4) lag += 8;
    while (lag > 4) lag -= 8;
    INFO("lag " << lag);
    CHECK(lag >= 1.0);   // ADR dragon test 6
    // The tip moves further than the middle.
    CHECK(*std::max_element(tip.begin(), tip.end()) - *std::min_element(tip.begin(), tip.end()) >
          *std::max_element(middle.begin(), middle.end()) - *std::min_element(middle.begin(), middle.end()));
    // The tail keeps its pixels (it only shifts by whole pixels), except where
    // the near wing, sweeping towards the tail, is drawn over it.
    int checked = 0;
    for (const Image& f : fr) {
        // Only wing pixels on the rows the tail can be on can cover it.
        int tailTop = f.height(), tailBottom = -1;
        for (int y = 0; y < f.height(); ++y)
            for (int x = tipX; x < tipX + 24; ++x)
                if (f.pixel(x, y) == kB) {
                    tailTop = std::min(tailTop, y);
                    tailBottom = std::max(tailBottom, y);
                }
        bool wingOverTail = false;
        for (int y = tailTop - 1; y <= tailBottom + 1; ++y)
            for (int x = tipX; x < tipX + 24; ++x) wingOverTail = wingOverTail || f.pixel(x, y) == kA;
        if (wingOverTail) continue;
        ++checked;
        CHECK(countColour(f, kB) == countColour(img, kB));
    }
    CHECK(checked >= 4);
}

// ---- Short legs ----------------------------------------------------------------

namespace {

// A big-headed side-view character facing left, as a generated hero on the
// test PC: the head is half the height, the legs are a seventh of it and
// mostly boot, the back foot stands 3 rows higher, and the front sole has a
// two-bump tread in its lowest row.
Image chibiSide() {
    Image img(64, 100);
    img.fillRect({10, 2, 44, 46}, kSkin);     // head
    img.fillRect({16, 48, 34, 32}, kCloth);   // body, down to the hem at row 80
    img.fillRect({14, 80, 16, 8}, kInk);      // front leg: trousers
    img.fillRect({10, 88, 20, 8}, kSole1);    // front boot, toes to the left
    img.fillRect({36, 80, 14, 8}, kInk);      // back leg: trousers
    img.fillRect({32, 88, 18, 5}, kSole2);    // back boot, ends 3 rows higher
    img.fillRect({12, 96, 4, 1}, kSole1);     // tread: two bumps under the front sole
    img.fillRect({22, 96, 4, 1}, kSole1);
    return img;
}

} // namespace

TEST_CASE("A big-headed side view with short legs is a biped", "[rig][gait]") {
    const Subject s = analyzeSubject(chibiSide());
    CHECK(s.body == Body::Biped);
    REQUIRE(s.feet.size() == 2);
    CHECK_FALSE(s.frontView);
    CHECK(s.legTop >= 76);
    CHECK(s.legTop <= 82);
    CHECK(s.facing == -1);
    // A fish's belly and tail fins still are not legs (split for 2 rows).
    Image fish(60, 24);
    fish.fillRect({4, 2, 52, 18}, kB);
    fish.fillRect({10, 20, 8, 2}, kB);
    fish.fillRect({40, 20, 8, 2}, kB);
    CHECK(analyzeSubject(fish).feet.size() < 2);
}

TEST_CASE("Short legs walk with each boot in one piece", "[rig][gait]") {
    const Image img = chibiSide();
    Options o;
    o.action = Action::Walk;
    o.frames = 8;
    auto clip = animate(img, o);
    REQUIRE(clip);
    const auto& fr = clip.value().frames;
    REQUIRE(fr.size() == 8);
    // Each boot's box in a frame: every pixel of it is still there (the back
    // boot can be partly behind the front one), and it keeps its shape.
    const auto box = [](const Image& f, Rgba8 c) {
        int x0 = 1 << 20, y0 = 1 << 20, x1 = -1, y1 = -1;
        for (int y = 0; y < f.height(); ++y)
            for (int x = 0; x < f.width(); ++x)
                if (f.pixel(x, y) == c) {
                    x0 = std::min(x0, x);
                    y0 = std::min(y0, y);
                    x1 = std::max(x1, x);
                    y1 = std::max(y1, y);
                }
        return IntRect{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
    };
    std::set<int> frontX;
    int lifted = 0;
    int ground = 0;
    for (const Image& f : fr) ground = std::max(ground, box(f, kSole1).y + box(f, kSole1).h);
    for (std::size_t i = 0; i < fr.size(); ++i) {
        INFO("frame " << i);
        // The front boot keeps its shape: 20 wide, at most its 9 rows, and
        // nearly all its pixels (the sinking hem may cover its top rows).
        CHECK(countColour(fr[i], kSole1) * 10 >= countColour(img, kSole1) * 7);
        const IntRect b = box(fr[i], kSole1);
        UNSCOPED_INFO("front boot " << b.x << "," << b.y << " " << b.w << "x" << b.h);
        CHECK(b.w == 20);
        CHECK(b.h <= 9);
        frontX.insert(b.x);
        lifted += b.y + b.h < ground ? 1 : 0;
        // The back boot is not stretched or turned either.
        const IntRect k = box(fr[i], kSole2);
        CHECK(k.w <= 18);
        CHECK(k.h <= 5);
        for (auto c : colours(fr[i])) CHECK(colours(img).count(c) == 1);
    }
    CHECK(frontX.size() >= 4);   // it travels
    CHECK(lifted >= 2);          // and lifts on the way forward
    // Nothing is wrong with the clip except what this walk is: boots that
    // slide. The checks say so for a figure this size (96 px with its bob).
    const ClipCheck chk = checkClip(clip.value());
    for (const auto& i : chk.issues) UNSCOPED_INFO(i);
    CHECK(chk.issues.size() == (chk.legsOnlySlide ? 1u : 0u));
}
