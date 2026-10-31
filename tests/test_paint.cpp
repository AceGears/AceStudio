#include <catch2/catch_test_macros.hpp>

#include "acestudio/core/Paint.h"
#include "acestudio/core/ProjectIO.h"

#include <algorithm>
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
const Rgba8 kRed = Rgba8::opaque(255, 0, 0);
const Rgba8 kBlue = Rgba8::opaque(0, 0, 255);

struct Fixture {
    Editor editor;
    LayerId layer;
    explicit Fixture(int w = 8, int h = 8, ColorMode mode = ColorMode::Rgba)
        : editor(Document::create(w, h, mode).value()), layer(editor.addLayer("L").value()) {
        editor.history().clear();
    }
    std::unique_ptr<PaintSession> session(const std::string& label = "Paint") {
        auto s = PaintSession::begin(editor, layer, 0, label);
        REQUIRE(s);
        return std::move(s).value();
    }
    Rgba8 px(int x, int y) const { return editor.document().composite(0).pixel(x, y); }
    std::size_t count(Rgba8 c) const {
        const Image img = editor.document().composite(0);
        return static_cast<std::size_t>(std::count(img.pixels().begin(), img.pixels().end(), c));
    }
};
} // namespace

// ------------------------------------------------------------ algorithms

TEST_CASE("Bresenham lines are continuous and inclusive", "[raster]") {
    auto pts = linePoints({0, 0}, {5, 2});
    CHECK(pts.front() == Point{0, 0});
    CHECK(pts.back() == Point{5, 2});
    CHECK(pts.size() == 6);
    for (std::size_t i = 1; i < pts.size(); ++i) {
        CHECK(std::abs(pts[i].x - pts[i - 1].x) <= 1);
        CHECK(std::abs(pts[i].y - pts[i - 1].y) <= 1);
    }
    CHECK(linePoints({3, 3}, {3, 3}).size() == 1);
    CHECK(linePoints({0, 5}, {0, 0}).size() == 6);
}

TEST_CASE("Rectangles and ellipses", "[raster]") {
    CHECK(rectFromCorners({5, 1}, {2, 4}) == IntRect{2, 1, 4, 4});
    CHECK(rectOutlinePoints({0, 0, 4, 3}).size() == 10);
    CHECK(rectOutlinePoints({0, 0, 1, 1}).size() == 1);

    const auto filled = ellipsePoints({0, 0, 9, 9}, true);
    const auto outline = ellipsePoints({0, 0, 9, 9}, false);
    CHECK(filled.size() > outline.size());
    // Symmetric about both axes.
    std::set<std::pair<int, int>> set;
    for (auto p : outline) set.insert({p.x, p.y});
    for (auto p : outline) {
        CHECK(set.count({8 - p.x, p.y}));
        CHECK(set.count({p.x, 8 - p.y}));
    }
    // Touches all four edges of its box.
    CHECK(set.count({4, 0}));
    CHECK(set.count({0, 4}));
    CHECK(set.count({8, 4}));
    CHECK(set.count({4, 8}));
}

TEST_CASE("Pixel-perfect detects L corners only", "[raster]") {
    CHECK(pixelPerfectDropIndex({{0, 0}, {1, 0}, {1, 1}}) == 1);
    CHECK(pixelPerfectDropIndex({{0, 0}, {1, 0}, {2, 0}}) == -1);
    CHECK(pixelPerfectDropIndex({{0, 0}, {1, 1}, {2, 2}}) == -1);
}

TEST_CASE("Flood region contiguous / global / tolerance", "[raster]") {
    Image img(5, 1, Rgba8::opaque(10, 10, 10));
    img.setPixel(2, 0, Rgba8::opaque(200, 0, 0));
    CHECK(floodRegion(img, {0, 0}, true, 0).count() == 2);
    CHECK(floodRegion(img, {0, 0}, false, 0).count() == 4);
    CHECK(floodRegion(img, {0, 0}, true, 255).count() == 5);
    Mask limit(5, 1);
    limit.set(4, 0, true);
    CHECK(floodRegion(img, {4, 0}, false, 0, &limit).count() == 1);
    CHECK(floodRegion(img, {9, 9}, true, 0).count() == 0);
    // Large region does not overflow the stack.
    Image big(1024, 1024);
    CHECK(floodRegion(big, {0, 0}, true, 0).count() == 1024u * 1024u);
}

// ------------------------------------------------------------ painting

TEST_CASE("Paint session draws live and commits one exact undo step", "[paint][undo]") {
    Fixture f;
    const auto before = documentToJson(f.editor.document());
    {
        auto s = f.session("Pencil");
        FreehandStroke stroke(*s, kRed, false, false);
        stroke.moveTo({1, 1});
        stroke.moveTo({4, 1});
        CHECK(f.px(3, 1) == kRed);          // visible before commit
        REQUIRE(s->commit());
    }
    CHECK(f.editor.history().size() == 1);
    CHECK(f.editor.history().undoLabel() == "Pencil");
    CHECK(f.count(kRed) == 4);
    REQUIRE(f.editor.undo());
    CHECK(f.count(kRed) == 0);
    CHECK(f.editor.document().cel(f.layer, 0) == nullptr);   // empty layer restored exactly
    CHECK(documentToJson(f.editor.document()) == before);
    REQUIRE(f.editor.redo());
    CHECK(f.count(kRed) == 4);
}

TEST_CASE("Paint session cancel and no-op commit leave no trace", "[paint]") {
    Fixture f;
    {
        auto s = f.session();
        s->stamp(2, 2, kRed);
        s->cancel();
    }
    CHECK(f.editor.document().cel(f.layer, 0) == nullptr);
    {
        auto s = f.session();
        s->stamp(2, 2, kRed);
    }   // destructor cancels
    CHECK(f.editor.document().cel(f.layer, 0) == nullptr);
    {
        auto s = f.session();
        CHECK_FALSE(s->commit());
    }
    CHECK(f.editor.history().size() == 0);
}

TEST_CASE("Small offset cels are restored exactly after painting", "[paint][undo]") {
    Fixture f;
    Document& d = f.editor.mutableDocument();
    REQUIRE(d.setCel(f.layer, 0, Cel{Image(2, 2, kBlue), 5, 5}));
    {
        auto s = f.session();
        s->stamp(0, 0, kRed);
        REQUIRE(s->commit());
    }
    CHECK(f.px(0, 0) == kRed);
    CHECK(f.px(6, 6) == kBlue);
    REQUIRE(f.editor.undo());
    const Cel* c = f.editor.document().cel(f.layer, 0);
    REQUIRE(c);
    CHECK(c->x == 5);
    CHECK(c->image.width() == 2);
}

TEST_CASE("Pixel-perfect freehand removes doubled corners", "[paint]") {
    Fixture f;
    auto s = f.session();
    FreehandStroke stroke(*s, kRed, false, true);
    stroke.moveTo({0, 0});
    stroke.moveTo({1, 0});
    stroke.moveTo({1, 1});   // L-corner at (1,0) is removed
    stroke.moveTo({2, 2});
    REQUIRE(s->commit());
    CHECK(f.px(1, 0).a == 0);
    CHECK(f.px(0, 0) == kRed);
    CHECK(f.px(1, 1) == kRed);
    CHECK(f.count(kRed) == 3);
}

TEST_CASE("Brush size, shape and mirror", "[paint]") {
    Fixture f(9, 9);
    auto s = f.session();
    s->setBrush(brush(3, BrushShape::Square, MirrorMode::Both));
    s->stamp(1, 1, kRed);
    REQUIRE(s->commit());
    CHECK(f.count(kRed) == 36);        // 3x3 in each of four quadrants
    CHECK(f.px(7, 7) == kRed);
    CHECK(f.px(4, 4).a == 0);

    Fixture g(9, 9);
    auto s2 = g.session();
    s2->setBrush(brush(5, BrushShape::Circle, MirrorMode::None));
    s2->stamp(4, 4, kRed);
    REQUIRE(s2->commit());
    CHECK(g.count(kRed) < 25);         // corners trimmed
    CHECK(g.px(4, 4) == kRed);
}

TEST_CASE("Selection and alpha lock constrain painting", "[paint]") {
    Fixture f;
    Mask sel(8, 8);
    sel.fillRect({0, 0, 4, 8}, true);
    {
        auto s = f.session();
        s->setSelection(&sel);
        drawRectangle(*s, {0, 0}, {7, 7}, true, kRed);
        REQUIRE(s->commit());
    }
    CHECK(f.count(kRed) == 32);
    CHECK(f.px(5, 0).a == 0);

    REQUIRE(f.editor.setLayerAlphaLocked(f.layer, true));
    {
        auto s = f.session();
        drawRectangle(*s, {0, 0}, {7, 7}, true, kBlue);
        s->eraseStamp(0, 0);   // erasing is blocked by alpha lock
        REQUIRE(s->commit());
    }
    CHECK(f.count(kBlue) == 32);       // only where pixels already existed
    CHECK(f.px(6, 6).a == 0);
}

TEST_CASE("Locked, hidden and group layers cannot be painted", "[paint]") {
    Fixture f;
    REQUIRE(f.editor.setLayerLocked(f.layer, true));
    CHECK(PaintSession::begin(f.editor, f.layer, 0, "x").error().code == ErrorCode::PermissionDenied);
    REQUIRE(f.editor.setLayerLocked(f.layer, false));
    REQUIRE(f.editor.setLayerVisible(f.layer, false));
    CHECK_FALSE(PaintSession::begin(f.editor, f.layer, 0, "x"));
    const auto g = f.editor.addLayer("G", LayerKind::Group).value();
    CHECK_FALSE(PaintSession::begin(f.editor, g, 0, "x"));
    CHECK_FALSE(PaintSession::begin(f.editor, f.layer, 5, "x"));
}

TEST_CASE("Indexed and grayscale modes constrain colours", "[paint]") {
    Fixture idx(4, 4, ColorMode::Indexed);
    idx.editor.mutableDocument().palette().clear();
    REQUIRE(idx.editor.mutableDocument().palette().add(Rgba8::opaque(0, 0, 0)));
    REQUIRE(idx.editor.mutableDocument().palette().add(Rgba8::opaque(250, 250, 250)));
    {
        auto s = idx.session();
        s->stamp(0, 0, Rgba8::opaque(200, 180, 190));
        REQUIRE(s->commit());
    }
    CHECK(idx.px(0, 0) == Rgba8::opaque(250, 250, 250));

    Fixture gray(4, 4, ColorMode::Grayscale);
    {
        auto s = gray.session();
        s->stamp(0, 0, Rgba8::opaque(0, 255, 0));
        REQUIRE(s->commit());
    }
    CHECK(gray.px(0, 0) == Rgba8::opaque(182, 182, 182));
}

TEST_CASE("Shape tools preview with revertAll", "[paint]") {
    Fixture f;
    auto s = f.session("Line");
    drawLine(*s, {0, 0}, {7, 0}, kRed);
    s->revertAll();                    // mouse moved: redraw the preview
    drawLine(*s, {0, 0}, {0, 7}, kRed);
    REQUIRE(s->commit());
    CHECK(f.count(kRed) == 8);
    CHECK(f.px(7, 0).a == 0);
    CHECK(f.px(0, 7) == kRed);

    auto e = f.session("Ellipse");
    drawEllipse(*e, {0, 0}, {6, 6}, false, kBlue);
    REQUIRE(e->commit());
    CHECK(f.count(kBlue) > 8);
}

TEST_CASE("Fill uses the layer's own pixels and supports undo", "[paint]") {
    Fixture f;
    {
        auto s = f.session();
        drawRectangle(*s, {2, 2}, {5, 5}, false, kBlue);
        REQUIRE(s->commit());
    }
    {
        auto s = f.session("Fill");
        fillRegion(*s, {3, 3}, kRed, true, 0);   // inside the outline
        REQUIRE(s->commit());
    }
    CHECK(f.count(kRed) == 4);
    {
        auto s = f.session("Fill");
        fillRegion(*s, {0, 0}, kRed, true, 0);   // outside
        REQUIRE(s->commit());
    }
    CHECK(f.count(kRed) == 4 + 64 - 16);
    REQUIRE(f.editor.undo());
    CHECK(f.count(kRed) == 4);
}

TEST_CASE("Eyedropper samples merged or single layer", "[paint]") {
    Fixture f;
    {
        auto s = f.session();
        s->stamp(1, 1, kRed);
        REQUIRE(s->commit());
    }
    const auto top = f.editor.addLayer("top").value();
    REQUIRE(f.editor.setLayerOpacity(top, 255));
    CHECK(sampleColor(f.editor.document(), 0, {1, 1}) == kRed);
    CHECK(sampleColor(f.editor.document(), 0, {1, 1}, top).a == 0);
    CHECK(sampleColor(f.editor.document(), 0, {1, 1}, f.layer) == kRed);
}
