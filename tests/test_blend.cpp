#include <catch2/catch_test_macros.hpp>

#include "acestudio/core/BlendMode.h"

#include <cstdlib>

using namespace acestudio;

namespace {
bool near(Rgba8 a, Rgba8 b, int tol = 1) {
    return std::abs(a.r - b.r) <= tol && std::abs(a.g - b.g) <= tol && std::abs(a.b - b.b) <= tol &&
           std::abs(a.a - b.a) <= tol;
}
} // namespace

TEST_CASE("Blend mode names round trip", "[blend]") {
    for (auto m : {BlendMode::Normal, BlendMode::Multiply, BlendMode::Screen, BlendMode::Overlay,
                   BlendMode::Darken, BlendMode::Lighten, BlendMode::Add, BlendMode::Subtract,
                   BlendMode::Difference}) {
        CHECK(blendModeFromString(toString(m)) == m);
    }
    CHECK_FALSE(blendModeFromString("bogus"));
}

TEST_CASE("Normal blending", "[blend]") {
    const Rgba8 red = Rgba8::opaque(255, 0, 0);
    const Rgba8 blue = Rgba8::opaque(0, 0, 255);
    CHECK(blendPixel(red, blue, BlendMode::Normal, 255) == blue);           // opaque replaces exactly
    CHECK(blendPixel(red, blue, BlendMode::Normal, 0) == red);              // zero opacity is a no-op
    CHECK(blendPixel(red, Rgba8::transparent(), BlendMode::Normal, 255) == red);
    CHECK(near(blendPixel(red, blue, BlendMode::Normal, 128), Rgba8{127, 0, 128, 255}));
    // Onto transparent: colour preserved, alpha from source.
    CHECK(near(blendPixel(Rgba8::transparent(), Rgba8{10, 20, 30, 100}, BlendMode::Normal, 255),
               Rgba8{10, 20, 30, 100}));
}

TEST_CASE("Separable blend modes on opaque pixels", "[blend]") {
    const Rgba8 b = Rgba8::opaque(200, 100, 50);
    const Rgba8 s = Rgba8::opaque(100, 200, 50);
    CHECK(near(blendPixel(b, s, BlendMode::Multiply, 255), Rgba8::opaque(78, 78, 10)));
    CHECK(near(blendPixel(b, s, BlendMode::Screen, 255), Rgba8::opaque(222, 222, 90)));
    CHECK(near(blendPixel(b, s, BlendMode::Darken, 255), Rgba8::opaque(100, 100, 50)));
    CHECK(near(blendPixel(b, s, BlendMode::Lighten, 255), Rgba8::opaque(200, 200, 50)));
    CHECK(near(blendPixel(b, s, BlendMode::Add, 255), Rgba8::opaque(255, 255, 100)));
    CHECK(near(blendPixel(b, s, BlendMode::Subtract, 255), Rgba8::opaque(100, 0, 0)));
    CHECK(near(blendPixel(b, s, BlendMode::Difference, 255), Rgba8::opaque(100, 100, 0)));
}

TEST_CASE("Blend modes onto transparent backdrop behave like normal", "[blend]") {
    const Rgba8 s{100, 150, 200, 255};
    for (auto m : {BlendMode::Multiply, BlendMode::Screen, BlendMode::Overlay, BlendMode::Difference})
        CHECK(near(blendPixel(Rgba8::transparent(), s, m, 255), s));
}
