#include <catch2/catch_test_macros.hpp>

#include "acestudio/core/Document.h"

using namespace acestudio;

namespace {
Document makeDoc(int w = 8, int h = 8) {
    auto d = Document::create(w, h);
    REQUIRE(d);
    return std::move(d).value();
}
Cel solidCel(int w, int h, Rgba8 c, int x = 0, int y = 0) { return Cel{Image(w, h, c), x, y}; }
} // namespace

TEST_CASE("Document creation", "[document]") {
    CHECK_FALSE(Document::create(0, 8));
    CHECK_FALSE(Document::create(8, Image::kMaxDimension + 1));
    Document d = makeDoc(64, 32);
    CHECK(d.width() == 64);
    CHECK(d.height() == 32);
    CHECK(d.frameCount() == 1);
    CHECK(d.layers().empty());
    CHECK(d.palette().size() == 16);
    CHECK(d.validate());
}

TEST_CASE("Layers: add / group parenting / move / remove", "[document]") {
    Document d = makeDoc();
    auto a = d.addLayer("Body");
    auto g = d.addLayer("Armor", LayerKind::Group);
    REQUIRE(a);
    REQUIRE(g);
    auto child = d.addLayer("Plate", LayerKind::Raster, g.value());
    REQUIRE(child);
    CHECK_FALSE(d.addLayer("Bad", LayerKind::Raster, a.value()));    // parent must be a group
    CHECK_FALSE(d.addLayer("Bad", LayerKind::Raster, 999));

    REQUIRE(d.moveLayer(a.value(), 2));
    CHECK(d.layers().back().id == a.value());
    CHECK_FALSE(d.moveLayer(a.value(), 3));

    REQUIRE(d.setCel(child.value(), 0, solidCel(2, 2, Rgba8::opaque(1, 1, 1))));
    REQUIRE(d.removeLayer(g.value()));                                 // removes child and its cel
    CHECK(d.layers().size() == 1);
    CHECK(d.cels().empty());
    CHECK_FALSE(d.removeLayer(g.value()));
    CHECK(d.validate());
}

TEST_CASE("Cels require raster layers and valid frames", "[document]") {
    Document d = makeDoc();
    auto r = d.addLayer("R").value();
    auto g = d.addLayer("G", LayerKind::Group).value();
    CHECK(d.setCel(r, 0, solidCel(1, 1, Rgba8::opaque(1, 2, 3))));
    CHECK_FALSE(d.setCel(g, 0, solidCel(1, 1, Rgba8{})));
    CHECK_FALSE(d.setCel(r, 1, solidCel(1, 1, Rgba8{})));
    CHECK_FALSE(d.setCel(r, 0, Cel{}));
    CHECK_FALSE(d.setCel(77, 0, solidCel(1, 1, Rgba8{})));
}

TEST_CASE("Frames: insert / duplicate / remove keep cels and tags consistent", "[document][animation]") {
    Document d = makeDoc();
    const auto l = d.addLayer("L").value();
    REQUIRE(d.setCel(l, 0, solidCel(1, 1, Rgba8::opaque(10, 0, 0))));

    auto dup = d.duplicateFrame(0);
    REQUIRE(dup);
    CHECK(dup.value() == 1);
    CHECK(d.frameCount() == 2);
    REQUIRE(d.cel(l, 1));
    CHECK(d.cel(l, 1)->image.pixel(0, 0).r == 10);

    REQUIRE(d.cel(l, 1));
    d.cel(l, 1)->image.setPixel(0, 0, Rgba8::opaque(20, 0, 0));
    CHECK(d.cel(l, 0)->image.pixel(0, 0).r == 10);                    // duplicate is a deep copy

    REQUIRE(d.addTag({"walk", 0, 1, PlaybackDirection::PingPong, 0, "#FF0000"}));
    REQUIRE(d.insertFrame(0, 50));                                     // shifts everything right
    CHECK(d.frameCount() == 3);
    CHECK(d.frames()[0].durationMs == 50);
    CHECK(d.cel(l, 0) == nullptr);
    CHECK(d.cel(l, 2)->image.pixel(0, 0).r == 20);
    CHECK(d.tags()[0].fromFrame == 1);
    CHECK(d.tags()[0].toFrame == 2);

    REQUIRE(d.removeFrame(1));
    CHECK(d.frameCount() == 2);
    CHECK(d.cel(l, 1)->image.pixel(0, 0).r == 20);
    CHECK(d.tags()[0].fromFrame == 1);
    CHECK(d.tags()[0].toFrame == 1);

    REQUIRE(d.removeFrame(1));                                          // tag covered only that frame
    CHECK(d.tags().empty());
    CHECK_FALSE(d.removeFrame(0));                                      // last frame protected
    CHECK(d.validate());
}

TEST_CASE("Frame durations and tag validation", "[document][animation]") {
    Document d = makeDoc();
    CHECK(d.setFrameDuration(0, 1));
    CHECK(d.setFrameDuration(0, 65535));
    CHECK_FALSE(d.setFrameDuration(0, 0));
    CHECK_FALSE(d.setFrameDuration(0, 70000));
    CHECK_FALSE(d.setFrameDuration(3, 100));
    CHECK_FALSE(d.addTag({"", 0, 0, PlaybackDirection::Forward, 0, ""}));
    CHECK_FALSE(d.addTag({"t", 0, 4, PlaybackDirection::Forward, 0, ""}));
    CHECK_FALSE(d.addTag({"t", 0, 0, PlaybackDirection::Forward, -1, ""}));
    CHECK(d.addTag({"t", 0, 0, PlaybackDirection::Forward, 0, ""}));
    CHECK(d.removeTag(0));
    CHECK_FALSE(d.removeTag(0));
}

TEST_CASE("Composite respects order / offsets / visibility and opacity", "[document][compositing]") {
    Document d = makeDoc(4, 4);
    const auto bottom = d.addLayer("bottom").value();
    const auto top = d.addLayer("top").value();
    REQUIRE(d.setCel(bottom, 0, solidCel(4, 4, Rgba8::opaque(255, 0, 0))));
    REQUIRE(d.setCel(top, 0, solidCel(2, 2, Rgba8::opaque(0, 0, 255), 2, 2)));

    Image img = d.composite(0);
    CHECK(img.pixel(0, 0) == Rgba8::opaque(255, 0, 0));
    CHECK(img.pixel(3, 3) == Rgba8::opaque(0, 0, 255));

    d.layer(top)->visible = false;
    CHECK(d.composite(0).pixel(3, 3) == Rgba8::opaque(255, 0, 0));

    d.layer(top)->visible = true;
    d.layer(top)->opacity = 0;
    CHECK(d.composite(0).pixel(3, 3) == Rgba8::opaque(255, 0, 0));

    CHECK(d.composite(5).isFullyTransparent());                        // out-of-range frame
}

TEST_CASE("Composite applies group visibility and opacity", "[document][compositing]") {
    Document d = makeDoc(1, 1);
    const auto g = d.addLayer("g", LayerKind::Group).value();
    const auto c = d.addLayer("c", LayerKind::Raster, g).value();
    REQUIRE(d.setCel(c, 0, solidCel(1, 1, Rgba8::opaque(0, 255, 0))));
    CHECK(d.composite(0).pixel(0, 0) == Rgba8::opaque(0, 255, 0));
    d.layer(g)->opacity = 128;
    CHECK(d.composite(0).pixel(0, 0).a == 128);
    d.layer(g)->visible = false;
    CHECK(d.composite(0).isFullyTransparent());
}

TEST_CASE("Validation detects broken hierarchies", "[document]") {
    Document d = makeDoc();
    Layer a; a.id = 5; a.kind = LayerKind::Group; a.parent = 6;
    Layer b; b.id = 6; b.kind = LayerKind::Group; b.parent = 5;
    REQUIRE(d.restoreLayer(a));
    REQUIRE(d.restoreLayer(b));
    auto st = d.validate();
    REQUIRE_FALSE(st);
    CHECK(st.error().code == ErrorCode::CorruptData);

    Document d2 = makeDoc();
    Layer dup; dup.id = 3;
    REQUIRE(d2.restoreLayer(dup));
    CHECK_FALSE(d2.restoreLayer(dup));
    Layer zero; zero.id = 0;
    CHECK_FALSE(d2.restoreLayer(zero));
    // New layers never collide with restored IDs.
    CHECK(d2.addLayer("next").value() == 4);
}
