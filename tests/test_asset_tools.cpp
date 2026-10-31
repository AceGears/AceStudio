// Slicer, nine-slice, UI kit and parallax (Phase 6).
#include <catch2/catch_test_macros.hpp>

#include "acestudio/core/AssetTools.h"
#include "acestudio/core/Autotile.h"

#include <nlohmann/json.hpp>

using namespace acestudio;

namespace {
const Rgba8 kRed{200, 40, 40, 255}, kBlue{40, 60, 200, 255}, kBg{255, 0, 255, 255};
// 4 x 2 grid of 16 px cells; each holds a small figure plus a detached "sword".
Image gridSheet(bool transparent) {
    Image img(64, 32, transparent ? Rgba8::transparent() : kBg);
    for (int r = 0; r < 2; ++r)
        for (int c = 0; c < 4; ++c) {
            if (r == 1 && c == 3) continue;   // one empty cell
            const int ox = c * 16, oy = r * 16;
            img.fillRect({ox + 4, oy + 3, 6, 10}, kRed);
            img.fillRect({ox + 11, oy + 5, 1, 6}, kBlue);    // 1 px gap from the body
        }
    return img;
}
} // namespace

TEST_CASE("Smart slicer finds sprites (merging detached parts) and the grid", "[assets]") {
    for (bool transparent : {true, false}) {
        INFO(transparent);
        const Image sheet = gridSheet(transparent);
        CHECK(sliceBackground(sheet).has_value() == !transparent);
        const auto sprites = detectSprites(sheet);
        REQUIRE(sprites.size() == 7);
        CHECK(sprites[0].name == "sprite_000");
        CHECK(sprites[0].rect == IntRect{4, 3, 8, 10});           // body + sword merged
        CHECK(sprites[4].rect.y == 19);                          // second row after the first
        const auto g = detectGrid(sheet);
        REQUIRE(g.found);
        CHECK(g.cellWidth == 16);
        CHECK(g.cellHeight == 16);
        CHECK(g.columns == 4);
        CHECK(g.rows == 2);
        CHECK(sliceGrid(sheet, g).size() == 7);                   // the empty cell is skipped
        CHECK(sliceGrid(sheet, g, false).size() == 8);
    }
    SliceOptions apart;
    apart.mergeDistance = 0;
    CHECK(detectSprites(gridSheet(true), apart).size() == 14);    // body and sword separate
    const auto j = nlohmann::json::parse(slicesJson(detectSprites(gridSheet(true)), "sheet.png", 64, 32));
    CHECK(j["frames"]["sprite_006"]["frame"]["x"] == 36);
    // Spacing between cells is detected.
    Image spaced(16 * 3 + 2 * 2, 16, Rgba8::transparent());
    for (int c = 0; c < 3; ++c) spaced.fillRect({c * 18 + 2, 2, 12, 12}, kRed);
    const auto gs = detectGrid(spaced);
    CHECK(gs.cellWidth == 16);
    CHECK(gs.spacingX == 2);
    CHECK(gs.columns == 3);
}

TEST_CASE("Nine-slice: detection, stretch and tile rendering", "[assets]") {
    // A 12x12 frame: 3 px border (outline + bevel), uniform centre.
    Image f(12, 12, kBlue);
    for (int y = 0; y < 12; ++y)
        for (int x = 0; x < 12; ++x)
            if (std::min({x, y, 11 - x, 11 - y}) < 3) f.setPixel(x, y, {static_cast<std::uint8_t>(40 * std::min({x, y, 11 - x, 11 - y})), 20, 20, 255});
    const NineSlice n = detectNineSlice(f);
    CHECK(n.exact);
    CHECK(n == NineSlice{3, 3, 3, 3, true});
    const Image big = renderNineSlice(f, n, 40, 20).value();
    CHECK(big.width() == 40);
    CHECK(big.pixel(0, 0) == f.pixel(0, 0));
    CHECK(big.pixel(39, 19) == f.pixel(11, 11));
    CHECK(big.pixel(20, 10) == kBlue);
    CHECK(big.pixel(20, 0) == f.pixel(5, 0));       // top edge stretched
    CHECK(renderNineSlice(f, n, 40, 20, NineSliceFill::Tile).value().pixel(37, 2) == f.pixel(9, 2));
    CHECK_FALSE(renderNineSlice(f, n, 4, 20));      // smaller than the borders
    Image grad(9, 9);
    for (int y = 0; y < 9; ++y)
        for (int x = 0; x < 9; ++x) grad.setPixel(x, y, {static_cast<std::uint8_t>(x * 20), static_cast<std::uint8_t>(y * 20), 0, 255});
    CHECK_FALSE(detectNineSlice(grad).exact);
}

TEST_CASE("UI kit: every stretchable element's nine-slice matches what detection finds", "[assets]") {
    for (int scale : {1, 2}) {
        UiTheme th;
        th.scale = scale;
        const auto kit = generateUiKit(th);
        CHECK(kit.size() >= 18);
        for (const auto& e : kit) {
            INFO(e.name << " @" << scale);
            CHECK_FALSE(e.image.isNull());
            if (!e.stretchable || e.name == "divider") continue;
            const NineSlice d = detectNineSlice(e.image);
            CHECK(d.exact);
            CHECK(d.left == e.slice.left);
            CHECK(d.top == e.slice.top);
            CHECK(d.right == e.slice.right);
            CHECK(d.bottom == e.slice.bottom);
            CHECK(renderNineSlice(e.image, e.slice, e.image.width() * 3, e.image.height() * 2));
        }
        const auto files = packUiKit(kit, "ui.png").value();
        const auto j = nlohmann::json::parse(files.json);
        CHECK(j["frames"]["button_normal"]["nineSlice"]["left"] == 2 * scale);
        CHECK(j["frames"]["checkbox_on"]["stretchable"] == false);
        CHECK(j["meta"]["image"] == "ui.png");
    }
    // Deterministic.
    CHECK(generateUiKit({})[3].image == generateUiKit({})[3].image);
}

TEST_CASE("Parallax: wrap check, rendering at camera positions, seamless fix", "[assets]") {
    // A layer whose right edge does not continue into its left edge.
    Image hills(64, 20, Rgba8::transparent());
    for (int x = 0; x < 64; ++x)
        for (int y = 20 - 4 - x / 8; y < 20; ++y) hills.setPixel(x, y, {40, static_cast<std::uint8_t>(90 + x), 40, 255});
    Image sky(32, 40, {120, 170, 230, 255});
    ParallaxSet set;
    set.viewWidth = 48;
    set.viewHeight = 40;
    set.layers = {{"sky", sky, 0.0, 0, true}, {"hills", hills, 0.5, 20, true}};
    auto issues = checkParallax(set);
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].find("hills") != std::string::npos);
    const Image fixed = makeSeamlessX(hills, 16).value();
    CHECK(fixed.width() == 48);
    CHECK(checkWrap(fixed).horizontal);
    // Every output pixel comes from the original (dithered, not blended).
    for (int y = 0; y < fixed.height(); ++y)
        for (int x = 0; x < 16; ++x) {
            const Rgba8 p = fixed.pixel(x, y);
            CHECK((p == hills.pixel(x, y) || p == hills.pixel(48 + x, y)));
        }
    set.layers[1].image = fixed;
    CHECK(checkParallax(set).empty());
    const Image a = renderParallax(set, 0), b = renderParallax(set, 20);
    CHECK(a.width() == 48);
    CHECK(a.pixel(5, 2) == b.pixel(5, 2));                    // the sky does not move
    CHECK(b.pixel(0, 39) == fixed.pixel(10, 19));            // hills move at half speed
    CHECK(renderParallax(set, 96) == renderParallax(set, 0));   // 96 * 0.5 = one full hill width
    const auto j = nlohmann::json::parse(parallaxJson(set, {"sky.png", "hills.png"}));
    CHECK(j["layers"][1]["scroll"] == 0.5);
    CHECK(j["layers"][1]["file"] == "hills.png");
    CHECK_FALSE(makeSeamlessX(hills, 40));
}
