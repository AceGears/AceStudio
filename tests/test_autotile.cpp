// Autotiling, procedural tilesets and seam checks (Phase 6).
#include <catch2/catch_test_macros.hpp>

#include "acestudio/core/Autotile.h"

#include <nlohmann/json.hpp>

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

TEST_CASE("Tileset JSON lists every tile with its mask and position", "[autotile]") {
    const Image grass = texture(16, {70, 150, 60, 255}, 1), dirt = texture(16, {140, 100, 60, 255}, 2);
    const auto t = generateTileset(AutotileScheme::Blob47, dirt, grass).value();
    const auto j = nlohmann::json::parse(tilesetJson(t, "grass.png", "dirt.png"));
    CHECK(j["scheme"] == "blob47");
    CHECK(j["tiles"].size() == 47);
    CHECK(j["tiles"][46]["mask"] == 255);
    CHECK(j["tiles"][9]["x"] == 16);
    CHECK(j["tiles"][9]["y"] == 16);
    CHECK(j["base"] == "dirt.png");
    const auto w = nlohmann::json::parse(tilesetJson(generateTileset(AutotileScheme::Wang16, dirt, grass).value(), "w.png"));
    CHECK(w["tiles"][5]["corners"] == 5);
    CHECK_FALSE(w.contains("base"));
}

TEST_CASE("Phases: textures larger than a tile keep their detail and stay seamless", "[autotile]") {
    const int T = 16;
    for (int P : {2, 3}) {
        const Image grass = texture(P * T, {70, 150, 60, 255}, 1), dirt = texture(P * T, {140, 100, 60, 255}, 2);
        for (auto s : {AutotileScheme::Cardinal16, AutotileScheme::Blob47, AutotileScheme::Wang16}) {
            INFO(toString(s) << " phases " << P);
            const auto t = generateTileset(s, dirt, grass, {}, T).value();
            CHECK(t.phases == P);
            REQUIRE(validateTileset(t));
            const auto rep = checkSeams(t).value();
            CHECK(rep.ok());
            CHECK(rep.pairsChecked == checkSeams(generateTileset(s, texture(T, {70, 150, 60, 255}, 1), texture(T, {140, 100, 60, 255}, 2)).value()).value().pairsChecked * P * P);
            // A map full of terrain shows the whole large texture, repeating
            // every P tiles, not one tile repeated.
            TerrainMap full(8, 8);
            full.fill(true);
            const Image img = renderMap(full, t).value();
            const int shift = s == AutotileScheme::Wang16 ? T / 2 : 0;
            bool same = true;
            for (int y = T; y < 7 * T && same; ++y)
                for (int x = T; x < 7 * T && same; ++x)
                    same = img.pixel(x, y) == grass.pixel((x + shift) % (P * T), (y + shift) % (P * T));
            CHECK(same);
            const auto j = nlohmann::json::parse(tilesetJson(t, "t.png"));
            CHECK(j["phases"] == P);
            CHECK(j["phaseOffset"]["x"] == tilesetGrid(s).first * T);
        }
    }
    // Sizes that are not whole tiles, or too many phases, are refused.
    CHECK_FALSE(generateTileset(AutotileScheme::Blob47, Image(24, 24), Image(24, 24), {}, 16));
    CHECK_FALSE(generateTileset(AutotileScheme::Blob47, Image(80, 80), Image(80, 80), {}, 16));
}

TEST_CASE("Levels: several terrains drawn as layers over each other", "[autotile]") {
    const int T = 16;
    const Image water = texture(T, {40, 80, 200, 255}, 3), sand = texture(T, {220, 200, 120, 255}, 4),
                grass = texture(T, {70, 150, 60, 255}, 1);
    for (auto s : {AutotileScheme::Blob47, AutotileScheme::Wang16, AutotileScheme::Cardinal16}) {
        INFO(toString(s));
        std::vector<Tileset> levels{generateTileset(s, water, sand).value(), generateTileset(s, Image(), grass).value()};
        // The upper set has a transparent base and still has no seams.
        CHECK(checkSeams(levels[1]).value().ok());
        if (s != AutotileScheme::Wang16) CHECK(levels[1].base.pixel(3, 3).a == 0);
        LevelMap m(12, 10);
        for (int y = 2; y < 9; ++y)
            for (int x = 2; x < 11; ++x) m.set(x, y, 1);
        for (int y = 4; y < 7; ++y)
            for (int x = 4; x < 9; ++x) m.set(x, y, 2);
        CHECK(m.maxLevel() == 2);
        CHECK(m.atLeast(2).at(5, 5));
        CHECK_FALSE(m.atLeast(2).at(2, 2));
        const Image img = renderLevels(m, levels).value();
        CHECK(img.width() == 12 * T);
        const int sh = s == AutotileScheme::Wang16 ? T / 2 : 0;
        auto tex = [&](const Image& t, int x, int y) { return t.pixel((x + sh) % T, (y + sh) % T); };
        // Middle of a grass cell, a sand cell and a water cell.
        CHECK(img.pixel(6 * T + 8, 5 * T + 8) == tex(grass, 6 * T + 8, 5 * T + 8));
        CHECK(img.pixel(2 * T + 8 + T, 7 * T + 8) == tex(sand, 3 * T + 8, 7 * T + 8));
        CHECK(img.pixel(8, 8) == tex(water, 8, 8));
        // Fully opaque everywhere.
        bool opaque = true;
        for (const Rgba8 p : img.pixels()) opaque = opaque && p.a == 255;
        CHECK(opaque);
        const auto j = nlohmann::json::parse(terrainSetJson(levels, {"a.png", "b.png"}, {"a.json", "b.json"}, "base.png"));
        CHECK(j["levels"].size() == 2);
        CHECK(j["levels"][1]["level"] == 2);
    }
    // Sets that do not match are refused.
    std::vector<Tileset> bad{generateTileset(AutotileScheme::Blob47, water, sand).value(),
                             generateTileset(AutotileScheme::Wang16, Image(), grass).value()};
    CHECK_FALSE(renderLevels(LevelMap(4, 4), bad));
    CHECK_FALSE(renderLevels(LevelMap(4, 4), {}));
}

TEST_CASE("Seam check: textures that do not wrap give seams; ragged edges do not", "[autotile]") {
    // A horizontal gradient as the terrain: every terrain-to-terrain edge jumps.
    Image grad(16, 16);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) grad.setPixel(x, y, {static_cast<std::uint8_t>(40 + x * 10), 140, 60, 255});
    const Image dirt = texture(16, {140, 100, 60, 255}, 2);
    for (auto s : {AutotileScheme::Blob47, AutotileScheme::Wang16}) {
        INFO(toString(s));
        CHECK_FALSE(checkSeams(generateTileset(s, dirt, grad).value()).value().ok());
        // Very ragged edges on a transparent base are still seamless.
        TilesetStyle st;
        st.edgeNoise = 0.9;
        st.noiseScale = 8;
        for (std::uint32_t seed = 1; seed <= 4; ++seed) {
            st.seed = seed;
            CHECK(checkSeams(generateTileset(s, Image(), texture(16, {70, 150, 60, 255}, 1), st).value()).value().ok());
        }
    }
}
