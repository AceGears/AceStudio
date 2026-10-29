#include <catch2/catch_test_macros.hpp>

#include "TestSupport.h"
#include "acestudio/core/PngCodec.h"

using namespace acestudio;

TEST_CASE("PNG round trip is lossless including alpha", "[png]") {
    Image img(5, 3);
    img.setPixel(0, 0, Rgba8{255, 0, 0, 255});
    img.setPixel(4, 2, Rgba8{1, 2, 3, 4});
    img.setPixel(2, 1, Rgba8{200, 100, 50, 128});

    auto bytes = encodePng(img);
    REQUIRE(bytes);
    REQUIRE(bytes.value().size() > 8);
    CHECK(bytes.value()[1] == 'P');

    auto back = decodePng(bytes.value());
    REQUIRE(back);
    CHECK(back.value() == img);
}

TEST_CASE("PNG decode rejects garbage", "[png]") {
    std::vector<std::uint8_t> junk = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    auto r = decodePng(junk);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::CorruptData);
    CHECK_FALSE(decodePng({}));
    CHECK_FALSE(encodePng(Image{}));
}

TEST_CASE("PNG file write/read", "[png]") {
    TempDir dir;
    Image img(2, 2, Rgba8::opaque(9, 8, 7));
    REQUIRE(writePng(img, dir / "a.png"));
    auto back = readPng(dir / "a.png");
    REQUIRE(back);
    CHECK(back.value() == img);
    CHECK(readPng(dir / "missing.png").error().code == ErrorCode::NotFound);
}
