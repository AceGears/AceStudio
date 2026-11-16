#include "acestudio/core/Document.h"
#include "acestudio/core/Editor.h"
#include "acestudio/core/Paint.h"
#include "acestudio/core/ProjectIO.h"
#include "acestudio/core/Vector.h"

#include <catch2/catch_test_macros.hpp>

using namespace acestudio;

namespace {

const Rgba8 kDark = Rgba8::opaque(20, 20, 30);
const Rgba8 kRed = Rgba8::opaque(200, 40, 40);
const Rgba8 kSky = Rgba8::opaque(90, 150, 230);

Palette threeColours() {
    Palette p("Three");
    REQUIRE(p.add(kDark));
    REQUIRE(p.add(kRed));
    REQUIRE(p.add(kSky));
    REQUIRE(p.add(Rgba8::transparent()));
    return p;
}

Document artDocument(LayerId& layer) {
    auto doc = Document::create(16, 12).value();
    doc.palette() = threeColours();
    layer = doc.addLayer("Art").value();
    return doc;
}

bool onPalette(const Palette& p, const Image& img) { return countOffPalette(p, img) == 0; }

} // namespace

TEST_CASE("Snapping keeps alpha and picks the nearest opaque colour", "[palette][lock]") {
    const Palette p = threeColours();
    CHECK(snapToPalette(p, Rgba8{210, 50, 30, 255}) == kRed);
    CHECK(snapToPalette(p, Rgba8{100, 140, 220, 128}) == Rgba8{kSky.r, kSky.g, kSky.b, 128});
    CHECK(snapToPalette(p, Rgba8::transparent()) == Rgba8::transparent());
    CHECK(snapToPalette(Palette{}, Rgba8{1, 2, 3, 255}) == Rgba8{1, 2, 3, 255});   // nothing to snap to

    Image img(4, 1);
    img.setPixel(0, 0, kRed);
    img.setPixel(1, 0, Rgba8{10, 10, 10, 255});
    img.setPixel(2, 0, Rgba8{250, 60, 60, 90});
    CHECK(countOffPalette(p, img) == 2);
    CHECK(snapToPalette(p, img) == 2);
    CHECK(img.pixel(1, 0) == kDark);
    CHECK(img.pixel(2, 0) == Rgba8{kRed.r, kRed.g, kRed.b, 90});
    CHECK(img.pixel(3, 0) == Rgba8::transparent());
    CHECK(countOffPalette(p, img) == 0);
}

TEST_CASE("A locked palette refuses edits and undoes as one step", "[palette][lock]") {
    LayerId l = kNoLayer;
    Editor ed(artDocument(l));
    REQUIRE(ed.setPaletteLocked(true));
    CHECK(ed.document().paletteLocked());
    CHECK(ed.document().extensions().value("paletteLock", false));
    CHECK_FALSE(ed.setPaletteColor(0, kSky));
    CHECK_FALSE(ed.addPaletteColor(kSky));
    CHECK_FALSE(ed.removePaletteColor(0));
    CHECK_FALSE(ed.replacePalette(Palette::defaultPalette()));
    CHECK_FALSE(ed.convertColorMode(ColorMode::Grayscale));
    CHECK(ed.document().palette() == threeColours());

    REQUIRE(ed.undo());
    CHECK_FALSE(ed.document().paletteLocked());
    CHECK_FALSE(ed.document().extensions().contains("paletteLock"));
    CHECK(ed.addPaletteColor(kSky));
    REQUIRE(ed.undo());

    // A palette with no opaque colour cannot be locked.
    auto doc = Document::create(4, 4).value();
    doc.palette() = Palette{};
    Editor empty(std::move(doc));
    CHECK_FALSE(empty.setPaletteLocked(true));
}

TEST_CASE("Pixels entering a locked document snap to the palette", "[palette][lock]") {
    LayerId l = kNoLayer;
    Editor ed(artDocument(l));
    REQUIRE(ed.setPaletteLocked(true));
    const Palette p = ed.document().palette();

    Image photo(8, 6);
    for (int y = 0; y < 6; ++y)
        for (int x = 0; x < 8; ++x)
            photo.setPixel(x, y, Rgba8{static_cast<std::uint8_t>(x * 30), static_cast<std::uint8_t>(y * 40), 120, 255});
    REQUIRE(ed.setCelImage(l, 0, photo));
    CHECK(onPalette(p, ed.document().cel(l, 0)->image));

    auto added = ed.addImageLayer("Pasted", photo, l, 0);
    REQUIRE(added);
    CHECK(onPalette(p, ed.document().cel(added.value(), 0)->image));

    auto anim = ed.addAnimationLayer("Clip", {photo, photo}, 0, 100, "");
    REQUIRE(anim);
    CHECK(onPalette(p, ed.document().cel(anim.value(), 1)->image));

    VectorShape g;
    g.kind = ShapeKind::Rectangle;
    g.points = {VectorPoint{1, 1}, VectorPoint{12, 9}};
    g.stroke = StrokeStyle::None;
    g.fill = FillStyle::Linear;
    g.fillColor = Rgba8::opaque(0, 0, 0);
    g.fillColor2 = Rgba8::opaque(255, 255, 255);
    g.dither = false;   // smooth: would make many new greys
    g.fromX = 1;
    g.toX = 12;
    REQUIRE(ed.setVectorShapes(l, 1, {g}, "Add Rectangle"));
    CHECK(onPalette(p, ed.document().cel(l, 1)->image));
    CHECK(ed.document().offPalettePixels() == 0);

    // Unlocked, the same image keeps its colours.
    REQUIRE(ed.setPaletteLocked(false));
    REQUIRE(ed.setCelImage(l, 0, photo));
    CHECK(ed.document().cel(l, 0)->image == photo);
    CHECK(ed.document().offPalettePixels() > 0);
}

TEST_CASE("Conform to palette snaps every cel in one undo step", "[palette][lock]") {
    auto doc = Document::create(8, 8).value();
    doc.palette() = threeColours();
    const LayerId l = doc.addLayer("Art").value();
    Image img(8, 8, Rgba8{205, 45, 35, 255});
    img.setPixel(0, 0, kSky);
    REQUIRE(doc.setCel(l, 0, Cel{img, 0, 0}));
    Editor ed(std::move(doc));
    CHECK(ed.document().offPalettePixels() == 63);

    auto n = ed.conformToPalette();
    REQUIRE(n);
    CHECK(n.value() == 63);
    CHECK(ed.document().offPalettePixels() == 0);
    CHECK(ed.document().cel(l, 0)->image.pixel(3, 3) == kRed);
    CHECK(ed.document().cel(l, 0)->image.pixel(0, 0) == kSky);

    auto again = ed.conformToPalette();
    REQUIRE(again);
    CHECK(again.value() == 0);   // nothing to do, no extra undo step

    REQUIRE(ed.undo());
    CHECK(ed.document().cel(l, 0)->image == img);
}

// LOCAL-SKIP-BEGIN (needs Paint/PngCodec and ProjectIO/miniz)
TEST_CASE("Painting on a locked RGBA document uses only palette colours", "[palette][lock][paint]") {
    LayerId l = kNoLayer;
    Editor ed(artDocument(l));
    REQUIRE(ed.setPaletteLocked(true));
    auto s = PaintSession::begin(ed, l, 0, "Pencil");
    REQUIRE(s);
    s.value()->writePixel(2, 2, Rgba8{215, 35, 50, 255});
    s.value()->writePixel(3, 2, Rgba8{80, 160, 240, 255});
    REQUIRE(s.value()->commit());
    const Image& img = ed.document().cel(l, 0)->image;
    CHECK(onPalette(ed.document().palette(), img));
    CHECK(ed.document().composite(0).pixel(2, 2) == kRed);
    CHECK(ed.document().composite(0).pixel(3, 2) == kSky);
}

TEST_CASE("The palette lock is saved and survives a reload", "[palette][lock][project]") {
    LayerId l = kNoLayer;
    Editor ed(artDocument(l));
    REQUIRE(ed.setPaletteLocked(true));
    CHECK(requiredSchemaVersion(ed.document()) == 1);   // an extension, no new schema
    auto bytes = serializeProject(ed.document());
    REQUIRE(bytes);
    auto back = deserializeProject(bytes.value());
    REQUIRE(back);
    CHECK(back.value().paletteLocked());
    CHECK(back.value().palette() == threeColours());
}
// LOCAL-SKIP-END
