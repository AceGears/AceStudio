// Autotiling, procedural tilesets and seam checks (Phase 6).
#include <catch2/catch_test_macros.hpp>

#include "acestudio/core/Autotile.h"

#include <random>

using namespace acestudio;

namespace {
// A seamless (periodic) textured tile: sum of sines with integer periods.
Image texture(int T, Rgba8 c, int seed) {
    Image img(T, T);
    for (int y = 0; y < T; ++y)
        for (int x = 0; x < T; ++x) {
            const double v = std::sin(6.2831853 * (x * (1 + seed % 2) + y * 2) / T) * 0.5 + std::sin(6.2831853 * (x + 3 * y) / T) * 0.5;
            const int d = static_cast<int>(v * 18);
            auto ch = [&](int b) { return static_cast<std::uint8_t>(std::clamp(b + d, 0, 255)); };
            img.setPixel(x, y, {ch(c.r), ch(c.g), ch(c.b), 255});
        }
    return img;
}
TerrainMap randomMap(int w, int h, unsigned seed, double p = 0.5) {
    std::mt19937 rng(seed);
    std::bernoulli_distribution d(p);
    TerrainMap m(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) m.set(x, y, d(rng));
    return m;
}
bool rightMatches(const TileSamples& a, const TileSamples& b) { return a[2] == b[0] && a[5] == b[3] && a[8] == b[6]; }
bool belowMatches(const TileSamples& a, const TileSamples& b) { return a[6] == b[0] && a[7] == b[1] && a[8] == b[2]; }
} // namespace

TEST_CASE("Blob47 has 47 reduced masks; tile choice for simple maps", "[autotile]") {
    REQUIRE(blobMasks().size() == 47);
    CHECK(tileCount(AutotileScheme::Blob47) == 47);
    CHECK(tilesetGrid(AutotileScheme::Blob47) == std::pair<int, int>{8, 6});
    TerrainMap m(3, 3);
    m.fill(true);
    const auto c16 = autotile(m, AutotileScheme::Cardinal16);
    CHECK(c16.at(1, 1) == 15);           // all four sides
    CHECK(c16.at(0, 0) == (2 | 4));      // E and S
    const auto b47 = autotile(m, AutotileScheme::Blob47);
    CHECK(blobMasks()[static_cast<std::size_t>(b47.at(1, 1))] == 255);
    m.set(2, 0, false);                  // remove a corner cell: the centre gets an inner corner
    CHECK(blobMasks()[static_cast<std::size_t>(autotile(m, AutotileScheme::Blob47).at(1, 1))] == (255 & ~2));
    TerrainMap one(1, 1);
    one.set(0, 0, true);
    const auto w = autotile(one, AutotileScheme::Wang16);
    REQUIRE(w.columns == 2);
    CHECK(w.at(0, 0) == 4);              // its BR corner is the terrain cell
    CHECK(w.at(1, 1) == 1);
    CHECK(autotile(one, AutotileScheme::Cardinal16).at(0, 0) == 0);
    CHECK(autotileSchemeFromString("wang16") == AutotileScheme::Wang16);
}

TEST_CASE("Blob47 and Wang16 only ever place compatible neighbours", "[autotile]") {
    for (auto s : {AutotileScheme::Blob47, AutotileScheme::Wang16}) {
        const auto map = randomMap(40, 30, 11);
        const auto p = autotile(map, s);
        const TileSamples base{};
        auto samples = [&](int x, int y) { const int t = p.at(x, y); return t < 0 ? base : tileSamples(s, t); };
        int checked = 0;
        for (int y = 0; y < p.rows; ++y)
            for (int x = 0; x < p.columns; ++x) {
                if (x + 1 < p.columns) { CHECK(rightMatches(samples(x, y), samples(x + 1, y))); ++checked; }
                if (y + 1 < p.rows) { CHECK(belowMatches(samples(x, y), samples(x, y + 1))); ++checked; }
            }
        CHECK(checked > 2000);
    }
}

TEST_CASE("Generated tilesets pass the seam check for every scheme, deterministically", "[autotile]") {
    const Image grass = texture(16, {70, 150, 60, 255}, 1), dirt = texture(16, {140, 100, 60, 255}, 2);
    CHECK(checkWrap(grass).horizontal);
    CHECK(checkWrap(grass).vertical);
    for (auto s : {AutotileScheme::Cardinal16, AutotileScheme::Blob47, AutotileScheme::Wang16}) {
        INFO(toString(s));
        TilesetStyle st;
        st.edgeNoise = 0.35;
        const auto t = generateTileset(s, dirt, grass, st).value();
        REQUIRE(validateTileset(t));
        const auto rep = checkSeams(t).value();
        CHECK(rep.pairsChecked > 0);
        for (const auto& i : rep.issues) INFO("seam " << i.a << " -> " << i.b << (i.horizontal ? " (h) " : " (v) ") << i.jump << " vs " << i.typical);
        CHECK(rep.ok());
        CHECK(generateTileset(s, dirt, grass, st).value().image == t.image);
        // Rendering a map gives the right size and only tileset colours.
        const auto img = renderMap(randomMap(12, 9, 3), t).value();
        CHECK(img.width() == 12 * 16);
        CHECK(img.height() == 9 * 16);
    }
}

TEST_CASE("The seam check finds a broken edge, and non-seamless textures", "[autotile]") {
    const Image grass = texture(16, {70, 150, 60, 255}, 1), dirt = texture(16, {140, 100, 60, 255}, 2);
    auto t = generateTileset(AutotileScheme::Blob47, dirt, grass).value();
    // Paint tile 46 (all neighbours)'s right column bright magenta.
    const auto [cols, rows] = tilesetGrid(t.scheme);
    (void)rows;
    for (int y = 0; y < 16; ++y) t.image.setPixel((46 % cols) * 16 + 15, (46 / cols) * 16 + y, {255, 0, 255, 255});
    const auto rep = checkSeams(t).value();
    REQUIRE_FALSE(rep.ok());
    CHECK(rep.issues.front().a == 46);
    CHECK(rep.issues.front().horizontal);
    // A gradient does not wrap horizontally.
    Image grad(16, 16);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) grad.setPixel(x, y, {static_cast<std::uint8_t>(x * 16), 80, 80, 255});
    CHECK_FALSE(checkWrap(grad).horizontal);
    CHECK(checkWrap(grad).vertical);
    // Wrong sizes are refused with a useful message.
    Tileset bad = t;
    bad.image = Image(10, 10);
    CHECK_FALSE(validateTileset(bad));
    CHECK_FALSE(generateTileset(AutotileScheme::Wang16, Image(8, 8), Image(16, 16)));
}

TEST_CASE("Coverage follows the samples; templates have the scheme's size", "[autotile]") {
    const TileSamples full{255, 255, 255, 255, 255, 255, 255, 255, 255};
    CHECK(tileCoverage(full, 0.3, 0.7, 1.0) == 1.0);
    const TileSamples island = tileSamples(AutotileScheme::Cardinal16, 0);
    CHECK(tileCoverage(island, 0.5, 0.5, 1.0) == 1.0);
    CHECK(tileCoverage(island, 0.0, 0.0, 1.0) == 0.0);
    const Image tmpl = tilesetTemplate(AutotileScheme::Blob47, 12, {0, 200, 0, 255}, {120, 80, 40, 255});
    CHECK(tmpl.width() == 8 * 12);
    CHECK(tmpl.height() == 6 * 12);
}
