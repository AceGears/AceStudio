// Writes the engine-export fixture used by the real-engine checks
// (tests/engine/godot, and Unity on the test PC): an animated sprite sheet
// with tags, a 3-level tileset for each scheme, a UI kit and an atlas.
//
//   engine_fixture <out-folder> <base.png> <terrain1.png> <terrain2.png>

#include "acestudio/core/AssetFiles.h"
#include "acestudio/core/Document.h"
#include "acestudio/core/PngCodec.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <iostream>

using namespace acestudio;
namespace fs = std::filesystem;

static int fail(const Error& e) {
    std::cerr << e.describe() << "\n";
    return 1;
}

int main(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "usage: engine_fixture <out> <base.png> <terrain1.png> <terrain2.png>\n";
        return 1;
    }
    const fs::path out = argv[1];
    fs::create_directories(out);
    AssetWriteOptions all;
    all.engines = kEngineGodot | kEngineUnity | kEngineUnityRuleTiles | kEngineTiled;
    all.overwrite = true;

    // A 24x32 hero with 6 frames: a walk (0-3, forward), an idle (4-5,
    // ping-pong, 3 frames after expansion is 4-5-4... here 2 frames) and
    // uneven durations.
    auto doc = Document::create(24, 32).value();
    const LayerId layer = doc.addLayer("Body").value();
    for (int f = 1; f < 6; ++f) (void)doc.insertFrame(f);
    for (int f = 0; f < 6; ++f) {
        Image img(24, 32);
        img.fillRect({6, 4 + (f % 2), 12, 24}, {static_cast<std::uint8_t>(60 + f * 30), 90, 200, 255});
        img.fillRect({8 + f, 28, 4, 4}, {30, 30, 40, 255});
        (void)doc.setCel(layer, f, Cel{img, 0, 0});
        (void)doc.setFrameDuration(f, f < 4 ? 100 : (f == 4 ? 300 : 150));
    }
    (void)doc.addTag({"walk", 0, 3, PlaybackDirection::Forward, 0, "#4F8CFF"});
    (void)doc.addTag({"idle", 4, 5, PlaybackDirection::PingPong, 0, "#FF8C4F"});
    (void)doc.addTag({"die", 2, 5, PlaybackDirection::Forward, 1, "#FF4F4F"});
    SheetOptions so;
    so.layout = SheetLayout::Grid;
    so.trim = true;
    if (auto w = writeSheetFiles(doc, {}, so, out / "hero.png", all); !w) return fail(w.error());

    auto base = readPng(argv[2]);
    auto t1 = readPng(argv[3]);
    auto t2 = readPng(argv[4]);
    if (!base || !t1 || !t2) {
        std::cerr << "could not read the textures\n";
        return 1;
    }
    auto crop = [](const Image& i) { return i.copy({0, 0, 16, 16}); };
    for (auto s : {AutotileScheme::Blob47, AutotileScheme::Wang16, AutotileScheme::Cardinal16}) {
        std::vector<Tileset> levels{generateTileset(s, crop(base.value()), crop(t1.value())).value(),
                                    generateTileset(s, Image(), crop(t2.value())).value()};
        GodotTerrainNames names{"water", {"sand", "grass"}};
        if (auto w = writeTilesetFiles(levels, out / (std::string(toString(s)) + ".png"), all, names); !w) return fail(w.error());
    }
    // A level map and, per scheme, the tiles AceStudio places for it: the
    // Godot check paints the same map with Godot's terrain tools and
    // compares (Blob47 and Cardinal16 place per cell like Godot; Wang16 is a
    // dual grid in AceStudio, so Godot's own corner placement is only checked
    // for completeness).
    {
        LevelMap m(14, 10);
        for (int y = 1; y < 9; ++y)
            for (int x = 1; x < 13; ++x) m.set(x, y, 1);
        m.paint(4, 4, 2, 1);
        m.paint(9, 5, 2, 0);
        m.paint(11, 2, 2, 0);
        m.set(6, 7, 0);
        m.limitSteps();
        nlohmann::json j;
        j["w"] = m.width();
        j["h"] = m.height();
        nlohmann::json cells = nlohmann::json::array();
        for (int y = 0; y < m.height(); ++y)
            for (int x = 0; x < m.width(); ++x) cells.push_back(m.at(x, y));
        j["levels"] = cells;
        for (auto s : {AutotileScheme::Blob47, AutotileScheme::Cardinal16}) {
            const int cols = tilesetGrid(s).first;
            nlohmann::json per = nlohmann::json::array();
            for (int level = 1; level <= 2; ++level) {
                const TilePlacement p = autotile(m.atLeast(level), s);
                nlohmann::json grid = nlohmann::json::array();
                for (int y = 0; y < m.height(); ++y)
                    for (int x = 0; x < m.width(); ++x) {
                        const int k = p.at(x, y);
                        if (k >= 0) grid.push_back({level, k % cols, k / cols});           // source = level
                        else if (level == 1) grid.push_back({0, 0, 0});                    // base tile
                        else grid.push_back({-1, -1, -1});                                  // empty
                    }
                per.push_back(grid);
            }
            j["expected"][std::string(toString(s))] = per;
        }
        // Wang16: (w+1) x (h+1) corner tiles on the dual grid, tile = corner mask.
        {
            nlohmann::json per = nlohmann::json::array();
            for (int level = 1; level <= 2; ++level) {
                const TilePlacement p = autotile(m.atLeast(level), AutotileScheme::Wang16);
                nlohmann::json grid = nlohmann::json::array();
                for (int y = 0; y < p.rows; ++y)
                    for (int x = 0; x < p.columns; ++x) grid.push_back(p.at(x, y));
                per.push_back(grid);
            }
            j["expectedWang16"] = per;
        }
        if (auto st = writeTextFile(out / "map.json", j.dump()); !st) return fail(st.error());
    }
    if (auto w = writeUiKitFiles(generateUiKit({}), out / "ui.png", all); !w) return fail(w.error());
    std::vector<AtlasItem> items;
    for (int i = 0; i < 5; ++i) {
        Image img(8 + i * 4, 12);
        img.fill({static_cast<std::uint8_t>(i * 50), 120, 80, 255});
        items.push_back({"item_" + std::to_string(i), img});
    }
    if (auto w = writeAtlasFiles(packAtlas(items).value(), out / "items.png", all); !w) return fail(w.error());
    std::cout << "ok\n";
    return 0;
}
