#include "TestSupport.h"

#include "acestudio/core/FileUtil.h"
#include "acestudio/core/WorldMap.h"

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <sstream>

using namespace acestudio;
using namespace acestudio::world;

namespace {
const Rgba8 kGrass{60, 160, 60, 255}, kStone{120, 120, 130, 255};

WorldMap sample() {
    auto m = WorldMap::create(10, 6, 8, 8);
    REQUIRE(m);
    WorldMap map = std::move(m).value();
    Image sheet(16, 8);
    sheet.fillRect({0, 0, 8, 8}, kGrass);
    sheet.fillRect({8, 0, 8, 8}, kStone);
    REQUIRE(map.addSheet({"ground", sheet, 8, 8}));
    map.addLayer("Ground");
    map.addLayer("Detail");
    Image tree(6, 12, Rgba8{30, 90, 30, 255});
    map.propDefs.push_back({"tree", tree});
    return map;
}
} // namespace

TEST_CASE("painting, filling and rendering tiles", "[world]") {
    WorldMap map = sample();
    map.fillRect(0, {0, 4, 10, 2}, makeCell(0, 1));     // stone floor
    map.floodFill(0, 0, 0, makeCell(0, 0));              // grass everywhere else
    CHECK(map.cell(0, 5, 5) == makeCell(0, 1));
    CHECK(map.cell(0, 5, 0) == makeCell(0, 0));
    CHECK(map.cell(1, 5, 0) == kEmpty);
    CHECK(map.cell(0, 99, 0) == kEmpty);

    map.props.push_back({0, 20, 4, true});
    const Image img = render(map);
    REQUIRE(img.width() == 80);
    REQUIRE(img.height() == 48);
    CHECK(img.pixel(0, 0) == kGrass);
    CHECK(img.pixel(4, 44) == kStone);
    CHECK(img.pixel(22, 10) == (Rgba8{30, 90, 30, 255}));   // the prop is drawn over the ground

    RenderOptions o;
    o.collision = true;
    map.setSolid(0, 0, true);
    CHECK(render(map, o).pixel(1, 1) != kGrass);
}

TEST_CASE("collision cells merge into rectangles", "[world]") {
    WorldMap map = sample();
    map.fillRect(0, {0, 4, 10, 2}, makeCell(0, 1));
    map.solidFromLayer(0);
    map.setSolid(3, 1, true);
    const auto rects = collisionRects(map);
    REQUIRE(rects.size() == 2);
    CHECK(rects[0] == IntRect{3, 1, 1, 1});
    CHECK(rects[1] == IntRect{0, 4, 10, 2});
}

TEST_CASE("parallax backgrounds move slower than the map", "[world]") {
    WorldMap map = sample();
    Image bg(20, 48);
    for (int x = 0; x < 20; ++x) bg.fillRect({x, 0, 1, 48}, Rgba8{static_cast<std::uint8_t>(x * 10), 0, 0, 255});
    map.backgrounds.push_back({"sky", bg, 0.5, 0});
    RenderOptions o;
    o.viewWidth = 40;
    o.cameraX = 0;
    const Image a = render(map, o);
    o.cameraX = 8;
    const Image b = render(map, o);
    CHECK(a.width() == 40);
    // Camera moved 8 px; the background moved 4 px.
    CHECK(b.pixel(0, 10) == a.pixel(4, 10));
}

TEST_CASE("lighting brightens near a lamp", "[world]") {
    WorldMap map = sample();
    map.fillRect(0, {0, 0, 10, 6}, makeCell(0, 1));
    map.lights.push_back({8, 8, 20, Rgba8{255, 255, 255, 255}});
    RenderOptions o;
    o.lighting = true;
    const Image lit = render(map, o);
    CHECK(lit.pixel(8, 8).r > lit.pixel(70, 40).r);
    CHECK(lit.pixel(70, 40).r < kStone.r);   // ambient darkens the far corner
}

TEST_CASE("maps save, load and export to Tiled", "[world]") {
    TempDir dir;
    WorldMap map = sample();
    map.fillRect(0, {0, 4, 10, 2}, makeCell(0, 1));
    map.setCell(1, 2, 3, makeCell(0, 0));
    map.solidFromLayer(0);
    map.props.push_back({0, 20, 4, true});
    map.lights.push_back({10, 10, 30, Rgba8{255, 200, 100, 255}});
    map.layers[1].visible = false;
    const auto file = dir / "level.acemap";
    REQUIRE(save(map, file));
    auto back = load(file);
    REQUIRE(back);
    const WorldMap& m = back.value();
    CHECK(m.width() == 10);
    CHECK(m.layers.size() == 2);
    CHECK(m.cell(0, 9, 5) == makeCell(0, 1));
    CHECK(m.cell(1, 2, 3) == makeCell(0, 0));
    CHECK_FALSE(m.layers[1].visible);
    CHECK(m.solid(0, 4));
    CHECK_FALSE(m.solid(0, 0));
    REQUIRE(m.props.size() == 1);
    CHECK(m.props[0].solid);
    CHECK(m.lights.size() == 1);
    CHECK(render(m) == render(map));

    auto files = exportTiled(map, dir / "level.tmx");
    REQUIRE(files);
    std::ifstream in(dir / "level.tmx");
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string tmx = ss.str();
    CHECK(tmx.find("<map version=\"1.10\"") != std::string::npos);
    CHECK(tmx.find("nextlayerid=\"5\"") != std::string::npos);
    CHECK(tmx.find("firstgid=\"3\" name=\"props\"") != std::string::npos);
    CHECK(tmx.find("<object id=\"1\" name=\"tree\" gid=\"3\" x=\"20\" y=\"16\"") != std::string::npos);
    CHECK(tmx.find("type=\"solid\" x=\"0\" y=\"32\" width=\"80\" height=\"16\"") != std::string::npos);
    CHECK(std::filesystem::exists(dir / "level_preview.png"));
}

TEST_CASE("damaged or unsafe map files are refused", "[world]") {
    TempDir dir;
    const auto file = dir / "bad.acemap";
    {
        std::ofstream o(file);
        o << R"({"format":"acestudio.map","version":1,"width":4,"height":4,"tileWidth":8,"tileHeight":8,
                 "sheets":[{"name":"x","image":"../../etc/passwd.png","tileWidth":8,"tileHeight":8}]})";
    }
    CHECK_FALSE(load(file));
    {
        std::ofstream o(file);
        o << R"({"format":"acestudio.map","version":1,"width":4,"height":4,"tileWidth":8,"tileHeight":8,
                 "layers":[{"name":"a","cells":[1,2,3]}]})";
    }
    CHECK_FALSE(load(file));
    CHECK_FALSE(WorldMap::create(0, 4, 8, 8));
    CHECK_FALSE(WorldMap::create(5000, 4, 8, 8));
}
