// Deterministic atlas packing (Phase 6).
#include <catch2/catch_test_macros.hpp>

#include "acestudio/core/Atlas.h"

#include <nlohmann/json.hpp>

#include <random>

using namespace acestudio;

namespace {
Image solid(int w, int h, std::uint8_t v) {
    Image img(w, h);
    img.fill({v, static_cast<std::uint8_t>(255 - v), static_cast<std::uint8_t>(v / 2), 255});
    return img;
}
std::vector<AtlasItem> randomItems(int n, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> d(4, 60);
    std::vector<AtlasItem> items;
    for (int i = 0; i < n; ++i) items.push_back({"s" + std::to_string(i), solid(d(rng), d(rng), static_cast<std::uint8_t>(i * 7))});
    return items;
}
bool overlap(const IntRect& a, const IntRect& b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}
} // namespace

TEST_CASE("packRects places every rect without overlap, inside the bin, with gaps", "[atlas]") {
    std::vector<std::pair<int, int>> sizes = {{30, 10}, {10, 30}, {20, 20}, {5, 5}, {40, 8}};
    const auto r = packRects(sizes, 64, 64, 2);
    REQUIRE(r.size() == sizes.size());
    for (std::size_t i = 0; i < r.size(); ++i) {
        CHECK(r[i].w == sizes[i].first);
        CHECK(r[i].h == sizes[i].second);
        CHECK(r[i].x >= 0);
        CHECK(r[i].x + r[i].w <= 64);
        CHECK(r[i].y + r[i].h <= 64);
        for (std::size_t j = i + 1; j < r.size(); ++j) {
            const IntRect gi{r[i].x, r[i].y, r[i].w + 2, r[i].h + 2}, gj{r[j].x, r[j].y, r[j].w + 2, r[j].h + 2};
            CHECK_FALSE(overlap(gi, r[j]));
            CHECK_FALSE(overlap(gj, r[i]));
        }
    }
    CHECK(packRects({{65, 1}}, 64, 64).empty());
}

TEST_CASE("Atlas packing is deterministic and byte-identical across runs", "[atlas]") {
    const auto items = randomItems(80, 7);
    const auto a = packAtlas(items).value();
    const auto b = packAtlas(items).value();
    REQUIRE(a.pages.size() == b.pages.size());
    for (std::size_t p = 0; p < a.pages.size(); ++p) CHECK(a.pages[p] == b.pages[p]);
    CHECK(atlasJson(a, {"atlas.png"}) == atlasJson(b, {"atlas.png"}));
    // No overlaps; every sprite's pixels are where the atlas says.
    for (std::size_t i = 0; i < a.sprites.size(); ++i) {
        const auto& s = a.sprites[i];
        CHECK(a.pages[0].copy(s.rect) == items[i].image);
        for (std::size_t j = i + 1; j < a.sprites.size(); ++j) CHECK_FALSE(overlap(s.rect, a.sprites[j].rect));
    }
    // Reasonably tight: at least 75% of the page is used.
    long long used = 0;
    for (const auto& s : a.sprites) used += static_cast<long long>(s.rect.w) * s.rect.h;
    CHECK(static_cast<double>(used) / (static_cast<double>(a.pages[0].width()) * a.pages[0].height()) > 0.75);
}

TEST_CASE("Atlas: trim, duplicates, extrusion, padding, border and power of two", "[atlas]") {
    Image framed(20, 20);
    framed.fillRect({5, 6, 8, 4}, {255, 0, 0, 255});
    std::vector<AtlasItem> items = {{"a", framed}, {"b", framed}, {"c", solid(10, 10, 9)}};
    AtlasOptions o;
    o.trim = true;
    o.extrude = 1;
    o.padding = 2;
    o.border = 3;
    o.powerOfTwo = true;
    const auto at = packAtlas(items, o).value();
    REQUIRE(at.pages.size() == 1);
    const auto& a = at.sprites[0];
    CHECK(a.trimmed);
    CHECK(a.source == IntRect{5, 6, 8, 4});
    CHECK(a.sourceWidth == 20);
    CHECK(at.sprites[1].duplicateOf == 0);
    CHECK(at.sprites[1].rect == a.rect);
    const Image& page = at.pages[0];
    CHECK((page.width() & (page.width() - 1)) == 0);
    CHECK((page.height() & (page.height() - 1)) == 0);
    // Extruded edge repeats the sprite's edge; border stays empty.
    CHECK(page.pixel(a.rect.x - 1, a.rect.y) == page.pixel(a.rect.x, a.rect.y));
    CHECK(page.pixel(a.rect.x + a.rect.w, a.rect.y + a.rect.h) == page.pixel(a.rect.x + a.rect.w - 1, a.rect.y + a.rect.h - 1));
    for (const auto& s : at.sprites) {
        CHECK(s.rect.x - o.extrude >= o.border);
        CHECK(s.rect.y - o.extrude >= o.border);
    }
    // Padding between distinct sprites (beyond their extrusion).
    const auto& c = at.sprites[2];
    const IntRect ea{a.rect.x - 1, a.rect.y - 1, a.rect.w + 2 + o.padding, a.rect.h + 2 + o.padding};
    const IntRect ec{c.rect.x - 1, c.rect.y - 1, c.rect.w + 2 + o.padding, c.rect.h + 2 + o.padding};
    CHECK_FALSE(overlap(ea, {c.rect.x - 1, c.rect.y - 1, c.rect.w + 2, c.rect.h + 2}));
    CHECK_FALSE(overlap(ec, {a.rect.x - 1, a.rect.y - 1, a.rect.w + 2, a.rect.h + 2}));
    // JSON keeps the trim information.
    const auto j = nlohmann::json::parse(atlasJson(at, {"ui.png"}));
    CHECK(j["frames"]["a"]["spriteSourceSize"]["x"] == 5);
    CHECK(j["frames"]["a"]["sourceSize"]["w"] == 20);
    CHECK(j["meta"]["image"] == "ui.png");
    CHECK(j["frames"].begin().key() == "a");      // input order
}

TEST_CASE("Atlas: several pages when one is not enough; errors are clear", "[atlas]") {
    std::vector<AtlasItem> items;
    for (int i = 0; i < 10; ++i) items.push_back({"t" + std::to_string(i), solid(30, 30, static_cast<std::uint8_t>(i * 20))});
    AtlasOptions o;
    o.maxWidth = 64;
    o.maxHeight = 64;
    const auto at = packAtlas(items, o).value();
    CHECK(at.pages.size() == 3);                 // 4 + 4 + 2
    const auto j = nlohmann::json::parse(atlasJson(at, {"p0.png", "p1.png", "p2.png"}));
    REQUIRE(j["textures"].size() == 3);
    CHECK(j["textures"][2]["image"] == "p2.png");
    std::size_t frames = 0;
    for (const auto& t : j["textures"]) frames += t["frames"].size();
    CHECK(frames == 10);
    o.multiplePages = false;
    CHECK_FALSE(packAtlas(items, o));
    CHECK_FALSE(packAtlas({{"big", solid(65, 10, 1)}}, AtlasOptions{64, 64}));
    CHECK_FALSE(packAtlas({{"x", solid(2, 2, 1)}, {"x", solid(3, 3, 1)}}));
    CHECK_FALSE(packAtlas({}));
}
