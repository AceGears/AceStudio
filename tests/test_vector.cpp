#include "acestudio/core/Document.h"
#include "acestudio/core/Editor.h"
#include "acestudio/core/ProjectIO.h"
#include "acestudio/core/Vector.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>
#include <set>

using namespace acestudio;

namespace {

const Rgba8 kRed = Rgba8::opaque(220, 40, 40);
const Rgba8 kBlue = Rgba8::opaque(40, 60, 220);
const Rgba8 kInk = Rgba8::opaque(20, 20, 30);

VectorShape box(ShapeKind kind, double x0, double y0, double x1, double y1) {
    VectorShape s;
    s.kind = kind;
    s.points = {VectorPoint{x0, y0}, VectorPoint{x1, y1}};
    s.strokeColor = kInk;
    return s;
}

int countColor(const Image& img, Rgba8 c) {
    int n = 0;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) n += img.pixel(x, y) == c ? 1 : 0;
    return n;
}

std::set<std::uint32_t> colorsOf(const Image& img) {
    std::set<std::uint32_t> out;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) {
            const Rgba8 c = img.pixel(x, y);
            if (c.a) out.insert((static_cast<std::uint32_t>(c.r) << 24) | (static_cast<std::uint32_t>(c.g) << 16) | (static_cast<std::uint32_t>(c.b) << 8) | c.a);
        }
    return out;
}

} // namespace

TEST_CASE("A rectangle's outline and fill are exact pixels", "[vector]") {
    VectorShape r = box(ShapeKind::Rectangle, 2, 3, 9, 7);   // 8 x 5
    r.fill = FillStyle::Solid;
    r.fillColor = kRed;
    const Image img = vectors::rasterize({r}, 16, 12);
    CHECK(countColor(img, kInk) == 2 * 8 + 2 * 3);   // the outline
    CHECK(countColor(img, kRed) == 6 * 3);           // the inside
    CHECK(img.pixel(1, 3).a == 0);
    CHECK(img.pixel(2, 3) == kInk);
    CHECK(img.pixel(9, 7) == kInk);
    CHECK(img.pixel(10, 7).a == 0);
    // The outline is one connected walk around the box.
    const auto line = vectors::outline(r);
    REQUIRE(line.size() == 22);
    for (std::size_t i = 0; i < line.size(); ++i) {
        const Point a = line[i], b = line[(i + 1) % line.size()];
        CHECK(std::abs(a.x - b.x) + std::abs(a.y - b.y) == 1);
    }
}

TEST_CASE("Strokes: width, dashes and dots", "[vector]") {
    VectorShape line;
    line.points = {VectorPoint{2, 5}, VectorPoint{29, 5}};   // 28 pixels long
    line.strokeColor = kInk;
    SECTION("solid") { CHECK(countColor(vectors::rasterize({line}, 32, 10), kInk) == 28); }
    SECTION("three wide") {
        line.width = 3;
        const Image img = vectors::rasterize({line}, 32, 10);
        CHECK(countColor(img, kInk) == 3 * 30);   // the pen overhangs each end by one
        CHECK(img.pixel(10, 4) == kInk);
        CHECK(img.pixel(10, 6) == kInk);
        CHECK(img.pixel(10, 7).a == 0);
    }
    SECTION("dashed 4 on, 2 off") {
        line.stroke = StrokeStyle::Dashed;
        const Image img = vectors::rasterize({line}, 32, 10);
        CHECK(countColor(img, kInk) == 20);   // 28 px: four periods of 6 (16 on), then 4 on
        CHECK(img.pixel(2, 5) == kInk);
        CHECK(img.pixel(5, 5) == kInk);
        CHECK(img.pixel(6, 5).a == 0);
        CHECK(img.pixel(7, 5).a == 0);
        CHECK(img.pixel(8, 5) == kInk);
    }
    SECTION("dotted") {
        line.stroke = StrokeStyle::Dotted;
        const Image img = vectors::rasterize({line}, 32, 10);
        CHECK(countColor(img, kInk) == 14);
        CHECK(img.pixel(2, 5) == kInk);
        CHECK(img.pixel(3, 5).a == 0);
    }
    SECTION("no stroke draws nothing") {
        line.stroke = StrokeStyle::None;
        CHECK(countColor(vectors::rasterize({line}, 32, 10), kInk) == 0);
    }
}

TEST_CASE("Paths: curves stay connected and closed paths fill", "[vector]") {
    VectorShape p;
    p.points = {VectorPoint{4, 20}, VectorPoint{28, 20}};
    p.points[0].outX = 6;
    p.points[0].outY = -16;
    p.points[1].inX = -6;
    p.points[1].inY = -16;
    p.strokeColor = kInk;
    const auto curve = vectors::outline(p);
    REQUIRE(curve.size() > 24);
    CHECK(curve.front() == Point{4, 20});
    CHECK(curve.back() == Point{28, 20});
    int top = 20;
    for (std::size_t i = 1; i < curve.size(); ++i) {
        CHECK(std::max(std::abs(curve[i].x - curve[i - 1].x), std::abs(curve[i].y - curve[i - 1].y)) == 1);
        top = std::min(top, curve[i].y);
    }
    CHECK(top < 12);   // the handles pull the curve up

    // A closed triangle: filled inside, and an open one fills as if closed.
    VectorShape tri;
    tri.points = {VectorPoint{2, 2}, VectorPoint{20, 2}, VectorPoint{2, 20}};
    tri.closed = true;
    tri.strokeColor = kInk;
    tri.fill = FillStyle::Solid;
    tri.fillColor = kRed;
    const Image closed = vectors::rasterize({tri}, 24, 24);
    CHECK(closed.pixel(5, 5) == kRed);
    CHECK(closed.pixel(18, 18).a == 0);
    CHECK(closed.pixel(11, 11) == kInk);   // on the closing edge
    tri.closed = false;
    const Image open = vectors::rasterize({tri}, 24, 24);
    CHECK(open.pixel(5, 5) == kRed);
    CHECK(open.pixel(16, 6).a != 0);
    CHECK(countColor(open, kInk) < countColor(closed, kInk));   // no closing stroke
}

TEST_CASE("Gradient fills: dithered ones use only the two colours", "[vector]") {
    VectorShape r = box(ShapeKind::Rectangle, 0, 0, 31, 7);
    r.stroke = StrokeStyle::None;
    r.fill = FillStyle::Linear;
    r.fillColor = kRed;
    r.fillColor2 = kBlue;
    r.fromX = 0;
    r.toX = 31;
    const Image dith = vectors::rasterize({r}, 32, 8);
    CHECK(colorsOf(dith).size() == 2);
    CHECK(dith.pixel(0, 0) == kRed);
    CHECK(dith.pixel(31, 7) == kBlue);
    int leftBlue = 0, rightBlue = 0;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            leftBlue += dith.pixel(x, y) == kBlue ? 1 : 0;
            rightBlue += dith.pixel(31 - x, y) == kBlue ? 1 : 0;
        }
    CHECK(leftBlue < rightBlue);
    r.dither = false;
    CHECK(colorsOf(vectors::rasterize({r}, 32, 8)).size() > 8);   // smooth: mixed colours
    r.fill = FillStyle::Radial;
    r.dither = true;
    r.fromX = 16;
    r.fromY = 4;
    r.toX = 16;
    r.toY = 0;
    const Image radial = vectors::rasterize({r}, 32, 8);
    CHECK(radial.pixel(16, 4) == kRed);
    CHECK(radial.pixel(0, 0) == kBlue);
}

TEST_CASE("Ellipses are symmetric and later shapes draw on top", "[vector]") {
    VectorShape e = box(ShapeKind::Ellipse, 1, 1, 14, 10);
    e.fill = FillStyle::Solid;
    e.fillColor = kRed;
    const Image img = vectors::rasterize({e}, 16, 12);
    for (int y = 0; y < 12; ++y)
        for (int x = 0; x < 16; ++x) CHECK(img.pixel(x, y) == img.pixel(15 - x, y));
    VectorShape dot = box(ShapeKind::Rectangle, 7, 5, 8, 6);
    dot.strokeColor = kBlue;
    const Image both = vectors::rasterize({e, dot}, 16, 12);
    CHECK(both.pixel(7, 5) == kBlue);
    CHECK(vectors::rasterize({dot, e}, 16, 12).pixel(7, 5) == kRed);
}

TEST_CASE("Vector shapes round-trip through JSON and bad ones are refused", "[vector]") {
    VectorShape p;
    p.points = {VectorPoint{1.5, 2, 0, 0, 3, -1}, VectorPoint{9, 4, -2, 1, 0, 0}, VectorPoint{4, 9}};
    p.closed = true;
    p.stroke = StrokeStyle::Dashed;
    p.width = 2;
    p.dash = 5;
    p.gap = 3;
    p.fill = FillStyle::Radial;
    p.fillColor = kRed;
    p.fillColor2 = Rgba8{10, 20, 30, 128};
    p.dither = false;
    p.fromX = 4;
    p.fromY = 5;
    p.toX = 9;
    p.toY = 5;
    const std::vector<VectorShape> shapes{p, box(ShapeKind::Ellipse, 0, 0, 5, 5)};
    auto back = vectors::fromJson(vectors::toJson(shapes));
    REQUIRE(back);
    CHECK(back.value() == shapes);

    CHECK_FALSE(vectors::fromJson(nlohmann::json::parse(R"([{"kind":"star","points":[]}])")));
    CHECK_FALSE(vectors::fromJson(nlohmann::json::parse(R"([{"kind":"path","points":[{"x":1,"y":1}]}])")));   // one anchor
    CHECK_FALSE(vectors::fromJson(nlohmann::json::parse(R"([{"kind":"rectangle","points":[{"x":0,"y":0},{"x":4,"y":4}],"stroke":{"width":99}}])")));
    CHECK_FALSE(vectors::fromJson(nlohmann::json::parse(R"([{"kind":"ellipse","points":[{"x":0,"y":0},{"x":4,"y":4}],"fill":{"style":"plaid"}}])")));
    CHECK_FALSE(vectors::fromJson(nlohmann::json::parse(R"({"kind":"path"})")));
}

TEST_CASE("Editor: vector shapes redraw the cel, undo, follow frames and layers", "[vector]") {
    auto doc = Document::create(32, 24).value();
    const LayerId l = doc.addLayer("Shapes").value();
    Editor ed(std::move(doc));
    VectorShape r = box(ShapeKind::Rectangle, 4, 4, 11, 9);
    r.fill = FillStyle::Solid;
    r.fillColor = kRed;
    REQUIRE(ed.setVectorShapes(l, 0, {r}, "Add Rectangle"));
    REQUIRE(ed.document().vectorShapes(l, 0));
    const Image drawn = ed.document().composite(0);
    CHECK(drawn.pixel(4, 4) == kInk);
    CHECK(drawn.pixel(6, 6) == kRed);
    CHECK(ed.document().validate());

    // Editing a shape redraws the whole cel from the shapes.
    VectorShape moved = r;
    moved.points = {VectorPoint{14, 4}, VectorPoint{21, 9}};
    REQUIRE(ed.setVectorShapes(l, 0, {moved}, "Move Rectangle"));
    CHECK(ed.document().composite(0).pixel(6, 6).a == 0);
    CHECK(ed.document().composite(0).pixel(16, 6) == kRed);
    ed.undo();
    CHECK(ed.document().composite(0) == drawn);
    CHECK(*ed.document().vectorShapes(l, 0) == std::vector<VectorShape>{r});
    ed.undo();
    CHECK_FALSE(ed.document().vectorShapes(l, 0));
    CHECK(ed.document().composite(0).pixel(6, 6).a == 0);
    ed.redo();
    CHECK(ed.document().composite(0) == drawn);

    // Shapes move with their frame, and a deleted frame's shapes come back on undo.
    REQUIRE(ed.insertFrame(0, 100));
    CHECK_FALSE(ed.document().vectorShapes(l, 0));
    REQUIRE(ed.document().vectorShapes(l, 1));
    REQUIRE(ed.removeFrame(1));
    CHECK(ed.document().vectorCels().empty());
    ed.undo();
    CHECK(ed.document().vectorShapes(l, 1));

    // Deleting the layer drops them; undo brings them back.
    REQUIRE(ed.removeLayer(l));
    CHECK(ed.document().vectorCels().empty());
    ed.undo();
    CHECK(ed.document().vectorShapes(l, 1));
    CHECK(ed.document().validate());

    // Rasterize keeps the pixels and drops the shapes.
    const Image before = ed.document().composite(1);
    REQUIRE(ed.rasterizeVectorShapes(l, 1));
    CHECK_FALSE(ed.document().vectorShapes(l, 1));
    CHECK(ed.document().composite(1) == before);
    CHECK_FALSE(ed.rasterizeVectorShapes(l, 1));
    ed.undo();
    CHECK(ed.document().vectorShapes(l, 1));

    // Refused: bad shapes, a group layer, a linked cel.
    VectorShape broken = r;
    broken.width = 0;
    CHECK_FALSE(ed.setVectorShapes(l, 1, {broken}, "Bad"));
    const LayerId g = ed.addLayer("Group", LayerKind::Group).value();
    CHECK_FALSE(ed.setVectorShapes(g, 0, {r}, "Group"));
    REQUIRE(ed.linkCels(l, {0, 1}));
    CHECK_FALSE(ed.setVectorShapes(l, 0, {r}, "Linked"));
}

// LOCAL-SKIP-BEGIN (needs ProjectIO and miniz)
TEST_CASE("Projects save vector shapes as schema 5", "[vector][project]") {
    auto doc = Document::create(32, 24).value();
    const LayerId l = doc.addLayer("Shapes").value();
    Editor ed(std::move(doc));
    VectorShape e = box(ShapeKind::Ellipse, 3, 3, 20, 14);
    e.fill = FillStyle::Linear;
    e.fillColor = kRed;
    e.fillColor2 = kBlue;
    e.toX = 20;
    REQUIRE(ed.setVectorShapes(l, 0, {e}, "Add Ellipse"));
    CHECK(requiredSchemaVersion(ed.document()) == 5);
    auto bytes = serializeProject(ed.document());
    REQUIRE(bytes);
    auto back = deserializeProject(bytes.value());
    REQUIRE(back);
    REQUIRE(back.value().vectorShapes(l, 0));
    CHECK(*back.value().vectorShapes(l, 0) == std::vector<VectorShape>{e});
    CHECK(back.value().composite(0) == ed.document().composite(0));
    REQUIRE(ed.rasterizeVectorShapes(l, 0));
    CHECK(requiredSchemaVersion(ed.document()) < 5);
}
// LOCAL-SKIP-END
