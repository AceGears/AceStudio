#include "TestSupport.h"

#include "acestudio/core/StyleProfile.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace acestudio;
using namespace acestudio::style;

namespace {
// A 24x24 blob, lit from the top left with `tones` shades, optionally with a
// dark outline, upscaled by `scale` (nearest neighbour).
Image sprite(int tones, bool outline, int scale, Rgba8 base) {
    Image s(24, 24);
    for (int y = 0; y < 24; ++y)
        for (int x = 0; x < 24; ++x) {
            const double dx = x - 11.5, dy = y - 11.5;
            if (dx * dx + dy * dy > 10.5 * 10.5) continue;
            // Brighter towards the top left.
            const double t = std::clamp((dx + dy + 16.0) / 32.0, 0.0, 0.999);
            const int level = static_cast<int>(t * tones);
            const double k = 1.0 - 0.18 * level;
            s.setPixel(x, y, Rgba8{static_cast<std::uint8_t>(base.r * k), static_cast<std::uint8_t>(base.g * k),
                                   static_cast<std::uint8_t>(base.b * k), 255});
        }
    if (outline) {
        Image o = s;
        for (int y = 0; y < 24; ++y)
            for (int x = 0; x < 24; ++x) {
                if (s.pixel(x, y).a) continue;
                bool next = false;
                for (auto [dx, dy] : {std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}})
                    next = next || s.pixel(x + dx, y + dy).a;
                if (next) o.setPixel(x, y, Rgba8::opaque(20, 16, 24));
            }
        s = o;
    }
    Image big(24 * scale, 24 * scale);
    for (int y = 0; y < big.height(); ++y)
        for (int x = 0; x < big.width(); ++x) big.setPixel(x, y, s.pixel(x / scale, y / scale));
    return big;
}
} // namespace

TEST_CASE("Style analysis measures scale, outline, shading and light", "[style]") {
    const StyleFeatures f = analyze(sprite(3, true, 4, Rgba8::opaque(220, 120, 60)));
    CHECK(f.pixelScale == 4);
    CHECK(f.outline == Outline::Dark);
    CHECK(f.shading == Shading::Cel);
    CHECK(f.lightX < -0.15);   // brighter on the left: light from the left
    CHECK(f.lightY < -0.15);   // and from the top
    const std::string words = describe(f);
    CHECK(words.find("dark 1-pixel outline") != std::string::npos);
    CHECK(words.find("cel shading with 3 tones") != std::string::npos);
    CHECK(words.find("light from the top-left") != std::string::npos);

    const StyleFeatures flat = analyze(sprite(1, false, 1, Rgba8::opaque(60, 200, 90)));
    CHECK(flat.pixelScale == 1);
    CHECK(flat.outline != Outline::Dark);
    CHECK(flat.shading == Shading::Flat);
}

TEST_CASE("Style similarity prefers results in the same style", "[style]") {
    const StyleFeatures profile = merge({analyze(sprite(3, true, 1, Rgba8::opaque(220, 120, 60))),
                                         analyze(sprite(3, true, 1, Rgba8::opaque(200, 100, 70)))});
    const double same = styleSimilarity(profile, analyze(sprite(3, true, 1, Rgba8::opaque(210, 110, 64))));
    const double other = styleSimilarity(profile, analyze(sprite(1, false, 1, Rgba8::opaque(40, 90, 240))));
    CHECK(same > 0.8);
    CHECK(other < same - 0.25);
}

TEST_CASE("Style library stores profiles with references and re-measures them", "[style]") {
    TempDir dir;
    StyleLibrary lib(dir.path());
    auto p = lib.create("Volcanic Dungeon", {{sprite(3, true, 2, Rgba8::opaque(220, 80, 40)), RefRole::Style},
                                             {sprite(3, true, 2, Rgba8::opaque(90, 90, 100)), RefRole::Character}});
    REQUIRE(p);
    CHECK(p.value().id == "volcanic-dungeon");
    REQUIRE(p.value().references.size() == 2);
    CHECK(p.value().features.sampleCount == 1);   // the character reference does not define the style
    CHECK(p.value().features.outline == Outline::Dark);
    auto again = lib.create("Volcanic Dungeon", {});
    REQUIRE(again);
    CHECK(again.value().id == "volcanic-dungeon-2");
    CHECK(lib.list().size() == 2);
    auto loaded = lib.load("volcanic-dungeon");
    REQUIRE(loaded);
    CHECK(loaded.value().features.palette.size() == p.value().features.palette.size());
    auto ref = lib.referenceImage(loaded.value(), loaded.value().references[0]);
    REQUIRE(ref);
    CHECK(ref.value().width() == 48);
    // A profile file naming a path outside its folder is not followed.
    auto j = loaded.value().toJson();
    j["references"].push_back({{"file", "../../secret.png"}, {"role", "style"}});
    auto parsed = StyleProfile::fromJson(j);
    REQUIRE(parsed);
    CHECK(parsed.value().references.size() == 2);
    CHECK_FALSE(lib.load("../escape"));
    REQUIRE(lib.remove("volcanic-dungeon-2"));
    CHECK(lib.list().size() == 1);
}
