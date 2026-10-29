#include <catch2/catch_test_macros.hpp>

#include "TestSupport.h"
#include "acestudio/core/Editor.h"
#include "acestudio/core/PaletteIO.h"

using namespace acestudio;

namespace {
Palette sample() {
    Palette p("Test Ramp");
    REQUIRE(p.add(Rgba8::opaque(0, 0, 0)));
    REQUIRE(p.add(Rgba8::opaque(18, 52, 86)));
    REQUIRE(p.add(Rgba8::opaque(255, 255, 255)));
    return p;
}
} // namespace

TEST_CASE("Palette formats round-trip", "[palette][io]") {
    const Palette p = sample();
    for (const char* ext : {".gpl", ".pal", ".hex", ".txt"}) {
        INFO(ext);
        auto text = formatPalette(p, ext);
        REQUIRE(text);
        auto back = parsePalette(text.value(), ext);
        REQUIRE(back);
        CHECK(back.value().colors() == p.colors());
    }
    CHECK(parsePalette(formatPalette(p, ".gpl").value(), ".gpl").value().name() == "Test Ramp");
}

TEST_CASE("Palette parsers accept real-world variations", "[palette][io]") {
    // GIMP with comments, CRLF and tabbed names
    auto gpl = parsePalette("GIMP Palette\r\nName: Endesga\r\nColumns: 4\r\n# comment\r\n190  38  51\tRed\r\n 0 0 0\r\n", ".gpl");
    REQUIRE(gpl);
    CHECK(gpl.value().size() == 2);
    CHECK(gpl.value().name() == "Endesga");
    // Lospec .hex, uppercase and lowercase, blank lines
    auto hex = parsePalette("be2633\n\nFFFFFF\n", ".hex");
    REQUIRE(hex);
    CHECK(hex.value().at(0) == Rgba8::opaque(0xBE, 0x26, 0x33));
    // Paint.NET with alpha
    auto pdn = parsePalette("; header\n80FF0000\n", ".txt");
    REQUIRE(pdn);
    CHECK(pdn.value().at(0) == Rgba8{255, 0, 0, 0x80});
}

TEST_CASE("Palette parsers reject malformed input", "[palette][io]") {
    CHECK_FALSE(parsePalette("not a palette", ".gpl"));
    CHECK_FALSE(parsePalette("GIMP Palette\n300 0 0\n", ".gpl"));
    CHECK_FALSE(parsePalette("JASC-PAL\n0100\n5\n0 0 0\n", ".pal"));      // truncated
    CHECK_FALSE(parsePalette("JASC-PAL\n0100\n999\n", ".pal"));
    CHECK_FALSE(parsePalette("RIFF....PAL data", ".pal"));
    CHECK_FALSE(parsePalette("zzzzzz\n", ".hex"));
    CHECK_FALSE(parsePalette("", ".hex"));
    CHECK_FALSE(parsePalette("000000", ".ase"));
    std::string big;
    for (int i = 0; i < 257; ++i) big += "000000\n";
    CHECK_FALSE(parsePalette(big, ".hex"));
}

TEST_CASE("Palette files on disk", "[palette][io]") {
    TempDir dir;
    REQUIRE(savePaletteFile(sample(), dir / "ramp.gpl"));
    auto back = loadPaletteFile(dir / "ramp.gpl");
    REQUIRE(back);
    CHECK(back.value().size() == 3);
    REQUIRE(savePaletteFile(sample(), dir / "ramp.hex"));
    CHECK(loadPaletteFile(dir / "ramp.hex").value().name() == "ramp");   // nameless formats use the file name
    CHECK_FALSE(savePaletteFile(sample(), dir / "ramp.xyz"));
}

TEST_CASE("Palette extraction: exact and median cut", "[palette]") {
    Image img(4, 1);
    img.setPixel(0, 0, Rgba8::opaque(1, 2, 3));
    img.setPixel(1, 0, Rgba8::opaque(9, 9, 9));
    img.setPixel(2, 0, Rgba8::opaque(1, 2, 3));
    img.setPixel(3, 0, Rgba8{50, 50, 50, 10});   // nearly transparent: ignored
    const Palette exact = extractPalette(img);
    REQUIRE(exact.size() == 2);
    CHECK(exact.at(0) == Rgba8::opaque(1, 2, 3));

    Image grad(256, 4);
    for (int x = 0; x < 256; ++x)
        for (int y = 0; y < 4; ++y)
            grad.setPixel(x, y, Rgba8::opaque(static_cast<std::uint8_t>(x), static_cast<std::uint8_t>(255 - x), static_cast<std::uint8_t>(y * 60)));
    const Palette reduced = extractPalette(grad, 16);
    CHECK(reduced.size() == 16);
    CHECK(extractPalette(Image(2, 2)).empty());
}

TEST_CASE("Colour-mode conversion is undoable and correct", "[palette][editor]") {
    auto d = Document::create(2, 1).value();
    const auto l = d.addLayer("L").value();
    Image px(2, 1);
    px.setPixel(0, 0, Rgba8::opaque(250, 10, 10));
    px.setPixel(1, 0, Rgba8{10, 240, 10, 60});
    REQUIRE(d.setCel(l, 0, Cel{px, 0, 0}));
    d.palette().clear();
    REQUIRE(d.palette().add(Rgba8::opaque(255, 0, 0)));
    REQUIRE(d.palette().add(Rgba8::opaque(0, 255, 0)));
    Editor e(std::move(d));

    REQUIRE(e.convertColorMode(ColorMode::Indexed));
    CHECK(e.document().colorMode() == ColorMode::Indexed);
    CHECK(e.document().composite(0).pixel(0, 0) == Rgba8::opaque(255, 0, 0));
    CHECK(e.document().composite(0).pixel(1, 0).a == 0);    // alpha < 128 becomes transparent
    REQUIRE(e.undo());
    CHECK(e.document().colorMode() == ColorMode::Rgba);
    CHECK(e.document().composite(0).pixel(0, 0) == Rgba8::opaque(250, 10, 10));

    REQUIRE(e.convertColorMode(ColorMode::Grayscale));
    const Rgba8 g = e.document().composite(0).pixel(1, 0);
    CHECK(g.r == g.g);
    CHECK(g.a == 60);

    Editor empty(Document::create(1, 1).value());
    REQUIRE(empty.replacePalette(Palette("none")));
    CHECK_FALSE(empty.convertColorMode(ColorMode::Indexed));
    REQUIRE(empty.undo());
    CHECK(empty.document().palette().size() == 16);
}
