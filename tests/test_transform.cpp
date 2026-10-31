#include <catch2/catch_test_macros.hpp>

#include "acestudio/core/Paint.h"
#include "acestudio/core/Transform.h"

#include <set>

using namespace acestudio;

namespace {
[[maybe_unused]] BrushSettings brush(int size, BrushShape shape, MirrorMode mirror, int density = 16) {
    BrushSettings b;
    b.size = size;
    b.shape = shape;
    b.mirror = mirror;
    b.ditherDensity = density;
    return b;
}
} // namespace


namespace {
const Rgba8 R = Rgba8::opaque(255, 0, 0), G = Rgba8::opaque(0, 255, 0), B = Rgba8::opaque(0, 0, 255);

Image asym() {   // 3×2 with distinct corners
    Image img(3, 2);
    img.setPixel(0, 0, R);
    img.setPixel(2, 0, G);
    img.setPixel(0, 1, B);
    return img;
}

std::set<std::uint32_t> colorsOf(const Image& img) {
    std::set<std::uint32_t> s;
    for (auto p : img.pixels())
        if (p.a) s.insert((std::uint32_t{p.r} << 24) | (std::uint32_t{p.g} << 16) | (std::uint32_t{p.b} << 8) | p.a);
    return s;
}

std::size_t opaqueCount(const Image& img) {
    std::size_t n = 0;
    for (auto p : img.pixels()) n += p.a ? 1 : 0;
    return n;
}
} // namespace

TEST_CASE("Flips and right-angle rotations are exact permutations", "[transform]") {
    const Image a = asym();
    CHECK(flipHorizontal(a).pixel(2, 0) == R);
    CHECK(flipVertical(a).pixel(0, 0) == B);
    CHECK(flipHorizontal(flipHorizontal(a)) == a);
    CHECK(flipVertical(flipVertical(a)) == a);

    const Image cw = rotate90(a, true);
    CHECK(cw.width() == 2);
    CHECK(cw.height() == 3);
    CHECK(cw.pixel(1, 0) == R);     // top-left -> top-right
    CHECK(cw.pixel(0, 0) == B);     // bottom-left -> top-left
    CHECK(rotate90(cw, false) == a);
    CHECK(rotate90(rotate90(rotate90(rotate90(a, true), true), true), true) == a);
    CHECK(rotate180(rotate180(a)) == a);
}

TEST_CASE("Nearest-neighbour scaling", "[transform]") {
    const Image a = asym();
    auto up = scaleNearest(a, 6, 4);
    REQUIRE(up);
    CHECK(up.value().pixel(1, 1) == R);
    CHECK(up.value().pixel(5, 0) == G);
    auto down = scaleNearest(up.value(), 3, 2);
    REQUIRE(down);
    CHECK(down.value() == a);   // integer up then down is lossless
    CHECK_FALSE(scaleNearest(a, 0, 5));
    CHECK(colorsOf(scaleNearest(a, 7, 5).value()) == colorsOf(a));
}

TEST_CASE("Scale2x rounds diagonal staircases", "[transform]") {
    // A 2-pixel diagonal: Scale2x should fill the inner corners.
    Image img(2, 2);
    img.setPixel(0, 0, R);
    img.setPixel(1, 1, R);
    const Image s = scale2xSimilar(img, 0);
    CHECK(s.width() == 4);
    // The two empty cells beside the diagonal gain their inner corner, so the
    // staircase becomes a continuous 2px-wide diagonal...
    CHECK(s.pixel(2, 1) == R);
    CHECK(s.pixel(1, 2) == R);
    // ...while the diagonal pixels' outer corners are trimmed.
    CHECK(s.pixel(1, 1).a == 0);
    CHECK(s.pixel(0, 0) == R);
    CHECK(s.pixel(3, 3) == R);
    CHECK(colorsOf(s) == colorsOf(img));
}

TEST_CASE("RotSprite keeps the palette and roughly preserves area", "[transform]") {
    Image img(16, 16);
    img.fillRect({4, 4, 8, 8}, R);
    img.fillRect({6, 6, 4, 4}, G);
    const Image r30 = rotSprite(img, 30.0);
    CHECK(r30.width() == 16);
    CHECK(colorsOf(r30) == colorsOf(img));   // no new colours, none lost
    const double ratio = double(opaqueCount(r30)) / double(opaqueCount(img));
    CHECK(ratio > 0.85);
    CHECK(ratio < 1.15);
    CHECK(rotSprite(img, 0.0) == img);
    CHECK(rotSprite(img, 360.0) == img);
    CHECK(rotSprite(img, 90.0) == rotate90(img, true));
    CHECK(rotSprite(img, -90.0) == rotate90(img, false));
    CHECK(rotSprite(img, 180.0) == rotate180(img));
}

TEST_CASE("Outline outside and inside", "[transform]") {
    Image img(5, 5);
    img.setPixel(2, 2, R);
    const Image out4 = outline(img, B, OutlineMode::Outside, false);
    CHECK(opaqueCount(out4) == 5);          // centre + 4 neighbours
    CHECK(out4.pixel(2, 1) == B);
    CHECK(out4.pixel(1, 1).a == 0);
    const Image out8 = outline(img, B, OutlineMode::Outside, true);
    CHECK(opaqueCount(out8) == 9);
    CHECK(out8.pixel(2, 2) == R);

    Image block(4, 4, R);
    const Image in = outline(block, B, OutlineMode::Inside, false);
    CHECK(in.pixel(0, 0) == B);             // canvas edge counts as outside
    CHECK(in.pixel(1, 1) == R);
}

// ---------------------------------------------------------------- selection / dither

TEST_CASE("Polygon masks", "[raster][selection]") {
    const Mask sq = polygonMask(10, 10, {{1, 1}, {5, 1}, {5, 5}, {1, 5}});
    CHECK(sq.count() == 25);
    CHECK(sq.bounds() == IntRect{1, 1, 5, 5});
    const Mask tri = polygonMask(10, 10, {{0, 0}, {8, 0}, {0, 8}});
    CHECK(tri.get(1, 1));
    CHECK_FALSE(tri.get(7, 7));
    CHECK(tri.get(8, 0));                   // outline always included
    CHECK(polygonMask(4, 4, {{2, 2}}).count() == 1);
}

TEST_CASE("Selection combine modes", "[raster][selection]") {
    Mask a(4, 1), b(4, 1);
    a.set(0, 0, true); a.set(1, 0, true);
    b.set(1, 0, true); b.set(2, 0, true);
    Mask add = a; combineMask(add, b, SelectionOp::Add);
    Mask sub = a; combineMask(sub, b, SelectionOp::Subtract);
    Mask inter = a; combineMask(inter, b, SelectionOp::Intersect);
    Mask rep = a; combineMask(rep, b, SelectionOp::Replace);
    CHECK(add.count() == 3);
    CHECK(sub.count() == 1);
    CHECK(sub.get(0, 0));
    CHECK(inter.count() == 1);
    CHECK(inter.get(1, 0));
    CHECK(rep.count() == 2);
    Mask empty;
    combineMask(empty, b, SelectionOp::Add);
    CHECK(empty.count() == 2);
}

TEST_CASE("Bayer matrix covers 0..15 once per tile", "[raster]") {
    std::set<int> v;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) v.insert(bayer4(x, y));
    CHECK(v.size() == 16);
    CHECK(*v.begin() == 0);
    CHECK(*v.rbegin() == 15);
    CHECK(bayer4(5, 6) == bayer4(1, 2));
}

namespace {
struct PaintFixture {
    Editor editor{Document::create(16, 16).value()};
    LayerId layer = editor.addLayer("L").value();
    std::unique_ptr<PaintSession> session() { return std::move(PaintSession::begin(editor, layer, 0, "t").value()); }
    Image flat() const { return editor.document().composite(0); }
};
} // namespace

TEST_CASE("Ordered gradient uses only the two end colours", "[paint][gradient]") {
    PaintFixture f;
    auto s = f.session();
    drawGradient(*s, {0, 0}, {15, 0}, R, B, GradientShape::Linear, GradientDither::Ordered);
    REQUIRE(s->commit());
    const Image img = f.flat();
    CHECK(colorsOf(img) == std::set<std::uint32_t>{0xFF0000FFu, 0x0000FFFFu});
    CHECK(img.pixel(0, 5) == R);
    CHECK(img.pixel(15, 5) == B);
    // Density of the end colour increases left to right.
    int left = 0, right = 0;
    for (int y = 0; y < 16; ++y) { left += img.pixel(4, y) == B; right += img.pixel(11, y) == B; }
    CHECK(left < right);
}

TEST_CASE("Smooth and radial gradients", "[paint][gradient]") {
    PaintFixture f;
    auto s = f.session();
    drawGradient(*s, {8, 8}, {8, 0}, Rgba8::opaque(0, 0, 0), Rgba8::opaque(200, 200, 200), GradientShape::Radial,
                 GradientDither::Smooth);
    REQUIRE(s->commit());
    const Image img = f.flat();
    CHECK(img.pixel(8, 8).r == 0);
    CHECK(img.pixel(8, 4).r > 80);
    CHECK(img.pixel(8, 4).r < 120);
    CHECK(img.pixel(0, 0).r == 200);
}

TEST_CASE("Dither brush paints a stable pattern", "[paint]") {
    PaintFixture f;
    auto s = f.session();
    s->setBrush(brush(4, BrushShape::Square, MirrorMode::None, 8));
    s->stamp(1, 1, R);   // footprint 0..3 x 0..3 = one Bayer tile
    REQUIRE(s->commit());
    CHECK(opaqueCount(f.flat()) == 8);
    CHECK(f.flat().pixel(0, 0) == R);   // threshold 0 < 8
    CHECK(f.flat().pixel(0, 3).a == 0); // threshold 15 >= 8
}

TEST_CASE("RotSprite expand keeps the corners of a full square", "[transform]") {
    Image img(16, 16, R);
    const Image clipped = rotSprite(img, 45.0);
    const Image grown = rotSprite(img, 45.0, 24, true);
    CHECK(clipped.width() == 16);
    CHECK(grown.width() >= 22);                       // 16·√2 ≈ 22.6
    CHECK(grown.width() == grown.height());
    CHECK((grown.width() - 16) % 2 == 0);             // centred on the same grid
    CHECK(opaqueCount(grown) > opaqueCount(clipped));
    const double ratio = double(opaqueCount(grown)) / 256.0;
    CHECK(ratio > 0.9);
    CHECK(ratio < 1.1);
    CHECK(colorsOf(grown) == colorsOf(img));
    // Right angles on a non-square image swap the dimensions when expanding.
    CHECK(rotSprite(asym(), 90.0, 24, true) == rotate90(asym(), true));
}
