#include "acestudio/core/AssetGen.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>

using namespace acestudio;
using namespace acestudio::gen;

TEST_CASE("Prompts carry type, perspective, style and isolation", "[gen]") {
    Spec s;
    s.description = "a dwarf blacksmith with a glowing hammer.";
    s.type = AssetType::Character;
    s.style = ArtStyle::DetailedPixel;
    s.perspective = Perspective::Isometric;
    const Prompt k = composePrompt(s, Backend::Klein, true);
    CHECK(k.positive.rfind("pixel art sprite, a dwarf blacksmith with a glowing hammer, ", 0) == 0);
    CHECK(k.positive.find("isometric view") != std::string::npos);
    CHECK(k.positive.find("isolated on a plain white background") != std::string::npos);
    CHECK(k.negative.empty());   // klein takes no negative prompt
    const Prompt x = composePrompt(s, Backend::Sdxl, true);
    CHECK(x.positive.rfind("pixel art, ", 0) == 0);
    CHECK(x.negative.find("sprite sheet") != std::string::npos);

    s.type = AssetType::Terrain;
    const Prompt t = composePrompt(s, Backend::Klein, false);
    CHECK(t.positive.find("seamless") != std::string::npos);
    CHECK(t.positive.find("isolated") == std::string::npos);
    CHECK(t.positive.find("isometric view") == std::string::npos);
    s.style = ArtStyle::HdPainted;
    s.type = AssetType::Monster;
    CHECK(composePrompt(s, Backend::Klein, true).positive.rfind("pixel art", 0) != 0);   // no pixel trigger for HD
}

TEST_CASE("Generation sizes follow the target aspect at the preset size", "[gen]") {
    CHECK(generationSize(64, 64, QualityPreset::Balanced) == std::pair{1024, 1024});
    CHECK(generationSize(64, 64, QualityPreset::FastDraft) == std::pair{768, 768});
    const auto [w, h] = generationSize(480, 270, QualityPreset::Balanced);
    CHECK(w == 1024);
    CHECK(h == 576);
    CHECK(w % 16 == 0);
    CHECK(h % 16 == 0);
    const auto [w2, h2] = generationSize(16, 160, QualityPreset::Balanced);   // clamped aspect
    CHECK(h2 == 1024);
    CHECK(w2 == 256);
}

TEST_CASE("Asset checks find cut-off, opaque, noisy and soft-edged results", "[gen]") {
    CheckOptions opt;
    opt.expectWidth = 32;
    opt.expectHeight = 32;
    // A clean sprite: one blob in the middle, hard alpha, few colours.
    Image good(32, 32);
    good.fillRect({8, 6, 16, 22}, Rgba8::opaque(200, 40, 40));
    good.fillRect({12, 10, 8, 4}, Rgba8::opaque(250, 220, 120));
    QualityReport r = checkAsset(good, opt);
    CHECK(r.issues.empty());
    CHECK(r.colors == 2);
    CHECK(r.pieces == 1);
    CHECK(r.score() == 100);

    Image opaque(32, 32, Rgba8::opaque(255, 255, 255));
    r = checkAsset(opaque, opt);
    REQUIRE_FALSE(r.issues.empty());
    CHECK(r.issues.front().code == "opaque");

    Image cut = good;
    cut.fillRect({0, 10, 4, 4}, Rgba8::opaque(200, 40, 40));
    cut.fillRect({4, 10, 4, 4}, Rgba8::opaque(200, 40, 40));
    r = checkAsset(cut, opt);
    CHECK(r.touchesEdge);
    CHECK(r.score() < 100);

    Image noisy = good;
    for (int i = 0; i < 30; ++i) noisy.setPixel((i * 7) % 32, (i * 11) % 5 + 27, Rgba8::opaque(10, 10, 10));
    noisy.setPixel(30, 1, Rgba8{255, 0, 0, 100});
    r = checkAsset(noisy, opt);
    bool stray = false, soft = false;
    for (const Issue& i : r.issues) {
        stray = stray || i.code == "stray";
        soft = soft || i.code == "soft-alpha";
    }
    CHECK(stray);
    CHECK(soft);

    Palette pal;
    REQUIRE(pal.add(Rgba8::opaque(0, 0, 255)));
    opt.stylePalette = &pal;
    r = checkAsset(good, opt);
    REQUIRE(r.paletteDistance);
    CHECK(*r.paletteDistance > 0.3);

    CheckOptions sizeOpt;
    sizeOpt.expectWidth = 64;
    sizeOpt.expectHeight = 64;
    CHECK(checkAsset(good, sizeOpt).issues.front().code == "size");
}

TEST_CASE("Reference roles limit what each reference contributes", "[gen]") {
    Spec s;
    s.description = "a goblin archer";
    s.referenceRoles = {"character", "style"};
    const std::string k = composePrompt(s, Backend::Klein, true).positive;
    CHECK(k.find("Keep the character from reference image 1 exactly") != std::string::npos);
    CHECK(k.find("art style of reference image 2") != std::string::npos);
    CHECK(k.find("do not copy its subject") != std::string::npos);
    // SDXL receives no reference images, so no reference instructions.
    CHECK(composePrompt(s, Backend::Sdxl, true).positive.find("reference image") == std::string::npos);
}

TEST_CASE("generated sheets are cut into their cells", "[gen]") {
    // A 3 x 2 sheet on an opaque background; the creatures are not centred
    // in their cells and one cell is empty, and there is a speck.
    Image sheet(300, 200, Rgba8{240, 236, 220, 255});
    const Rgba8 body{120, 40, 160, 255};
    sheet.fillRect({10, 20, 60, 50}, body);       // cell (0,0), off-centre
    sheet.fillRect({120, 30, 40, 60}, body);      // cell (1,0)
    sheet.fillRect({215, 15, 50, 70}, body);      // cell (2,0)
    sheet.fillRect({30, 120, 40, 40}, body);      // cell (0,1)
    sheet.fillRect({140, 130, 2, 2}, body);       // speck in cell (1,1): empty
    sheet.fillRect({220, 110, 60, 80}, body);     // cell (2,1)
    sheet.fillRect({274, 112, 2, 2}, Rgba8{10, 10, 10, 255});  // speck next to it
    const auto cells = gen::extractGrid(sheet, 3, 2);
    REQUIRE(cells.size() == 5);
    CHECK(cells[0].image.width() == 60);
    CHECK(cells[0].image.height() == 50);
    CHECK(cells[0].rect.x == 10);
    CHECK(cells[3].column == 0);
    CHECK(cells[3].row == 1);
    CHECK(cells[4].column == 2);
    CHECK(cells[4].image.width() == 60);    // the speck was dropped
    for (const auto& c : cells) CHECK(c.image.pixel(0, 0).a == 255);
    CHECK(gen::gridPrompt("slimes", 3, 3).find("9 different slimes") != std::string::npos);
}

TEST_CASE("a failed generation that is only noise is flagged", "[gen]") {
    Image noise(64, 64);
    std::uint32_t state = 12345;
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            state = state * 1664525u + 1013904223u;
            const auto v = static_cast<std::uint8_t>(state >> 24);
            noise.setPixel(x, y, Rgba8{v, static_cast<std::uint8_t>(v / 2), 40, 255});
        }
    gen::CheckOptions o;
    o.expectTransparent = false;
    o.singleSubject = false;
    const auto bad = gen::checkAsset(noise, o);
    bool flagged = false;
    for (const auto& i : bad.issues) flagged = flagged || i.code == "noise";
    CHECK(bad.score() == 0);   // ranks below every real result
    CHECK(gen::checkAsset(Image(16, 16), o).score() == 0);   // fully transparent
    CHECK(flagged);
    // A smooth gradient and blocky pixel art are not noise.
    Image ramp(64, 64);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) ramp.setPixel(x, y, Rgba8{static_cast<std::uint8_t>(x * 4), static_cast<std::uint8_t>(y * 4), 90, 255});
    for (const auto& i : gen::checkAsset(ramp, o).issues) CHECK(i.code != "noise");
    Image blocks(64, 64);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) blocks.setPixel(x, y, ((x / 4 + y / 4) % 3) ? Rgba8{200, 60, 60, 255} : Rgba8{30, 30, 40, 255});
    for (const auto& i : gen::checkAsset(blocks, o).issues) CHECK(i.code != "noise");
}

TEST_CASE("a thin outlined 32 px sprite is not taken for noise", "[gen]") {
    // A key-like sprite: one-pixel shafts with a dark outline on each side, so
    // horizontal neighbours alternate dark and light (low correlation, as the
    // silver key generated on the test PC: 0.06), but the colours run
    // vertically. Palette noise at the same size is still flagged.
    gen::CheckOptions o;
    o.expectTransparent = true;
    o.pixelArt = true;
    Image key(32, 32);
    const Rgba8 ink{20, 22, 30, 255}, steel{200, 205, 215, 255}, shine{250, 250, 255, 255};
    for (int col = 0; col < 5; ++col) {
        const int x = 4 + col * 5;
        for (int y = 3; y < 29; ++y) {
            key.setPixel(x, y, ink);
            key.setPixel(x + 1, y, (y / 3) % 2 ? steel : shine);
            key.setPixel(x + 2, y, ink);
        }
    }
    for (const auto& i : gen::checkAsset(key, o).issues) CHECK(i.code != "noise");
    Image noise(32, 32);
    std::uint32_t state = 99;
    const Rgba8 pal[16] = {{20, 20, 30, 255},   {200, 60, 60, 255},  {60, 200, 60, 255},  {60, 60, 200, 255},
                           {220, 220, 60, 255}, {60, 220, 220, 255}, {220, 60, 220, 255}, {240, 240, 240, 255},
                           {120, 80, 40, 255},  {40, 120, 80, 255},  {80, 40, 120, 255},  {160, 160, 160, 255},
                           {250, 150, 30, 255}, {30, 150, 250, 255}, {150, 250, 30, 255}, {90, 90, 90, 255}};
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) {
            state = state * 1664525u + 1013904223u;
            noise.setPixel(x, y, pal[state >> 28]);
        }
    o.expectTransparent = false;
    bool flagged = false;
    for (const auto& i : gen::checkAsset(noise, o).issues) flagged = flagged || i.code == "noise";
    CHECK(flagged);
}

TEST_CASE("a cell holding two subjects gives two sprites", "[gen]") {
    Image sheet(200, 100, Rgba8{250, 250, 250, 255});
    const Rgba8 body{40, 120, 60, 255};
    sheet.fillRect({10, 5, 60, 35}, body);     // cell (0,0): two creatures stacked
    sheet.fillRect({15, 55, 50, 35}, body);
    sheet.fillRect({120, 20, 60, 60}, body);   // cell (1,0): one
    const auto cells = gen::extractGrid(sheet, 2, 1);
    REQUIRE(cells.size() == 3);
    CHECK(cells[0].column == 0);
    CHECK(cells[1].column == 0);
    CHECK(cells[2].column == 1);
}

TEST_CASE("creatures drawn across the grid lines are kept whole", "[gen]") {
    // As on the test PC: 7 x 7 was asked for, the model drew 3 rows of 4
    // larger creatures that straddle the cell lines, some with a detached horn.
    Image sheet(700, 700, Rgba8{250, 250, 250, 255});
    const Rgba8 body{40, 120, 60, 255};
    int n = 0;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c, ++n) {
            const int x = 20 + c * 170, y = 40 + r * 220;
            sheet.fillRect({x, y + 30, 130, 120}, body);   // 130 px: spans two 100 px cells
            if (n % 3 == 0) sheet.fillRect({x + 100, y + 18, 6, 6}, body);   // a horn, 6 px above the body
        }
    const auto cells = gen::extractGrid(sheet, 7, 7);
    REQUIRE(cells.size() == 12);
    for (const auto& c : cells) {
        INFO(c.rect.x << "," << c.rect.y << " " << c.rect.w << "x" << c.rect.h);
        CHECK(c.rect.w == 130);
        CHECK((c.rect.h == 120 || c.rect.h == 132));   // with its horn
    }
    // Reading order.
    CHECK(cells[0].rect.x < cells[1].rect.x);
    CHECK(cells[3].rect.y < cells[4].rect.y);
}

TEST_CASE("creatures drawn touching are still split by the grid", "[gen]") {
    Image sheet(300, 100, Rgba8{250, 250, 250, 255});
    const Rgba8 body{40, 120, 60, 255};
    sheet.fillRect({10, 20, 180, 60}, body);   // two creatures touching: one piece over cells 0 and 1
    sheet.fillRect({215, 20, 70, 60}, body);   // cell 2
    const auto cells = gen::extractGrid(sheet, 3, 1);
    REQUIRE(cells.size() == 3);
    CHECK(cells[0].column == 0);
    CHECK(cells[1].column == 1);
    CHECK(cells[2].column == 2);
}

TEST_CASE("generic bestiaries get different creature kinds per sheet", "[gen]") {
    const std::string a = gen::bestiaryKinds("fantasy RPG monsters", 0, 9), b = gen::bestiaryKinds("fantasy RPG monsters", 1, 9);
    CHECK_FALSE(a.empty());
    CHECK(a != b);
    CHECK(std::count(a.begin(), a.end(), ',') == 8);
    CHECK(gen::bestiaryKinds("blue slimes", 0, 9).empty());   // a specific subject is left as asked
}

TEST_CASE("dark clothes on a black sheet are not cut away with the background", "[gen]") {
    // As on a generated walk sheet: a flat black background (0 or 1 per
    // channel) and characters whose trousers are a dark grey that the old
    // fixed tolerance took for background, leaving the boots as a separate
    // piece under the jacket.
    Image sheet(240, 120, Rgba8{0, 0, 0, 255});
    for (int y = 0; y < 120; ++y)
        for (int x = (y * 7) % 5; x < 240; x += 5) sheet.setPixel(x, y, Rgba8{1, 0, 1, 255});   // faint noise
    const Rgba8 jacket{170, 30, 30, 255}, trousers{22, 22, 26, 255}, boots{200, 200, 210, 255};
    for (int c = 0; c < 2; ++c) {
        const int x = 30 + c * 120;
        sheet.fillRect({x, 10, 60, 50}, jacket);
        sheet.fillRect({x + 10, 60, 40, 30}, trousers);
        sheet.fillRect({x + 5, 90, 50, 14}, boots);
    }
    const auto cells = gen::extractGrid(sheet, 2, 1);
    REQUIRE(cells.size() == 2);
    for (const auto& c : cells) {
        CHECK(c.rect.w == 60);
        CHECK(c.rect.h == 94);   // jacket, trousers and boots in one piece
        int kept = 0;
        for (const Rgba8& p : c.image.pixels()) kept += p == trousers ? 1 : 0;
        CHECK(kept == 40 * 30);
    }
    CHECK(cells[0].column == 0);
    CHECK(cells[1].column == 1);
}

TEST_CASE("a noisy background is still removed", "[gen]") {
    // Speckle of up to 9 per channel around a mid grey: the tolerance follows it.
    Image sheet(200, 100, Rgba8{128, 128, 128, 255});
    unsigned seed = 7;
    for (int y = 0; y < 100; ++y)
        for (int x = 0; x < 200; ++x) {
            seed = seed * 1664525u + 1013904223u;
            const auto v = static_cast<std::uint8_t>(119 + (seed >> 24) % 19);
            sheet.setPixel(x, y, Rgba8{v, v, v, 255});
        }
    const Rgba8 body{40, 120, 60, 255};
    sheet.fillRect({20, 20, 60, 60}, body);
    sheet.fillRect({120, 20, 60, 60}, body);
    const auto cells = gen::extractGrid(sheet, 2, 1);
    REQUIRE(cells.size() == 2);
    CHECK(cells[0].rect.w == 60);
    CHECK(cells[0].rect.h == 60);
}

TEST_CASE("pieces of one cell that touch are one subject", "[gen]") {
    // A belt the colour of the background cuts the figure in two; the halves
    // are 2 px apart, so they are one subject. (Two creatures 15 px apart in
    // one cell stay two: see the test above.)
    Image sheet(200, 100, Rgba8{250, 250, 250, 255});
    const Rgba8 body{40, 120, 60, 255};
    sheet.fillRect({20, 10, 60, 38}, body);
    sheet.fillRect({24, 50, 52, 40}, body);    // 2 px gap at rows 48 and 49
    sheet.fillRect({120, 20, 60, 60}, body);
    const auto cells = gen::extractGrid(sheet, 2, 1);
    REQUIRE(cells.size() == 2);
    CHECK(cells[0].rect.x == 20);
    CHECK(cells[0].rect.y == 10);
    CHECK(cells[0].rect.w == 60);
    CHECK(cells[0].rect.h == 80);
    CHECK(cells[0].image.pixel(30, 39).a == 0);    // the gap stays empty
    CHECK(cells[0].image.pixel(30, 45) == body);
}
