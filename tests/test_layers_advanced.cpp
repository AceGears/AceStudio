// Layer masks, clipping masks, adjustment layers and tile mode (schema 2).
#include <catch2/catch_test_macros.hpp>

#include "acestudio/core/Adjust.h"
#include "acestudio/core/Document.h"
#include "acestudio/core/Editor.h"
#include "acestudio/core/ProjectIO.h"

using namespace acestudio;

namespace {
Document makeDoc(int w = 4, int h = 4) {
    auto d = Document::create(w, h);
    REQUIRE(d);
    return std::move(d).value();
}
const Rgba8 kRed{255, 0, 0, 255}, kBlue{0, 0, 255, 255}, kGreen{0, 255, 0, 255};

Image maskImage(int w, int h, std::uint8_t v) { return Image(w, h, Rgba8{v, v, v, 255}); }
} // namespace

TEST_CASE("Layer mask hides and partially hides pixels", "[layers][mask]") {
    Document d = makeDoc();
    const LayerId l = d.addLayer("Paint").value();
    REQUIRE(d.setCel(l, 0, Cel{Image(4, 4, kRed), 0, 0}));
    CHECK_FALSE(d.setMaskCel(l, 0, Cel{maskImage(4, 4, 0), 0, 0}));   // no mask yet
    d.layer(l)->hasMask = true;
    Image m = maskImage(4, 4, 255);
    m.setPixel(0, 0, {0, 0, 0, 255});
    m.setPixel(1, 0, {128, 128, 128, 255});
    REQUIRE(d.setMaskCel(l, 0, Cel{m, 0, 0}));
    const Image out = d.composite(0);
    CHECK(out.pixel(0, 0).a == 0);
    CHECK(out.pixel(1, 0).a == 128);
    CHECK(out.pixel(2, 0) == kRed);
    CHECK(d.maskValue(l, 0, 1, 0) == 128);
    // Disabled mask: everything shows.
    d.layer(l)->maskEnabled = false;
    CHECK(d.composite(0).pixel(0, 0) == kRed);
    CHECK(d.maskValue(l, 0, 0, 0) == 255);
    // Mask cels outside their rectangle count as white.
    d.layer(l)->maskEnabled = true;
    REQUIRE(d.setMaskCel(l, 0, Cel{maskImage(1, 1, 0), 3, 3}));
    CHECK(d.composite(0).pixel(0, 0) == kRed);
    CHECK(d.composite(0).pixel(3, 3).a == 0);
    CHECK(d.renderMask(l, 0).pixel(3, 3).r == 0);
    CHECK(d.renderMask(l, 0).pixel(0, 0).r == 255);
    CHECK(d.validate());
}

TEST_CASE("Group mask applies to the composited group", "[layers][mask]") {
    Document d = makeDoc();
    const LayerId g = d.addLayer("G", LayerKind::Group).value();
    const LayerId a = d.addLayer("A", LayerKind::Raster, g).value();
    REQUIRE(d.setCel(a, 0, Cel{Image(4, 4, kBlue), 0, 0}));
    d.layer(g)->hasMask = true;
    REQUIRE(d.setMaskCel(g, 0, Cel{maskImage(2, 4, 0), 0, 0}));
    const Image out = d.composite(0);
    CHECK(out.pixel(0, 1).a == 0);
    CHECK(out.pixel(3, 1) == kBlue);
}

TEST_CASE("Clipping masks follow the base layer's alpha", "[layers][clip]") {
    Document d = makeDoc();
    const LayerId base = d.addLayer("Base").value();
    Image b(4, 4);
    b.fillRect({0, 0, 2, 4}, kRed);   // left half opaque
    REQUIRE(d.setCel(base, 0, Cel{b, 0, 0}));
    const LayerId clip = d.addLayer("Shade").value();
    REQUIRE(d.setCel(clip, 0, Cel{Image(4, 4, kBlue), 0, 0}));
    d.layer(clip)->clipped = true;
    Image out = d.composite(0);
    CHECK(out.pixel(0, 0) == kBlue);
    CHECK(out.pixel(3, 0).a == 0);          // clipped away where the base is empty
    // A second clipped layer uses the same base.
    const LayerId clip2 = d.addLayer("Line").value();
    Image c2(4, 4);
    c2.setPixel(1, 1, kGreen);
    c2.setPixel(3, 3, kGreen);
    REQUIRE(d.setCel(clip2, 0, Cel{c2, 0, 0}));
    d.layer(clip2)->clipped = true;
    out = d.composite(0);
    CHECK(out.pixel(1, 1) == kGreen);
    CHECK(out.pixel(3, 3).a == 0);
    // Hidden base hides the clipped layers too.
    d.layer(base)->visible = false;
    CHECK(d.composite(0).pixel(0, 0).a == 0);
    // A clipped layer at the bottom of a group has no base and draws normally.
    Document e = makeDoc();
    const LayerId only = e.addLayer("Only").value();
    REQUIRE(e.setCel(only, 0, Cel{Image(4, 4, kRed), 0, 0}));
    e.layer(only)->clipped = true;
    CHECK(e.composite(0).pixel(3, 3) == kRed);
}

TEST_CASE("Adjustment math", "[layers][adjust]") {
    const Rgba8 c{200, 100, 50, 128};
    CHECK(adjustPixel(c, {AdjustmentType::Invert}) == Rgba8{55, 155, 205, 128});
    const Rgba8 t = adjustPixel(c, {AdjustmentType::Threshold, 120});
    CHECK(t.r == 0); CHECK(t.a == 128);   // luma of (200,100,50) is ~118
    CHECK(adjustPixel(Rgba8{255, 255, 255, 255}, {AdjustmentType::Threshold, 120}).r == 255);
    const Rgba8 p = adjustPixel(Rgba8{100, 160, 30, 255}, {AdjustmentType::Posterize, 2});
    CHECK(p == Rgba8{0, 255, 0, 255});
    CHECK(adjustPixel(c, {AdjustmentType::BrightnessContrast, 0, 0}) == c);
    CHECK(adjustPixel(Rgba8{100, 100, 100, 255}, {AdjustmentType::BrightnessContrast, 100, 0}).r == 255);
    CHECK(adjustPixel(c, {AdjustmentType::HueSaturation, 0, 0, 0}) == c);
    const Rgba8 grey = adjustPixel(Rgba8{255, 0, 0, 255}, {AdjustmentType::HueSaturation, 0, -100, 0});
    CHECK(grey.r == grey.g); CHECK(grey.g == grey.b);
    const Rgba8 shifted = adjustPixel(Rgba8{255, 0, 0, 255}, {AdjustmentType::HueSaturation, 120, 0, 0});
    CHECK(shifted == Rgba8{0, 255, 0, 255});
    CHECK(validateAdjustment({AdjustmentType::Posterize, 1}).ok() == false);
    CHECK(validateAdjustment({AdjustmentType::HueSaturation, 200}).ok() == false);
}

TEST_CASE("Adjustment layers act on what is below them, with opacity, mask and clipping", "[layers][adjust]") {
    Document d = makeDoc();
    const LayerId below = d.addLayer("Below").value();
    REQUIRE(d.setCel(below, 0, Cel{Image(4, 4, Rgba8{200, 100, 50, 255}), 0, 0}));
    const LayerId adj = d.addLayer("Invert", LayerKind::Adjustment).value();
    d.layer(adj)->adjustment = {AdjustmentType::Invert};
    const LayerId above = d.addLayer("Above").value();
    Image a(4, 4);
    a.setPixel(3, 3, kRed);
    REQUIRE(d.setCel(above, 0, Cel{a, 0, 0}));
    Image out = d.composite(0);
    CHECK(out.pixel(0, 0) == Rgba8{55, 155, 205, 255});
    CHECK(out.pixel(3, 3) == kRed);            // layers above are not adjusted
    d.layer(adj)->opacity = 128;
    out = d.composite(0);
    CHECK(out.pixel(0, 0).r > 120);
    CHECK(out.pixel(0, 0).r < 135);
    d.layer(adj)->opacity = 255;
    d.layer(adj)->hasMask = true;
    REQUIRE(d.setMaskCel(adj, 0, Cel{maskImage(2, 4, 0), 0, 0}));
    out = d.composite(0);
    CHECK(out.pixel(0, 0) == Rgba8{200, 100, 50, 255});   // masked out
    CHECK(out.pixel(3, 0) == Rgba8{55, 155, 205, 255});
    // Transparent pixels stay transparent.
    d.clearCel(below, 0);
    CHECK(d.composite(0).pixel(0, 0).a == 0);
    // Adjustment layers have no cels.
    CHECK_FALSE(d.setCel(adj, 0, Cel{Image(1, 1, kRed), 0, 0}));
    // Invalid parameters fail validation.
    d.layer(adj)->adjustment = {AdjustmentType::Posterize, 100};
    CHECK_FALSE(d.validate());
}

TEST_CASE("Mask cels follow frame insert, duplicate, move and remove", "[layers][mask]") {
    Document d = makeDoc();
    const LayerId l = d.addLayer("L").value();
    d.layer(l)->hasMask = true;
    REQUIRE(d.setMaskCel(l, 0, Cel{maskImage(4, 4, 10), 0, 0}));
    REQUIRE(d.insertFrame(0));                 // mask moves to frame 1
    CHECK_FALSE(d.maskCel(l, 0));
    REQUIRE(d.maskCel(l, 1));
    REQUIRE(d.duplicateFrame(1));               // copy on frame 2
    REQUIRE(d.maskCel(l, 2));
    REQUIRE(d.moveFrame(2, 0));
    CHECK(d.maskCel(l, 0));
    REQUIRE(d.removeFrame(0));
    CHECK(d.maskCels().size() == 1);
    CHECK(d.validate());
    REQUIRE(d.removeLayer(l));
    CHECK(d.maskCels().empty());
}

TEST_CASE("Schema 2 round trip: masks, clipping, adjustments, tile mode", "[layers][project]") {
    Document d = makeDoc(8, 8);
    const LayerId base = d.addLayer("Base").value();
    REQUIRE(d.setCel(base, 0, Cel{Image(8, 8, kRed), 0, 0}));
    const LayerId clip = d.addLayer("Clip").value();
    REQUIRE(d.setCel(clip, 0, Cel{Image(2, 2, kBlue), 1, 1}));
    d.layer(clip)->clipped = true;
    d.layer(base)->hasMask = true;
    d.layer(base)->maskEnabled = false;
    REQUIRE(d.setMaskCel(base, 0, Cel{maskImage(3, 3, 77), 2, 2}));
    const LayerId adj = d.addLayer("Hue", LayerKind::Adjustment).value();
    d.layer(adj)->adjustment = {AdjustmentType::HueSaturation, -30, 20, 5};
    d.setTileMode(TileMode::Both);
    CHECK(requiredSchemaVersion(d) == 2);

    auto bytes = serializeProject(d);
    REQUIRE(bytes);
    ProjectInfo info;
    auto back = deserializeProject(bytes.value(), &info);
    REQUIRE(back);
    CHECK(info.schemaVersion == 2);
    const Document& r = back.value();
    CHECK(r.tileMode() == TileMode::Both);
    CHECK(r.layer(clip)->clipped);
    CHECK(r.layer(base)->hasMask);
    CHECK_FALSE(r.layer(base)->maskEnabled);
    REQUIRE(r.maskCel(base, 0));
    CHECK(r.maskCel(base, 0)->x == 2);
    CHECK(r.maskCel(base, 0)->image.pixel(0, 0).r == 77);
    CHECK(r.layer(adj)->kind == LayerKind::Adjustment);
    CHECK(r.layer(adj)->adjustment == Adjustment{AdjustmentType::HueSaturation, -30, 20, 5});
    CHECK(r.composite(0) == d.composite(0));
}

TEST_CASE("Tile mode names", "[layers][tile]") {
    CHECK(tileModeFromString("both") == TileMode::Both);
    CHECK_FALSE(tileModeFromString("diagonal"));
    CHECK(wrapsX(TileMode::Horizontal));
    CHECK_FALSE(wrapsY(TileMode::Horizontal));
    CHECK(adjustmentTypeFromString("posterize") == AdjustmentType::Posterize);
}

TEST_CASE("Banded multi-threaded compositing matches row-by-row compositing", "[layers][composite]") {
    // 300x300 is above the threading threshold; 1-row regions always run on one thread.
    Document d = makeDoc(300, 300);
    std::uint32_t seed = 7;
    auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return static_cast<std::uint8_t>(seed >> 24); };
    for (int i = 0; i < 6; ++i) {
        const LayerId id = d.addLayer("L").value();
        Image img(300, 300);
        for (int y = 0; y < 300; ++y)
            for (int x = 0; x < 300; ++x)
                if (rnd() % 3 == 0) img.setPixel(x, y, {rnd(), rnd(), rnd(), static_cast<std::uint8_t>(i % 2 ? 255 : 120)});
        REQUIRE(d.setCel(id, 0, Cel{img, 0, 0}));
        Layer* l = d.layer(id);
        if (i == 2) l->blend = BlendMode::Multiply;
        if (i == 3) l->clipped = true;
        if (i == 4) l->opacity = 140;
        if (i == 5) {
            l->hasMask = true;
            Image m(300, 300);
            for (int y = 0; y < 300; ++y)
                for (int x = 0; x < 300; ++x) { const std::uint8_t v = rnd(); m.setPixel(x, y, {v, v, v, 255}); }
            REQUIRE(d.setMaskCel(id, 0, Cel{m, 0, 0}));
        }
    }
    const LayerId adj = d.addLayer("Adjust", LayerKind::Adjustment).value();
    d.layer(adj)->adjustment = Adjustment{AdjustmentType::Invert, 0, 0, 0};
    d.layer(adj)->opacity = 100;

    const Image full = d.composite(0);
    Image rows(300, 300);
    for (int y = 0; y < 300; ++y) d.compositeRegion(0, {0, y, 300, 1}, rows);
    CHECK(full == rows);

    Image region(300, 300);
    d.compositeRegion(0, {13, 7, 280, 290}, region);
    bool same = true;
    for (int y = 7; y < 297; ++y)
        for (int x = 13; x < 293; ++x) same = same && region.pixel(x, y) == full.pixel(x, y);
    CHECK(same);
    CHECK(region.pixel(5, 5).a == 0);   // outside the region stays untouched
}

TEST_CASE("Levels and Curves adjustments", "[layers][adjust]") {
    // Levels: black 50, white 200, gamma 1.0 stretches linearly.
    const Adjustment lv{AdjustmentType::Levels, 50, 200, 100};
    CHECK(validateAdjustment(lv));
    CHECK(adjustPixel(Rgba8{50, 200, 125, 9}, lv) == Rgba8{0, 255, 128, 9});
    CHECK(adjustPixel(Rgba8{10, 250, 0, 255}, lv).r == 0);
    // Gamma 2.0 brightens midtones.
    CHECK(adjustPixel(Rgba8{64, 64, 64, 255}, Adjustment{AdjustmentType::Levels, 0, 255, 200}).r > 100);
    CHECK_FALSE(validateAdjustment(Adjustment{AdjustmentType::Levels, 100, 100, 100}));   // white must exceed black
    CHECK_FALSE(validateAdjustment(Adjustment{AdjustmentType::Levels, 0, 255, 5}));

    // Curves: identity through the corners, exact at control points, monotone between.
    const Adjustment id{AdjustmentType::Curves, {{0, 0}, {255, 255}}};
    for (int v : {0, 1, 77, 128, 254, 255}) CHECK(adjustPixel(Rgba8{std::uint8_t(v), 0, 0, 255}, id).r == v);
    const Adjustment s{AdjustmentType::Curves, {{0, 0}, {64, 32}, {192, 224}, {255, 255}}};
    CHECK(validateAdjustment(s));
    const auto t = curveTable(s.points);
    CHECK(t[64] == 32);
    CHECK(t[192] == 224);
    for (int i = 1; i < 256; ++i) CHECK(t[static_cast<std::size_t>(i)] >= t[static_cast<std::size_t>(i - 1)]);   // no overshoot
    CHECK(PreparedAdjustment(s).apply(Rgba8{64, 192, 0, 7}) == Rgba8{32, 224, 0, 7});
    CHECK_FALSE(validateAdjustment(Adjustment{AdjustmentType::Curves, {{10, 0}, {10, 5}}}));   // x not increasing
    CHECK_FALSE(validateAdjustment(Adjustment{AdjustmentType::Curves, {{10, 0}}}));
    CHECK_FALSE(validateAdjustment(Adjustment{AdjustmentType::Invert, {{0, 0}, {255, 255}}}));  // points only for curves
    // Prepared tables match the per-pixel path for every LUT type.
    for (const Adjustment& a : {Adjustment{AdjustmentType::BrightnessContrast, 30, -20}, Adjustment{AdjustmentType::Invert},
                                Adjustment{AdjustmentType::Posterize, 5}, lv, s}) {
        const PreparedAdjustment p(a);
        for (int v = 0; v < 256; v += 17) {
            const Rgba8 c{std::uint8_t(v), std::uint8_t(255 - v), std::uint8_t(v / 2), 200};
            CHECK(p.apply(c) == adjustPixel(c, a));
        }
    }
}

TEST_CASE("Schema 3 for Levels and Curves round-trips", "[layers][project]") {
    Document d = makeDoc(4, 4);
    const LayerId base = d.addLayer("Base").value();
    REQUIRE(d.setCel(base, 0, Cel{Image(4, 4, Rgba8{90, 140, 200, 255}), 0, 0}));
    const LayerId c = d.addLayer("Curve", LayerKind::Adjustment).value();
    d.layer(c)->adjustment = Adjustment{AdjustmentType::Curves, {{0, 10}, {128, 100}, {255, 240}}};
    CHECK(requiredSchemaVersion(d) == 3);
    auto bytes = serializeProject(d);
    REQUIRE(bytes);
    ProjectInfo info;
    auto back = deserializeProject(bytes.value(), &info);
    REQUIRE(back);
    CHECK(info.schemaVersion == 3);
    CHECK(back.value().layer(c)->adjustment == d.layer(c)->adjustment);
    CHECK(back.value().composite(0) == d.composite(0));
    d.layer(c)->adjustment = Adjustment{AdjustmentType::Invert};
    CHECK(requiredSchemaVersion(d) == 2);
}

TEST_CASE("Adjustment layers are named after their type", "[layers][adjust]") {
    Editor ed(makeDoc());
    const LayerId c = ed.addAdjustmentLayer(Adjustment{AdjustmentType::Curves, {{0, 0}, {255, 255}}}, kNoLayer).value();
    CHECK(ed.document().layer(c)->name == "Curves");
    const LayerId lv = ed.addAdjustmentLayer(Adjustment{AdjustmentType::Levels, 0, 255, 100}, kNoLayer).value();
    CHECK(ed.document().layer(lv)->name == "Levels");
}

TEST_CASE("Linked cels round-trip with one PNG per group", "[project][links]") {
    Document d = makeDoc(4, 4);
    const LayerId l = d.addLayer("L").value();
    for (int f = 1; f < 4; ++f) REQUIRE(d.insertFrame(f));
    for (int f = 0; f < 4; ++f) REQUIRE(d.setCel(l, f, Cel{Image(2, 2, Rgba8{std::uint8_t(f * 50), 0, 0, 255}), f, 0}));
    REQUIRE(d.linkCels(l, {1, 3}));
    CHECK(requiredSchemaVersion(d) == 3);
    auto bytes = serializeProject(d);
    REQUIRE(bytes);
    ProjectInfo info;
    auto back = deserializeProject(bytes.value(), &info);
    REQUIRE(back);
    CHECK(info.schemaVersion == 3);
    const Document& r = back.value();
    CHECK(r.linkedFrames(l, 3) == std::vector<int>{1, 3});
    CHECK(r.cel(l, 3)->image.pixel(0, 0).r == 50);
    CHECK(r.cel(l, 3)->x == 1);
    for (int f = 0; f < 4; ++f) CHECK(r.composite(f) == d.composite(f));
    // Only 3 cel PNGs are stored (frame 3 reuses frame 1's).
    const std::string text(bytes.value().begin(), bytes.value().end());
    CHECK(text.find("cels/L" + std::to_string(l) + "_F3.png") == std::string::npos);
}
