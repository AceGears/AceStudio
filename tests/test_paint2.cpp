// Phase 2c painting: masks, tile mode, inks, custom tips, clone, curve, polygon.
#include <catch2/catch_test_macros.hpp>

#include "TestSupport.h"
#include "acestudio/core/Paint.h"

#include <algorithm>
#include <set>

using namespace acestudio;

namespace {
const Rgba8 kRed{255, 0, 0, 255}, kBlue{0, 0, 255, 255};

struct Fx {
    Editor editor;
    LayerId layer;
    explicit Fx(int w = 8, int h = 8) : editor(Document::create(w, h).value()), layer(editor.addLayer("L").value()) {
        editor.history().clear();
    }
    std::unique_ptr<PaintSession> session(PaintTarget t = PaintTarget::Pixels) {
        auto s = PaintSession::begin(editor, layer, 0, "Paint", t);
        REQUIRE(s);
        return std::move(s).value();
    }
    Rgba8 px(int x, int y) const { return editor.document().composite(0).pixel(x, y); }
    Rgba8 raw(int x, int y) const { return editor.document().renderLayer(layer, 0).pixel(x, y); }
};

BrushSettings sized(int n) {
    BrushSettings b;
    b.size = n;
    return b;
}
} // namespace

TEST_CASE("Painting a layer mask hides pixels and undoes exactly", "[paint][mask]") {
    Fx f;
    { auto s = f.session(); for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) s->writePixel(x, y, kRed); REQUIRE(s->commit()); }
    CHECK_FALSE(PaintSession::begin(f.editor, f.layer, 0, "x", PaintTarget::Mask));   // no mask yet
    REQUIRE(f.editor.addLayerMask(f.layer));
    {
        auto s = f.session(PaintTarget::Mask);
        s->writePixel(1, 1, Rgba8{0, 0, 0, 255});        // black hides
        s->writePixel(2, 1, Rgba8{128, 128, 128, 255});  // grey half-hides
        s->eraseStamp(3, 1);                              // eraser hides
        REQUIRE(s->commit());
    }
    CHECK(f.px(1, 1).a == 0);
    CHECK(f.px(2, 1).a == 128);
    CHECK(f.px(3, 1).a == 0);
    CHECK(f.px(4, 1) == kRed);
    CHECK(f.raw(1, 1) == kRed);                           // pixels untouched
    REQUIRE(f.editor.undo());
    CHECK(f.px(1, 1) == kRed);
    REQUIRE(f.editor.redo());
    CHECK(f.px(1, 1).a == 0);
    // White reveals again.
    { auto s = f.session(PaintTarget::Mask); s->writePixel(1, 1, Rgba8{255, 255, 255, 255}); REQUIRE(s->commit()); }
    CHECK(f.px(1, 1) == kRed);
}

TEST_CASE("Adjustment layers can only be painted through their mask", "[paint][mask]") {
    Fx f;
    auto adj = f.editor.addAdjustmentLayer({AdjustmentType::Invert});
    REQUIRE(adj);
    CHECK_FALSE(PaintSession::begin(f.editor, adj.value(), 0, "x"));
    REQUIRE(f.editor.addLayerMask(adj.value()));
    CHECK(PaintSession::begin(f.editor, adj.value(), 0, "x", PaintTarget::Mask));
}

TEST_CASE("Tile mode wraps strokes around the canvas edges", "[paint][tile]") {
    Fx f;
    REQUIRE(f.editor.setTileMode(TileMode::Both));
    auto s = f.session();
    s->setBrush(sized(3));
    s->stamp(0, 0, kRed);                                  // covers -1..1 in both axes
    REQUIRE(s->commit());
    CHECK(f.raw(7, 7) == kRed);
    CHECK(f.raw(0, 7) == kRed);
    CHECK(f.raw(7, 0) == kRed);
    CHECK(f.raw(1, 1) == kRed);
    CHECK(f.raw(2, 2).a == 0);
    // Horizontal only: no vertical wrap.
    Fx g;
    REQUIRE(g.editor.setTileMode(TileMode::Horizontal));
    auto t = g.session();
    t->setBrush(sized(3));
    t->stamp(0, 0, kRed);
    REQUIRE(t->commit());
    CHECK(g.raw(7, 0) == kRed);
    CHECK(g.raw(0, 7).a == 0);
    // A line across the seam continues on the other side.
    Fx h;
    REQUIRE(h.editor.setTileMode(TileMode::Horizontal));
    auto u = h.session();
    drawLine(*u, {6, 3}, {9, 3}, kBlue);
    REQUIRE(u->commit());
    CHECK(h.raw(7, 3) == kBlue);
    CHECK(h.raw(0, 3) == kBlue);
    CHECK(h.raw(1, 3) == kBlue);
}

TEST_CASE("Blend ink composites; simple ink replaces", "[paint][ink]") {
    Fx f;
    { auto s = f.session(); s->writePixel(0, 0, kBlue); s->writePixel(1, 0, kBlue); REQUIRE(s->commit()); }
    auto s = f.session();
    BrushSettings b;
    b.ink = Ink::Blend;
    s->setBrush(b);
    s->writePixel(0, 0, Rgba8{255, 0, 0, 128});
    b.ink = Ink::Simple;
    s->setBrush(b);
    s->writePixel(1, 0, Rgba8{255, 0, 0, 128});
    REQUIRE(s->commit());
    const Rgba8 blended = f.raw(0, 0);
    CHECK(blended.a == 255);
    CHECK(blended.r == 128);
    CHECK(blended.b == 127);
    CHECK(f.raw(1, 0) == (Rgba8{255, 0, 0, 128}));
    // Blend on an empty pixel = the colour itself.
    auto t = f.session();
    b.ink = Ink::Blend;
    t->setBrush(b);
    t->writePixel(5, 5, Rgba8{10, 20, 30, 100});
    REQUIRE(t->commit());
    CHECK(f.raw(5, 5) == (Rgba8{10, 20, 30, 100}));
}

TEST_CASE("Shading ink steps pixels along the ramp once per stroke", "[paint][ink]") {
    Fx f;
    const std::vector<Rgba8> ramp{{20, 20, 20, 255}, {80, 80, 80, 255}, {160, 160, 160, 255}};
    { auto s = f.session(); s->writePixel(0, 0, ramp[0]); s->writePixel(1, 0, ramp[1]); s->writePixel(2, 0, ramp[2]);
      s->writePixel(3, 0, kRed); REQUIRE(s->commit()); }
    auto s = f.session();
    BrushSettings b;
    b.ink = Ink::Shading;
    s->setBrush(b);
    s->setShading(ramp, +1);
    for (int pass = 0; pass < 3; ++pass)                  // scrubbing over a pixel shades it once
        for (int x = 0; x < 5; ++x) s->stamp(x, 0, kBlue);
    REQUIRE(s->commit());
    CHECK(f.raw(0, 0) == ramp[1]);
    CHECK(f.raw(1, 0) == ramp[2]);
    CHECK(f.raw(2, 0) == ramp[2]);                         // end of the ramp: unchanged
    CHECK(f.raw(3, 0) == kRed);                            // not in the ramp
    CHECK(f.raw(4, 0).a == 0);                             // empty stays empty
    auto t = f.session();
    t->setBrush(b);
    t->setShading(ramp, -1);
    t->stamp(1, 0, kBlue);
    REQUIRE(t->commit());
    CHECK(f.raw(1, 0) == ramp[1]);
}

TEST_CASE("Custom brush tips: footprint, colours, files", "[paint][brush]") {
    Image tip(3, 3);
    tip.setPixel(1, 0, kRed);
    tip.setPixel(0, 1, kBlue);
    tip.setPixel(2, 2, kRed);
    Fx f;
    auto s = f.session();
    BrushSettings b;
    b.shape = BrushShape::Custom;
    b.tip = std::make_shared<Image>(tip);
    s->setBrush(b);
    s->stamp(4, 4, Rgba8{0, 255, 0, 255});
    REQUIRE(s->commit());
    CHECK(f.raw(4, 3) == (Rgba8{0, 255, 0, 255}));
    CHECK(f.raw(3, 4) == (Rgba8{0, 255, 0, 255}));
    CHECK(f.raw(5, 5) == (Rgba8{0, 255, 0, 255}));
    CHECK(f.raw(4, 4).a == 0);
    Fx g;
    auto t = g.session();
    b.tipColors = true;
    t->setBrush(b);
    t->stamp(4, 4, Rgba8{0, 255, 0, 255});
    REQUIRE(t->commit());
    CHECK(g.raw(4, 3) == kRed);
    CHECK(g.raw(3, 4) == kBlue);

    Mask sel(8, 8);
    sel.fillRect({3, 3, 3, 3}, true);
    auto fromSel = brushTipFromSelection(g.editor.document().renderLayer(g.layer, 0), sel);
    REQUIRE(fromSel);
    CHECK(fromSel.value() == tip);
    CHECK_FALSE(brushTipFromSelection(g.editor.document().renderLayer(g.layer, 0), Mask(8, 8)));
    TempDir dir;
    REQUIRE(saveBrushTip(tip, dir.path() / "tip.png"));
    auto loaded = loadBrushTip(dir.path() / "tip.png");
    REQUIRE(loaded);
    CHECK(loaded.value() == tip);
    REQUIRE(saveBrushTip(Image(3, 3), dir.path() / "empty.png"));
    CHECK_FALSE(loadBrushTip(dir.path() / "empty.png"));
}

TEST_CASE("Clone stroke copies pixels from the offset source", "[paint][clone]") {
    Fx f;
    { auto s = f.session(); s->writePixel(1, 1, kRed); s->writePixel(2, 1, kBlue); REQUIRE(s->commit()); }
    const Image source = f.editor.document().renderLayer(f.layer, 0);
    auto s = f.session();
    CloneStroke c(*s, source, Point{4, 4});               // destination = source + (4, 4)
    c.moveTo({5, 5});
    c.moveTo({6, 5});
    REQUIRE(s->commit());
    CHECK(f.raw(5, 5) == kRed);
    CHECK(f.raw(6, 5) == kBlue);
    CHECK(f.raw(1, 1) == kRed);                            // source untouched
}

TEST_CASE("Curves are connected, start and end exactly, and bend", "[raster][curve]") {
    const auto pts = curvePoints({0, 0}, {20, 0}, {5, 10}, {15, 10});
    REQUIRE(pts.size() > 20);
    CHECK(pts.front() == Point{0, 0});
    CHECK(pts.back() == Point{20, 0});
    std::set<std::pair<int, int>> seen;
    int maxY = 0;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        maxY = std::max(maxY, pts[i].y);
        CHECK(seen.insert({pts[i].x, pts[i].y}).second);   // no duplicates
        if (i) {
            CHECK(std::abs(pts[i].x - pts[i - 1].x) <= 1);
            CHECK(std::abs(pts[i].y - pts[i - 1].y) <= 1);
        }
    }
    CHECK(maxY >= 7);
    CHECK(maxY <= 8);
    // Degenerate: control points on the line = a straight line.
    const auto straight = curvePoints({0, 3}, {10, 3}, {3, 3}, {7, 3});
    for (const auto& p : straight) CHECK(p.y == 3);
    CHECK(straight.size() == 11);
}

TEST_CASE("Polygons: outline closes, fill covers the interior", "[paint][polygon]") {
    Fx f(10, 10);
    auto s = f.session();
    drawPolygon(*s, {{1, 1}, {8, 1}, {8, 8}, {1, 8}}, false, kRed);
    REQUIRE(s->commit());
    CHECK(f.raw(1, 5) == kRed);                            // closing edge drawn
    CHECK(f.raw(8, 8) == kRed);
    CHECK(f.raw(4, 4).a == 0);
    Fx g(10, 10);
    auto t = g.session();
    drawPolygon(*t, {{1, 1}, {8, 1}, {1, 8}}, true, kBlue);
    REQUIRE(t->commit());
    CHECK(g.raw(2, 2) == kBlue);
    CHECK(g.raw(8, 8).a == 0);
}
