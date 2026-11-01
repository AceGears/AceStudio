// Regression suite: projects written by older AceStudio versions, and golden
// renders of the compositor, adjustments, onion skin and sprite-sheet export.
//
// tests/data/compat: projects and per-frame renders written by 0.1.0, 0.5.0
// and 0.8.1 themselves (tests/compat/make_fixture.cpp built against those
// commits). The current build must open them and produce identical pixels.
//
// tests/data/golden: renders by the current build. A change in any of them is
// a change in what users see; if it is intended, regenerate with
//   ACESTUDIO_UPDATE_GOLDEN=1 acestudio_core_tests "[golden]"
// and say why in the commit.
#include "TestSupport.h"

#include "acestudio/core/Animation.h"
#include "acestudio/core/Editor.h"
#include "acestudio/core/Export.h"
#include "acestudio/core/Version.h"
#include "acestudio/core/FileUtil.h"
#include "acestudio/core/PngCodec.h"
#include "acestudio/core/ProjectIO.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <string>

using namespace acestudio;
namespace fs = std::filesystem;

namespace {

fs::path dataDir(const char* sub) { return fs::path(ACESTUDIO_SOURCE_DIR) / "tests" / "data" / sub; }

// ACESTUDIO_UPDATE_GOLDEN=1 rewrites the golden files instead of comparing.
bool updateGolden() {
#ifdef _MSC_VER
    char* v = nullptr;
    std::size_t n = 0;
    const bool on = _dupenv_s(&v, &n, "ACESTUDIO_UPDATE_GOLDEN") == 0 && v && std::string(v) == "1";
    std::free(v);
    return on;
#else
    const char* v = std::getenv("ACESTUDIO_UPDATE_GOLDEN");
    return v && std::string(v) == "1";
#endif
}

// Number of differing pixels (or -1 when the sizes differ). Pixels where
// `skip` is opaque are not compared.
long pixelDiff(const Image& a, const Image& b, const Image* skip = nullptr) {
    if (a.width() != b.width() || a.height() != b.height()) return -1;
    long n = 0;
    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width(); ++x) {
            if (skip && skip->pixel(x, y).a) continue;
            const Rgba8 p = a.pixel(x, y), q = b.pixel(x, y);
            // Fully transparent pixels compare equal whatever their colour.
            if (p.a == 0 && q.a == 0) continue;
            if (p.r != q.r || p.g != q.g || p.b != q.b || p.a != q.a) ++n;
        }
    return n;
}

void checkGolden(const Image& img, const std::string& name) {
    const fs::path path = dataDir("golden") / (name + ".png");
    if (updateGolden()) {
        REQUIRE(writePng(img, path));
        WARN("updated golden " << path.string());
        return;
    }
    auto golden = readPng(path);
    INFO("golden " << path.string());
    REQUIRE(golden);
    CHECK(pixelDiff(img, golden.value()) == 0);
}

Image pattern(int w, int h, int seed, std::uint8_t alpha) {
    Image img(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if ((x * 3 + y * 5 + seed) % 7 == 0) continue;
            img.setPixel(x, y, Rgba8{static_cast<std::uint8_t>((x * 16 + seed * 40) & 255),
                                     static_cast<std::uint8_t>((y * 16 + seed * 70) & 255),
                                     static_cast<std::uint8_t>((x * y + seed * 30) & 255), alpha});
        }
    return img;
}

// A gradient backdrop with varying alpha, so every blend path is exercised.
Image backdrop(int w, int h) {
    Image img(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            img.setPixel(x, y, Rgba8{static_cast<std::uint8_t>(x * 255 / (w - 1)), static_cast<std::uint8_t>(y * 255 / (h - 1)),
                                     static_cast<std::uint8_t>(128 + (x - y) / 2), static_cast<std::uint8_t>(y < h / 2 ? 255 : 128 + x)});
    return img;
}

} // namespace

TEST_CASE("Projects written by older versions open and render identically", "[regression][compat]") {
    struct Era { const char* name; int schema; const char* writer; std::size_t layers; };
    for (const Era era : {Era{"era1", 1, "0.1.0", 12}, Era{"era2", 2, "0.5.0", 16}, Era{"era3", 3, "0.8.1", 19}}) {
        INFO(era.name);
        ProjectInfo info;
        auto doc = loadProject(dataDir("compat") / (std::string(era.name) + ".acestudio"), &info);
        REQUIRE(doc);
        CHECK(info.schemaVersion == era.schema);
        CHECK(info.writerVersion == era.writer);
        const Document& d = doc.value();
        CHECK(d.layers().size() == era.layers);
        REQUIRE(d.frameCount() == 3);
        CHECK(d.frames()[0].durationMs == 120);
        CHECK(d.frames()[1].durationMs == 150);
        REQUIRE(d.tags().size() == 1);
        CHECK(d.tags()[0].name == "walk");
        CHECK(d.tags()[0].direction == PlaybackDirection::PingPong);
        CHECK(d.tags()[0].repeat == 2);
        for (int f = 0; f < 3; ++f) {
            INFO("frame " << f);
            auto expected = readPng(dataDir("compat") / (std::string(era.name) + "_frame" + std::to_string(f) + ".png"));
            REQUIRE(expected);
            if (era.schema == 1) {
                // 0.2.0 changed group compositing on purpose (CHANGELOG: groups
                // composite in isolation, W3C semantics, instead of each child
                // blending onto the backdrop with multiplied opacity). Pixels
                // under group content may differ; everything else must not.
                Image groupArea(d.width(), d.height());
                for (const Layer& l : d.layers())
                    if (l.parent != kNoLayer)
                        if (const Cel* c = d.cel(l.id, f))
                            for (int y = 0; y < c->image.height(); ++y)
                                for (int x = 0; x < c->image.width(); ++x)
                                    if (c->image.pixel(x, y).a) groupArea.setPixel(c->x + x, c->y + y, Rgba8{0, 0, 0, 255});
                CHECK(pixelDiff(d.composite(f), expected.value(), &groupArea) == 0);
                CHECK(pixelDiff(d.composite(f), expected.value()) > 0);   // the change is real and confined
            } else {
                CHECK(pixelDiff(d.composite(f), expected.value()) == 0);
            }
        }
        // Saving with this version and loading again keeps the pixels.
        auto bytes = serializeProject(d);
        REQUIRE(bytes);
        auto again = deserializeProject(bytes.value());
        REQUIRE(again);
        for (int f = 0; f < 3; ++f) CHECK(pixelDiff(again.value().composite(f), d.composite(f)) == 0);
        if (era.schema >= 2) CHECK(d.tileMode() == TileMode::Horizontal);
        if (era.schema >= 3) {
            CHECK(d.palette().size() >= 2);
            const LayerId linked = d.layers().back().id;
            CHECK(d.celLink(linked, 0) != 0);
            CHECK(d.celLink(linked, 0) == d.celLink(linked, 2));
        }
    }
}

TEST_CASE("Golden: every blend mode at several opacities", "[regression][golden]") {
    const BlendMode modes[] = {BlendMode::Normal, BlendMode::Multiply, BlendMode::Screen, BlendMode::Overlay, BlendMode::Darken,
                               BlendMode::Lighten, BlendMode::Add, BlendMode::Subtract, BlendMode::Difference};
    const int cell = 16;
    auto d = Document::create(cell * 9, cell * 3).value();
    const LayerId bg = d.addLayer("bg").value();
    REQUIRE(d.setCel(bg, 0, Cel{backdrop(cell * 9, cell * 3), 0, 0}));
    int col = 0;
    for (BlendMode m : modes) {
        int row = 0;
        for (std::uint8_t opacity : {std::uint8_t{255}, std::uint8_t{160}, std::uint8_t{60}}) {
            const LayerId l = d.addLayer(std::string(toString(m))).value();
            d.layer(l)->blend = m;
            d.layer(l)->opacity = opacity;
            REQUIRE(d.setCel(l, 0, Cel{pattern(cell, cell, col * 3 + row, row == 2 ? 200 : 255), col * cell, row * cell}));
            ++row;
        }
        ++col;
    }
    checkGolden(d.composite(0), "blend_modes");
}

TEST_CASE("Golden: adjustment layers, masks and clipping", "[regression][golden]") {
    struct Adj { Adjustment adj; std::uint8_t opacity; };
    const Adj adjs[] = {
        {Adjustment{AdjustmentType::BrightnessContrast, 30, 40, 0}, 255},
        {Adjustment{AdjustmentType::HueSaturation, 90, 50, -10}, 255},
        {Adjustment{AdjustmentType::Invert, 0, 0, 0}, 128},
        {Adjustment{AdjustmentType::Posterize, 4, 0, 0}, 255},
        {Adjustment{AdjustmentType::Threshold, 128, 0, 0}, 200},
        {Adjustment{AdjustmentType::Levels, 20, 220, 150}, 255},
        {Adjustment(AdjustmentType::Curves, {{0, 30}, {128, 90}, {255, 240}}), 255},
    };
    const int cell = 16;
    Image sheet(cell * 7, cell * 2);
    int col = 0;
    for (const Adj& a : adjs) {
        for (int row = 0; row < 2; ++row) {
            auto d = Document::create(cell, cell).value();
            const LayerId base = d.addLayer("base").value();
            REQUIRE(d.setCel(base, 0, Cel{backdrop(cell, cell), 0, 0}));
            const LayerId adj = d.addLayer("adj", LayerKind::Adjustment).value();
            d.layer(adj)->adjustment = a.adj;
            d.layer(adj)->opacity = a.opacity;
            if (row == 1) {   // second row: a gradient mask and a clipped sprite on top
                d.layer(adj)->hasMask = true;
                Image mask(cell, cell);
                for (int y = 0; y < cell; ++y)
                    for (int x = 0; x < cell; ++x) {
                        const auto v = static_cast<std::uint8_t>(x * 17);
                        mask.setPixel(x, y, Rgba8{v, v, v, 255});
                    }
                REQUIRE(d.setMaskCel(adj, 0, Cel{mask, 0, 0}));
                const LayerId clipped = d.addLayer("clipped").value();
                d.layer(clipped)->clipped = true;
                REQUIRE(d.setCel(clipped, 0, Cel{pattern(8, 8, col, 255), 4, 4}));
            }
            sheet.blit(d.composite(0), col * cell, row * cell);
        }
        ++col;
    }
    checkGolden(sheet, "adjustments");
}

TEST_CASE("Golden: onion skin and packed sprite sheet", "[regression][golden]") {
    auto doc = Document::create(24, 24).value();
    const LayerId l = doc.addLayer("sprite").value();
    Editor e(std::move(doc));
    for (int f = 1; f < 6; ++f) REQUIRE(e.duplicateFrame(0));
    for (int f = 0; f < 6; ++f) {
        Image img(24, 24);
        img.fillRect({2 + f * 3, 4 + (f % 3) * 2, 6 + f, 8}, Rgba8{static_cast<std::uint8_t>(40 * f), 200, 120, 255});
        if (f == 4) img = Image(24, 24);   // an empty frame
        if (f == 5) img.fillRect({2, 4, 6, 8}, Rgba8{0, 200, 120, 255});   // equal to frame 0 after the fill below
        REQUIRE(e.setCelImage(l, f, img));
    }
    {
        Image f0(24, 24);
        f0.fillRect({2, 4, 6, 8}, Rgba8{0, 200, 120, 255});
        REQUIRE(e.setCelImage(l, 0, f0));
        REQUIRE(e.setCelImage(l, 5, f0));
    }
    OnionSkin o;
    o.before = 2;
    o.after = 1;
    o.opacity = 60;
    checkGolden(renderOnionSkin(e.document(), 2, o), "onion_skin");

    SheetOptions packed;
    packed.layout = SheetLayout::Packed;
    packed.trim = true;
    packed.mergeDuplicates = true;
    packed.spacing = 1;
    packed.scale = 2;
    auto sheet = buildSpriteSheet(e.document(), FrameRange{}, packed);
    REQUIRE(sheet);
    checkGolden(sheet.value().image, "sheet_packed");
    // The frame rectangles are part of the contract with game engines.
    // The app version in meta changes with every release; the golden holds a fixed one.
    std::string json = spriteSheetJson(e.document(), sheet.value(), "sheet.png", "sprite", JsonStyle::Array);
    const std::string current = std::string("\"version\": \"") + kVersionString + "\"";
    if (const auto at = json.find(current); at != std::string::npos) json.replace(at, current.size(), "\"version\": \"0.9.0\"");
    const fs::path jsonPath = dataDir("golden") / "sheet_packed.json";
    if (updateGolden()) {
        REQUIRE(writeFileAtomic(jsonPath, std::vector<std::uint8_t>(json.begin(), json.end())));
    } else {
        auto expected = readFileBytes(jsonPath, 1u << 20);
        REQUIRE(expected);
        CHECK(json == std::string(expected.value().begin(), expected.value().end()));
    }
}
