// In-betweens for pixel art (core/include/acestudio/core/Inbetween.h).

#include "acestudio/core/Inbetween.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <set>

using namespace acestudio;
using namespace acestudio::inbetween;

namespace {

const Rgba8 kInk{20, 20, 30, 255}, kRed{200, 60, 50, 255}, kBlue{40, 90, 200, 255}, kSkin{230, 180, 140, 255};

// A small figure: an inked block with a face colour and a blue base.
Image figure(int w, int h, int x, int y) {
    Image img(w, h);
    img.fillRect({x, y, 12, 16}, kInk);
    img.fillRect({x + 1, y + 1, 10, 6}, kSkin);
    img.fillRect({x + 1, y + 7, 10, 8}, kRed);
    img.fillRect({x + 3, y + 11, 6, 3}, kBlue);
    return img;
}

std::set<std::uint32_t> colours(const Image& img) {
    std::set<std::uint32_t> s;
    for (const Rgba8& c : img.pixels())
        if (c.a) s.insert((std::uint32_t(c.r) << 24) | (std::uint32_t(c.g) << 16) | (std::uint32_t(c.b) << 8) | c.a);
    return s;
}

int area(const Image& img) {
    int n = 0;
    for (const Rgba8& c : img.pixels()) n += c.a ? 1 : 0;
    return n;
}

} // namespace

TEST_CASE("The matcher finds how far a figure moved", "[inbetween]") {
    const Image a = figure(64, 40, 6, 10), b = figure(64, 40, 30, 14);
    auto f = estimateFlow(a, b);
    REQUIRE(f);
    int right = 0, total = 0;
    for (int y = 0; y < 40; ++y)
        for (int x = 0; x < 64; ++x) {
            if (!a.pixel(x, y).a) continue;
            ++total;
            const std::size_t i = static_cast<std::size_t>(y) * 64u + static_cast<std::size_t>(x);
            right += (f.value().dx[i] == 24 && f.value().dy[i] == 4) ? 1 : 0;
        }
    INFO(right << " of " << total);
    CHECK(right * 100 >= total * 95);
}

TEST_CASE("An in-between of a moved figure sits halfway, in its own colours, alpha binary", "[inbetween]") {
    const Image a = figure(64, 40, 6, 10), b = figure(64, 40, 30, 14);
    Options o;
    o.intermediates = 1;
    auto r = inbetweenPair(a, b, o);
    REQUIRE(r);
    REQUIRE(r.value().frames.size() == 1);
    const Image& mid = r.value().frames[0];
    const IntRect box = mid.opaqueBounds();
    CHECK(box.x == 18);
    CHECK(box.y == 12);
    CHECK(box.w == 12);
    CHECK(box.h == 16);
    // Exactly the figure, moved: every pixel matches the key moved halfway.
    CHECK(mid == figure(64, 40, 18, 12));
    for (const Rgba8& c : mid.pixels()) CHECK((c.a == 0 || c.a == 255));
    CHECK(std::abs(r.value().areaDeviation[0]) < 0.01);
}

TEST_CASE("Three in-betweens step evenly and keep the palette", "[inbetween]") {
    const Image a = figure(64, 40, 4, 12), b = figure(64, 40, 36, 12);
    Options o;
    o.intermediates = 3;
    auto r = inbetweenPair(a, b, o);
    REQUIRE(r);
    REQUIRE(r.value().frames.size() == 3);
    const auto keys = colours(a);
    for (int k = 0; k < 3; ++k) {
        INFO("in-between " << k);
        const Image& f = r.value().frames[static_cast<std::size_t>(k)];
        CHECK(f.opaqueBounds().x == 4 + 8 * (k + 1));
        CHECK(area(f) == area(a));
        for (auto c : colours(f)) CHECK(keys.count(c) == 1);
    }
}

TEST_CASE("A shape that changes is blended by votes, without new colours or holes", "[inbetween]") {
    // An arm (a bar) swings from horizontal to diagonal down.
    Image a(48, 48), b(48, 48);
    a.fillRect({10, 10, 8, 20}, kRed);
    b.fillRect({10, 10, 8, 20}, kRed);
    a.fillRect({18, 12, 16, 4}, kBlue);
    for (int i = 0; i < 14; ++i) b.fillRect({18 + i, 12 + i, 3, 3}, kBlue);
    Options o;
    o.intermediates = 1;
    auto r = inbetweenPair(a, b, o);
    REQUIRE(r);
    const Image& mid = r.value().frames[0];
    std::set<std::uint32_t> allowed = colours(a);
    for (auto c : colours(b)) allowed.insert(c);
    for (auto c : colours(mid)) CHECK(allowed.count(c) == 1);
    // The body does not move.
    for (int y = 10; y < 30; ++y)
        for (int x = 10; x < 18; ++x) CHECK(mid.pixel(x, y) == kRed);
    // The arm is there, between its two poses in size (64 and 74 pixels):
    // it must not collapse onto the nearest blue, whatever the matcher's
    // random search draws.
    for (std::uint32_t seed = 1; seed <= 8; ++seed) {
        INFO("seed " << seed);
        Options so;
        so.seed = seed;
        auto rs = inbetweenPair(a, b, so);
        REQUIRE(rs);
        int blue = 0;
        for (const Rgba8& c : rs.value().frames[0].pixels()) blue += c == kBlue ? 1 : 0;
        CHECK(blue >= 45);
    }
}

TEST_CASE("Identical keys give identical in-betweens", "[inbetween]") {
    const Image a = figure(40, 30, 10, 8);
    auto r = inbetweenPair(a, a, Options{});
    REQUIRE(r);
    CHECK(r.value().frames[0] == a);
}

TEST_CASE("A sequence splits each key's time and loops back to the first", "[inbetween]") {
    std::vector<Image> keys{figure(64, 40, 4, 12), figure(64, 40, 20, 12), figure(64, 40, 36, 12)};
    Options o;
    o.intermediates = 1;
    auto r = inbetweenSequence(keys, {200, 200, 100}, true, Timing::Split, 0, o);
    REQUIRE(r);
    const auto& s = r.value();
    REQUIRE(s.frames.size() == 6);
    CHECK(s.key == std::vector<int>{0, -1, 1, -1, 2, -1});
    CHECK(s.durations == std::vector<int>{100, 100, 100, 100, 50, 50});
    // The last in-between is between the last key and the first.
    CHECK(s.frames[5].opaqueBounds().x == 20);
    auto open = inbetweenSequence(keys, {200, 200, 100}, false, Timing::Fixed, 80, o);
    REQUIRE(open);
    CHECK(open.value().frames.size() == 5);
    CHECK(open.value().durations == std::vector<int>(5, 80));
}

TEST_CASE("In-betweening refuses bad input and can be cancelled", "[inbetween]") {
    const Image a = figure(40, 30, 4, 4);
    CHECK_FALSE(inbetweenPair(a, Image(41, 30)));
    Options o;
    o.intermediates = 8;
    CHECK_FALSE(inbetweenPair(a, a, o));
    CHECK_FALSE(inbetweenSequence({a}, {100}, false, Timing::Split, 0, Options{}));
    CHECK_FALSE(inbetweenSequence({a, a}, {100}, false, Timing::Split, 0, Options{}));
    Options c;
    c.cancelled = [] { return true; };
    auto r = inbetweenPair(a, figure(40, 30, 10, 4), c);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::Cancelled);
}

TEST_CASE("A frame whose pose changes too much is flagged", "[inbetween]") {
    // A tall figure becomes a wide one of a different shape: no clean
    // in-between exists, and the area check should not be fooled.
    Image a(64, 64), b(64, 64);
    a.fillRect({28, 4, 8, 56}, kRed);
    b.fillRect({4, 52, 56, 8}, kBlue);
    Options o;
    o.areaTolerance = 0.2;
    auto r = inbetweenSequence({a, b}, {100, 100}, false, Timing::Split, 0, o);
    REQUIRE(r);
    CHECK_FALSE(r.value().flagged.empty());
    CHECK_FALSE(r.value().notes.empty());
}

TEST_CASE("In-betweening a layer inserts held frames, keeps other layers and grows the tag", "[inbetween][document]") {
    auto created = Document::create(64, 40);
    REQUIRE(created);
    Document doc = std::move(created).value();
    auto bgLayer = doc.addLayer("background");
    REQUIRE(bgLayer);
    const LayerId bg = bgLayer.value();
    auto fig = doc.addLayer("figure");
    REQUIRE(fig);
    // Three frames: the figure moves; the background stays a blue strip.
    while (doc.frameCount() < 3) REQUIRE(doc.insertFrame(doc.frameCount(), 120));
    for (int f = 0; f < 3; ++f) {
        Image strip(64, 4);
        strip.fill(kBlue);
        REQUIRE(doc.setCel(bg, f, Cel{strip, 0, 36}));
        const Image k = figure(64, 40, 4 + 16 * f, 12);
        const IntRect b = k.opaqueBounds();
        REQUIRE(doc.setCel(fig.value(), f, Cel{k.copy(b), b.x, b.y}));
        REQUIRE(doc.setFrameDuration(f, 120));
    }
    AnimationTag tag;
    tag.name = "walk";
    tag.fromFrame = 0;
    tag.toFrame = 2;
    REQUIRE(doc.addTag(tag));
    Options o;
    o.intermediates = 1;
    auto r = inbetweenLayer(doc, fig.value(), 0, 2, false, Timing::Split, 0, o);
    REQUIRE(r);
    const Document& d = r.value().document;
    REQUIRE(d.frameCount() == 5);
    CHECK(r.value().newFrames == std::vector<int>{1, 3});
    CHECK(d.tags()[0].fromFrame == 0);
    CHECK(d.tags()[0].toFrame == 4);
    std::vector<int> durs;
    for (const Frame& f : d.frames()) durs.push_back(f.durationMs);
    CHECK(durs == std::vector<int>{60, 60, 60, 60, 120});
    for (int f = 0; f < 5; ++f) {
        INFO("frame " << f);
        const Cel* c = d.cel(bg, f);
        REQUIRE(c);
        CHECK(c->y == 36);
        CHECK(c->image.pixel(0, 0) == kBlue);
        const Cel* m = d.cel(fig.value(), f);
        REQUIRE(m);
        CHECK(m->x == 4 + 8 * f);
    }
    // The source document is untouched.
    CHECK(doc.frameCount() == 3);
    // Bad requests.
    CHECK_FALSE(inbetweenLayer(doc, fig.value(), 1, 1, false, Timing::Split, 0, o));
    CHECK_FALSE(inbetweenLayer(doc, 9999, 0, 2, false, Timing::Split, 0, o));
}
