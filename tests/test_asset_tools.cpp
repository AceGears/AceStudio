// Slicer, nine-slice, UI kit and parallax (Phase 6).
#include <algorithm>
#include <cmath>
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

TEST_CASE("Parallax: a step in a silhouette at the wrap is found", "[assets]") {
    // Rolling mountains whose height at the right edge is 6 px off the left
    // edge: the column mean barely changes, the outline visibly jumps.
    auto mountains = [](bool periodic) {
        Image m(200, 80, {240, 240, 250, 255});
        for (int x = 0; x < 200; ++x) {
            const double u = periodic ? x * 2 * 3.14159265 / 200 : 0;
            const double h = periodic ? 40 + 18 * std::sin(2 * u) + 10 * std::sin(5 * u) : 40 + 18 * std::sin(x / 13.0) + 10 * std::sin(x / 5.3);
            for (int y = 80 - static_cast<int>(h); y < 80; ++y) m.setPixel(x, y, {70, 50, 110, 255});
        }
        return m;
    };
    ParallaxSet set;
    set.viewWidth = 160;
    set.viewHeight = 80;
    set.layers = {{"mountains", mountains(false), 0.5, 0, true}};
    const auto issues = checkParallax(set);
    REQUIRE(issues.size() == 1);
    CHECK(issues[0].find("outline jumps") != std::string::npos);
    CHECK(checkWrap(set.layers[0].image).horizontal);   // the column mean alone misses it
    // The same shape with a period that divides the width wraps cleanly.
    set.layers[0].image = mountains(true);
    CHECK(checkParallax(set).empty());
    // And Make Seamless fixes the first one.
    set.layers[0].image = makeSeamlessX(mountains(false), 40).value();
    CHECK(checkParallax(set).empty());
}

TEST_CASE("Clear Sky: flat, gradient and every-pixel modes", "[assets]") {
    // A vertical gradient sky over a dark hill, with a pocket of sky colour
    // enclosed inside the hill.
    Image layer(40, 40);
    for (int y = 0; y < 40; ++y)
        for (int x = 0; x < 40; ++x) layer.setPixel(x, y, {static_cast<std::uint8_t>(250 - y * 4), 160, 220, 255});
    for (int y = 25; y < 40; ++y)
        for (int x = 0; x < 40; ++x) layer.setPixel(x, y, {40, 30, 60, 255});
    for (int y = 30; y < 33; ++y)
        for (int x = 10; x < 13; ++x) layer.setPixel(x, y, layer.pixel(0, 0));   // pocket of the top colour
    const Image flat = clearSky(layer, 12, SkyClear::Flat);
    CHECK(flat.pixel(5, 0).a == 0);
    CHECK(flat.pixel(5, 20).a == 255);        // the gradient drifted too far from the top colour
    const Image grad = clearSky(layer, 12, SkyClear::Gradient);
    CHECK(grad.pixel(5, 20).a == 0);          // followed the gradient down to the hill
    CHECK(grad.pixel(5, 24).a == 0);
    CHECK(grad.pixel(5, 26).a == 255);        // the hill stays
    CHECK(grad.pixel(11, 31).a == 255);       // the enclosed pocket stays
    const Image every = clearSky(layer, 12, SkyClear::Every);
    CHECK(every.pixel(11, 31).a == 0);        // ...unless every matching pixel goes
    CHECK(every.pixel(5, 26).a == 255);
}

TEST_CASE("Slicer: a picked background colour and a frame around the sheet", "[assets]") {
    // Magenta sheet with a black 1 px frame: the border colour is black, so
    // the automatic background is wrong until magenta is picked.
    Image sheet(96, 48, {255, 0, 255, 255});
    for (int x = 0; x < 96; ++x) { sheet.setPixel(x, 0, {0, 0, 0, 255}); sheet.setPixel(x, 47, {0, 0, 0, 255}); }
    for (int y = 0; y < 48; ++y) { sheet.setPixel(0, y, {0, 0, 0, 255}); sheet.setPixel(95, y, {0, 0, 0, 255}); }
    const IntRect boxes[] = {{6, 6, 14, 12}, {30, 8, 14, 12}, {60, 5, 14, 12}, {10, 28, 14, 12}, {40, 26, 14, 12}, {70, 30, 14, 12}};
    for (const auto& b : boxes) sheet.fillRect(b, {40, 120, 60, 255});
    SliceOptions o;
    o.background = Rgba8{255, 0, 255, 255};
    const auto found = detectSprites(sheet, o);
    REQUIRE(found.size() == 6);
    for (const auto& b : boxes)
        CHECK(std::any_of(found.begin(), found.end(), [&](const SlicedSprite& s) { return s.rect == b; }));
}

TEST_CASE("Clear Sky, Split: banded pixel-art sky against a silhouette", "[assets]") {
    // Sky in 6 bands, each 30 brighter than the one below, over a castle
    // with a tower that reaches the top edge.
    auto scene = [](int castleShade) {
        Image img(60, 48);
        for (int y = 0; y < 48; ++y)
            for (int x = 0; x < 60; ++x) {
                const int band = y / 6;   // 0..7
                const auto v = static_cast<std::uint8_t>(230 - band * 30 < 40 ? 40 : 230 - band * 30);
                img.setPixel(x, y, {v, static_cast<std::uint8_t>(v * 3 / 4), static_cast<std::uint8_t>(v / 2 + 60), 255});
            }
        const auto c = static_cast<std::uint8_t>(castleShade);
        for (int y = 30; y < 48; ++y)
            for (int x = 0; x < 60; ++x) img.setPixel(x, y, {c, c, static_cast<std::uint8_t>(c + 10), 255});   // hill
        for (int y = 14; y < 30; ++y)
            for (int x = 20; x < 32; ++x) img.setPixel(x, y, {c, c, static_cast<std::uint8_t>(c + 10), 255});  // keep
        for (int y = 0; y < 14; ++y)
            for (int x = 40; x < 44; ++x) img.setPixel(x, y, {c, c, static_cast<std::uint8_t>(c + 10), 255});  // tower to the top
        for (int y = 14; y < 30; ++y)
            for (int x = 40; x < 44; ++x) img.setPixel(x, y, {c, c, static_cast<std::uint8_t>(c + 10), 255});
        return img;
    };
    for (int shade : {30, 70}) {
        INFO("castle shade " << shade);
        const Image src = scene(shade);
        const Image out = clearSky(src, 0, SkyClear::Split);
        // Every castle pixel stays.
        bool castleKept = true;
        for (int y = 0; y < 48; ++y)
            for (int x = 0; x < 60; ++x) {
                const bool castle = y >= 30 || (x >= 20 && x < 32 && y >= 14) || (x >= 40 && x < 44);
                if (castle && out.pixel(x, y).a != 255) castleKept = false;
            }
        CHECK(castleKept);
        // The upper sky goes (bands are small steps compared with the edge).
        int cleared = 0, sky = 0;
        for (int y = 0; y < 14; ++y)
            for (int x = 0; x < 20; ++x) { ++sky; cleared += out.pixel(x, y).a == 0; }
        CHECK(cleared == sky);
    }
    // Flood by tolerance would either stop at the first band or eat the castle.
    CHECK(clearSky(scene(70), 6, SkyClear::Gradient).pixel(5, 10).a == 255);
    // Wrapping: a repeating layer is split the same at both edges.
    const Image wrapped = clearSky(scene(30), 0, SkyClear::Split, true);
    for (int y = 0; y < 48; ++y) CHECK((wrapped.pixel(0, y).a == 0) == (wrapped.pixel(59, y).a == 0));
}

TEST_CASE("refineCutout: brings back sprite parts the mask missed, not the background", "[assets]") {
    // A noisy gradient background with a knight-like sprite: grey body,
    // dark legs.
    Image sheet(40, 40);
    std::uint32_t r = 7;
    for (int y = 0; y < 40; ++y)
        for (int x = 0; x < 40; ++x) {
            r = r * 1664525u + 1013904223u;
            const int n = static_cast<int>(r >> 27) - 16;   // -16..15
            sheet.setPixel(x, y, {static_cast<std::uint8_t>(100 + y + n), static_cast<std::uint8_t>(140 - y + n), static_cast<std::uint8_t>(170 + n), 255});
        }
    const IntRect body{14, 8, 12, 16}, legs{16, 24, 8, 8};
    sheet.fillRect(body, {150, 150, 160, 255});
    sheet.fillRect(legs, {25, 20, 30, 255});
    // The AI mask: the body with soft alpha, the legs lost, a stray speck.
    Image cut(40, 40);
    for (int y = body.y; y < body.y + body.h; ++y)
        for (int x = body.x; x < body.x + body.w; ++x) {
            Rgba8 p = sheet.pixel(x, y);
            p.a = (x == body.x || y == body.y) ? 140 : 255;
            cut.setPixel(x, y, p);
        }
    const Image out = refineCutout(sheet, cut);
    int legsBack = 0, bgKept = 0;
    for (int y = 0; y < 40; ++y)
        for (int x = 0; x < 40; ++x) {
            const bool inBody = x >= body.x && x < body.x + body.w && y >= body.y && y < body.y + body.h;
            const bool inLegs = x >= legs.x && x < legs.x + legs.w && y >= legs.y && y < legs.y + legs.h;
            const std::uint8_t a = out.pixel(x, y).a;
            if (inLegs) legsBack += a == 255;
            else if (inBody) CHECK(a == 255);           // soft edge snapped to opaque
            else bgKept += a != 0;
        }
    CHECK(legsBack == legs.w * legs.h);
    CHECK(bgKept == 0);
    // Original colours are kept.
    CHECK(out.pixel(18, 28) == sheet.pixel(18, 28));
}
