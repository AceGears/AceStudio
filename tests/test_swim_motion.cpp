// Fins from a 3D motion (ADR-ANIM-002): the swim preset, and a motion's
// spine bending a fish-like sprite (retarget --body swimmer).

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

const Rgba8 kBody{60, 130, 170, 255}, kEye{20, 20, 30, 255}, kFin{220, 150, 50, 255};

// A fish seen from the side, head on the left: a body that tapers to a
// narrow stalk, and a tall tail fin behind it.
Image fish() {
    Image img(72, 40);
    for (int x = 6; x < 44; ++x) {
        const double u = (x - 6) / 37.0;
        const int half = static_cast<int>(std::lround(2 + 8 * std::sin(3.14159265 * std::min(1.0, u * 0.9 + 0.08))));
        img.fillRect({x, 20 - half, 1, 2 * half}, kBody);
    }
    img.fillRect({44, 18, 6, 4}, kBody);    // the stalk
    img.fillRect({50, 10, 9, 20}, kFin);    // the tail fin
    img.fillRect({11, 17, 2, 2}, kEye);
    return img;
}

Motion preset(const char* name) {
    auto m = builtInPreset(name);
    REQUIRE(m);
    return std::move(m).value();
}

RetargetOptions swimmer() {
    RetargetOptions o;
    o.body = motion::Body::Swimmer;
    return o;
}

// The mean row of a colour's pixels, or -1.
double meanRow(const Image& f, Rgba8 c) {
    double sum = 0;
    int n = 0;
    for (int y = 0; y < f.height(); ++y)
        for (int x = 0; x < f.width(); ++x)
            if (f.pixel(x, y) == c) {
                sum += y;
                ++n;
            }
    return n ? sum / n : -1;
}

int opaque(const Image& f) {
    int n = 0;
    for (const Rgba8& p : f.pixels()) n += p.a ? 1 : 0;
    return n;
}

} // namespace

TEST_CASE("The swim preset runs a wave down a spine", "[mocap][swim]") {
    bool listed = false;
    for (const PresetInfo& p : builtInPresets())
        if (p.name == "swim") listed = p.loops && p.swimmer && !p.fourLegged && p.builtIn;
    CHECK(listed);
    const Motion m = preset("swim");
    int head = -1, tip = -1;
    for (std::size_t j = 0; j < m.joints.size(); ++j) {
        if (m.joints[j].name == "Head") head = static_cast<int>(j);
        if (m.joints[j].name == "TailFin_end") tip = static_cast<int>(j);
    }
    REQUIRE(head >= 0);
    REQUIRE(tip >= 0);
    double headLo = 1e9, headHi = -1e9, tipLo = 1e9, tipHi = -1e9;
    for (int f = 0; f < m.frameCount; ++f) {
        const auto w = jointPositions(m, f * m.frameTime);
        headLo = std::min(headLo, w[static_cast<std::size_t>(head)].x);
        headHi = std::max(headHi, w[static_cast<std::size_t>(head)].x);
        tipLo = std::min(tipLo, w[static_cast<std::size_t>(tip)].x);
        tipHi = std::max(tipHi, w[static_cast<std::size_t>(tip)].x);
    }
    CHECK(headHi - headLo < 1e-6);   // the head stays; the tail's end sweeps
    CHECK(tipHi - tipLo > 15);
}

TEST_CASE("A motion's spine bends a fish and turns its tail fin", "[mocap][swim]") {
    const Image s = fish();
    auto r = retarget(s, preset("swim"), swimmer());
    REQUIRE(r);
    const RetargetResult& res = r.value();
    const auto& fr = res.clip.frames;
    REQUIRE(fr.size() == 10);   // 0.8 s at 12 fps
    CHECK(res.clip.loops);
    CHECK(res.clip.body == motion::Body::Swimmer);
    CHECK(res.source.frames.size() == fr.size());
    CHECK(res.camera.turn == -90);   // the head is on the left
    CHECK_FALSE(res.approximateView);
    bool spineNote = false;
    for (const std::string& n : res.notes) spineNote = spineNote || n.rfind("Spine: Head to TailFin_end", 0) == 0;
    CHECK(spineNote);

    std::set<std::uint32_t> drawn;
    for (const Rgba8& p : s.pixels()) drawn.insert(p.a ? (std::uint32_t(p.r) << 16 | std::uint32_t(p.g) << 8 | p.b) : 0u);
    const int body0 = opaque(s);
    double finLo = 1e9, finHi = -1e9, eyeLo = 1e9, eyeHi = -1e9;
    std::set<int> finWidths;
    for (const Image& f : fr) {
        CHECK(f.width() == fr[0].width());
        // Columns only slide and the fin only turns: the pixel count stays
        // close, and only the sprite's colours appear.
        CHECK(std::abs(opaque(f) - body0) * 10 <= body0);
        for (const Rgba8& p : f.pixels())
            if (p.a) CHECK(drawn.count(std::uint32_t(p.r) << 16 | std::uint32_t(p.g) << 8 | p.b));
        const double fin = meanRow(f, kFin), eye = meanRow(f, kEye);
        REQUIRE(fin >= 0);
        REQUIRE(eye >= 0);
        finLo = std::min(finLo, fin);
        finHi = std::max(finHi, fin);
        eyeLo = std::min(eyeLo, eye);
        eyeHi = std::max(eyeHi, eye);
        int x0 = f.width(), x1 = -1;
        for (int y = 0; y < f.height(); ++y)
            for (int x = 0; x < f.width(); ++x)
                if (f.pixel(x, y) == kFin) {
                    x0 = std::min(x0, x);
                    x1 = std::max(x1, x);
                }
        finWidths.insert(x1 - x0);
    }
    CHECK(finHi - finLo >= 3);    // the tail fin sweeps up and down
    CHECK(finHi - finLo <= 20);   // within the fish's height
    CHECK(eyeHi - eyeLo <= 1);    // the head hardly moves
    CHECK(finWidths.size() >= 2); // the fin turns or narrows, it does not only slide

    // More amount, more sweep.
    RetargetOptions big = swimmer();
    big.amount = 2;
    auto rb = retarget(s, preset("swim"), big);
    REQUIRE(rb);
    double bLo = 1e9, bHi = -1e9;
    for (const Image& f : rb.value().clip.frames) {
        bLo = std::min(bLo, meanRow(f, kFin));
        bHi = std::max(bHi, meanRow(f, kFin));
    }
    CHECK(bHi - bLo > finHi - finLo);
}

TEST_CASE("Swimmer retarget: other motions, one-shots and refusals", "[mocap][swim]") {
    const Image s = fish();
    // A walk has a spine too (head to a foot); it bends little and says which chain it used.
    auto w = retarget(s, preset("walk"), swimmer());
    REQUIRE(w);
    bool said = false;
    for (const std::string& n : w.value().notes) said = said || n.rfind("Spine: ", 0) == 0;
    CHECK(said);
    // A one-shot starts as drawn: its first frame bends nothing.
    RetargetOptions once = swimmer();
    once.loop = false;
    once.frames = 6;
    auto j = retarget(s, preset("swim"), once);
    REQUIRE(j);
    REQUIRE(j.value().clip.frames.size() == 6);
    CHECK(std::abs(meanRow(j.value().clip.frames[0], kFin) - meanRow(j.value().clip.frames[0], kEye) - (meanRow(s, kFin) - meanRow(s, kEye))) < 0.51);
    // Too small to bend.
    Image tiny(6, 3);
    tiny.fillRect({0, 0, 6, 3}, kBody);
    CHECK_FALSE(retarget(tiny, preset("swim"), swimmer()));
}
