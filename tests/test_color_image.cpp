#include <catch2/catch_test_macros.hpp>

#include "acestudio/core/Image.h"
#include "acestudio/core/Palette.h"

using namespace acestudio;

TEST_CASE("Rgba8 hex round trip", "[color]") {
    auto c = Rgba8::fromHex("#1A2b3C");
    REQUIRE(c);
    CHECK(*c == Rgba8{0x1A, 0x2B, 0x3C, 255});
    CHECK(c->toHex() == "#1A2B3CFF");
    CHECK(c->toHex(false) == "#1A2B3C");

    auto withAlpha = Rgba8::fromHex("80FF0040");
    REQUIRE(withAlpha);
    CHECK(withAlpha->a == 0x40);

    CHECK_FALSE(Rgba8::fromHex("#12345"));
    CHECK_FALSE(Rgba8::fromHex("#GG0000"));
    CHECK_FALSE(Rgba8::fromHex(""));
}

TEST_CASE("Rgba8 luma", "[color]") {
    CHECK(Rgba8::opaque(0, 0, 0).luma() == 0);
    CHECK(Rgba8::opaque(255, 255, 255).luma() == 255);
    CHECK(Rgba8::opaque(0, 255, 0).luma() == 182);   // 0.7152 * 255
}

TEST_CASE("Image creation enforces limits", "[image]") {
    CHECK_FALSE(Image::create(0, 10));
    CHECK_FALSE(Image::create(10, -1));
    CHECK_FALSE(Image::create(Image::kMaxDimension + 1, 1));
    auto img = Image::create(4, 3, Rgba8::opaque(1, 2, 3));
    REQUIRE(img);
    CHECK(img.value().width() == 4);
    CHECK(img.value().height() == 3);
    CHECK(img.value().pixel(3, 2) == Rgba8::opaque(1, 2, 3));
    CHECK(img.value().byteSize() == 48);
}

TEST_CASE("Image pixel access is bounds-safe", "[image]") {
    Image img(2, 2);
    img.setPixel(-1, 0, Rgba8::opaque(9, 9, 9));
    img.setPixel(2, 0, Rgba8::opaque(9, 9, 9));
    CHECK(img.isFullyTransparent());
    CHECK(img.pixel(5, 5) == Rgba8::transparent());
    img.setPixel(1, 1, Rgba8::opaque(9, 9, 9));
    CHECK(img.pixel(1, 1).r == 9);
}

TEST_CASE("Image fillRect / copy and blit clip correctly", "[image]") {
    Image img(8, 8);
    img.fillRect({6, 6, 10, 10}, Rgba8::opaque(255, 0, 0));
    CHECK(img.pixel(7, 7).r == 255);
    CHECK(img.pixel(5, 5).a == 0);
    CHECK(img.opaqueBounds() == IntRect{6, 6, 2, 2});

    Image part = img.copy({5, 5, 10, 10});
    CHECK(part.width() == 3);
    CHECK(part.height() == 3);
    CHECK(part.pixel(2, 2).r == 255);

    Image dst(4, 4);
    dst.blit(part, -1, -1);   // part's bottom-right 2x2 (all red) lands at (0,0)-(1,1)
    CHECK(dst.pixel(0, 0).r == 255);
    CHECK(dst.pixel(1, 1).r == 255);
    CHECK(dst.pixel(2, 2).a == 0);   // untouched
}

TEST_CASE("Image fromRgba validates buffer size", "[image]") {
    std::vector<std::uint8_t> bytes(2 * 2 * 4, 7);
    CHECK(Image::fromRgba(2, 2, bytes));
    bytes.pop_back();
    CHECK_FALSE(Image::fromRgba(2, 2, bytes));
}

TEST_CASE("Palette add / find / nearest and capacity", "[palette]") {
    Palette p;
    REQUIRE(p.add(Rgba8::opaque(0, 0, 0)));
    REQUIRE(p.add(Rgba8::opaque(255, 255, 255)));
    CHECK(p.find(Rgba8::opaque(255, 255, 255)) == std::optional<std::size_t>(1));
    CHECK_FALSE(p.find(Rgba8::opaque(1, 1, 1)));
    CHECK(p.nearest(Rgba8::opaque(200, 200, 200)) == std::optional<std::size_t>(1));
    CHECK_FALSE(p.set(9, Rgba8{}));
    CHECK_FALSE(p.remove(9));

    Palette full;
    for (int i = 0; i < 256; ++i) REQUIRE(full.add(Rgba8::opaque(static_cast<std::uint8_t>(i), 0, 0)));
    CHECK_FALSE(full.add(Rgba8{}));
    CHECK(Palette().nearest(Rgba8{}) == std::nullopt);
    CHECK(Palette::defaultPalette().size() == 16);
}
