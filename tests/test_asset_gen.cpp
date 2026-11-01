#include "acestudio/core/AssetGen.h"

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
