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

namespace {

const Rgba8 kLower{200, 90, 60, 255}, kUpper{240, 200, 80, 255};

// The fish with a fin under its belly and one on its back.
Image finnedFish() {
    Image img = fish();
    img.fillRect({18, 30, 5, 7}, kLower);
    img.fillRect({26, 4, 6, 6}, kUpper);
    return img;
}

double meanCol(const Image& f, Rgba8 c) {
    double sum = 0;
    int n = 0;
    for (int y = 0; y < f.height(); ++y)
        for (int x = 0; x < f.width(); ++x)
            if (f.pixel(x, y) == c) {
                sum += x;
                ++n;
            }
    return n ? sum / n : -1;
}

} // namespace

TEST_CASE("A motion's fin joints paddle a fish's fins", "[mocap][swim]") {
    const Image s = finnedFish();
    const Motion swim = preset("swim");
    auto r = retarget(s, swim, swimmer());
    REQUIRE(r);
    const auto& fr = r.value().clip.frames;
    REQUIRE(fr.size() == 10);
    bool said = false;
    for (const std::string& n : r.value().notes) {
        INFO(n);
        said = said || (n.rfind("Fins: ", 0) == 0 && n.find("PectoralFinL") != std::string::npos && n.find("DorsalFin") != std::string::npos);
        CHECK(n.find("only ride the body") == std::string::npos);
    }
    CHECK(said);

    // The lower fin sweeps toward the tail (on the right) when the motion's
    // fin does (its tip further behind its root; the skeleton faces +Z).
    const int root = swim.find("PectoralFinL"), tip = swim.find("PectoralFinL_end");
    REQUIRE(root >= 0);
    REQUIRE(tip >= 0);
    double lowLo = 1e9, lowHi = -1e9, upLo = 1e9, upHi = -1e9, backMost = 1e9, foreMost = -1e9;
    double colBack = 0, colFore = 0;
    const int lower0 = [&] {
        int n = 0;
        for (const Rgba8& p : s.pixels()) n += p == kLower ? 1 : 0;
        return n;
    }();
    for (std::size_t f = 0; f < fr.size(); ++f) {
        const double lo = meanCol(fr[f], kLower), up = meanCol(fr[f], kUpper);
        REQUIRE(lo >= 0);
        REQUIRE(up >= 0);
        lowLo = std::min(lowLo, lo);
        lowHi = std::max(lowHi, lo);
        upLo = std::min(upLo, up);
        upHi = std::max(upHi, up);
        const std::vector<Vec3> w = jointPositions(swim, 0.08 * static_cast<double>(f));
        const double dz = w[static_cast<std::size_t>(tip)].z - w[static_cast<std::size_t>(root)].z;
        if (dz < backMost) {
            backMost = dz;
            colBack = lo;
        }
        if (dz > foreMost) {
            foreMost = dz;
            colFore = lo;
        }
        int n = 0;
        for (const Rgba8& p : fr[f].pixels()) n += p == kLower ? 1 : 0;
        CHECK(std::abs(n - lower0) * 4 <= lower0);   // it turns; it is not redrawn
    }
    CHECK(lowHi - lowLo >= 1.0);   // the lower fin paddles
    CHECK(lowHi - lowLo <= 6.0);
    CHECK(colBack > colFore + 0.5);
    CHECK(upHi - upLo >= 0.2);     // the back fin a little
    CHECK(upHi - upLo < lowHi - lowLo);

    // A one-shot starts as drawn.
    RetargetOptions once = swimmer();
    once.loop = false;
    auto o = retarget(s, swim, once);
    REQUIRE(o);
    const Image& f0 = o.value().clip.frames[0];
    CHECK(std::abs((meanCol(f0, kLower) - meanCol(f0, kEye)) - (meanCol(s, kLower) - meanCol(s, kEye))) < 0.01);
    CHECK(std::abs((meanRow(f0, kUpper) - meanRow(f0, kEye)) - (meanRow(s, kUpper) - meanRow(s, kEye))) < 0.01);

    // A motion with no fin joints: the fins ride the body as before, and a note says so.
    auto w = retarget(s, preset("walk"), swimmer());
    REQUIRE(w);
    bool riding = false;
    for (const std::string& n : w.value().notes) riding = riding || n.find("fin(s) only ride the body") != std::string::npos;
    CHECK(riding);
}

TEST_CASE("A fin along the belly bends with the body's columns", "[mocap][swim]") {
    // A long low fin under the rear of the body.
    Image s = fish();
    s.fillRect({24, 28, 18, 5}, kLower);
    // The swim preset without its fin joints: nothing paddles, the fins only ride.
    Motion swim = preset("swim");
    for (Joint& j : swim.joints)
        if (j.name.find("Pectoral") != std::string::npos || j.name.find("Dorsal") != std::string::npos) j.name = "X" + std::to_string(&j - swim.joints.data());
    auto r = retarget(s, swim, swimmer());
    REQUIRE(r);
    // The fin's lowest row at its first and last columns: a rigid fin keeps
    // their difference; one that follows the body's columns does not.
    auto slope = [&](const Image& f) {
        int x0 = f.width(), x1 = -1;
        for (int y = 0; y < f.height(); ++y)
            for (int x = 0; x < f.width(); ++x)
                if (f.pixel(x, y) == kLower) {
                    x0 = std::min(x0, x);
                    x1 = std::max(x1, x);
                }
        auto bottom = [&](int x) {
            int b = -1;
            for (int y = 0; y < f.height(); ++y)
                if (f.pixel(x, y) == kLower) b = y;
            return b;
        };
        return x1 < 0 ? 0 : bottom(x1) - bottom(x0);
    };
    int lo = 1000, hi = -1000;
    for (const Image& f : r.value().clip.frames) {
        lo = std::min(lo, slope(f));
        hi = std::max(hi, slope(f));
    }
    CHECK(hi - lo >= 2);
}

TEST_CASE("A fin swung toward the viewer is drawn shorter", "[mocap][swim]") {
    const Image s = finnedFish();
    // The preset's pectoral fins also swing out to the side and back (a
    // turn about the up axis), once a beat.
    Motion m = preset("swim");
    for (const char* name : {"PectoralFinL", "PectoralFinR"}) {
        const int j = m.find(name);
        REQUIRE(j >= 0);
        const Joint& fin = m.joints[static_cast<std::size_t>(j)];
        int yAt = -1;
        for (std::size_t c = 0; c < fin.channels.size(); ++c)
            if (fin.channels[c] == Channel::Yrotation) yAt = fin.firstChannel + static_cast<int>(c);
        REQUIRE(yAt >= 0);
        const double side = std::string(name) == "PectoralFinL" ? 1 : -1;
        for (int f = 0; f < m.frameCount; ++f)
            m.values[static_cast<std::size_t>(f) * static_cast<std::size_t>(m.channelCount) + static_cast<std::size_t>(yAt)] +=
                side * 70 * (0.5 - 0.5 * std::cos(2 * 3.14159265358979 * f / m.frameCount));
    }
    auto count = [](const Image& f, Rgba8 c) {
        int n = 0;
        for (const Rgba8& p : f.pixels()) n += p == c ? 1 : 0;
        return n;
    };
    const int lower0 = count(s, kLower), upper0 = count(s, kUpper);
    auto fewest = [&](const RetargetResult& res, Rgba8 c) {
        int n = 1 << 30;
        for (const Image& f : res.clip.frames) n = std::min(n, count(f, c));
        return n;
    };
    auto r = retarget(s, m, swimmer());
    REQUIRE(r);
    bool said = false;
    for (const std::string& n : r.value().notes) {
        INFO(n);
        said = said || (n.find("A fin swings toward or away from the viewer") != std::string::npos && n.find("% shorter") != std::string::npos);
    }
    CHECK(said);
    const int low = fewest(r.value(), kLower);
    CHECK(low * 100 <= lower0 * 85);             // the lower fin shortens
    CHECK(low * 100 >= lower0 * 40);             // never under half its length (and a turn's rounding)
    CHECK(fewest(r.value(), kUpper) * 4 >= upper0 * 3);   // the back fin does not
    int most = 0;
    for (const Image& f : r.value().clip.frames) most = std::max(most, count(f, kLower));
    CHECK(most * 100 >= lower0 * 90);            // at its longest it is as drawn

    // Half the amount: half the shortening. The plain preset: none, and no note.
    RetargetOptions half = swimmer();
    half.amount = 0.5;
    auto h = retarget(s, m, half);
    REQUIRE(h);
    CHECK(fewest(h.value(), kLower) > low);
    auto plain = retarget(s, preset("swim"), swimmer());
    REQUIRE(plain);
    for (const std::string& n : plain.value().notes) CHECK(n.find("shorter") == std::string::npos);
    CHECK(fewest(plain.value(), kLower) * 100 >= lower0 * 75);
}
