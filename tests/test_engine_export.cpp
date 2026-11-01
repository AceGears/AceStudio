// Engine export (Phase 7): Godot, Unity and Tiled text formats. The real
// engines load these in tests/engine (Godot headless; Unity on the test PC).
#include <catch2/catch_test_macros.hpp>

#include "acestudio/core/AssetFiles.h"
#include "acestudio/core/Document.h"
#include "acestudio/core/EngineExport.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

using namespace acestudio;
namespace fs = std::filesystem;

namespace {
Image tex(Rgba8 c) {
    Image img(16, 16);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) img.setPixel(x, y, {static_cast<std::uint8_t>(c.r + (x + y) % 4), c.g, c.b, 255});
    return img;
}
int count(const std::string& s, const std::string& what) {
    int n = 0;
    for (std::size_t p = s.find(what); p != std::string::npos; p = s.find(what, p + 1)) ++n;
    return n;
}
Document animated() {
    auto doc = Document::create(8, 8).value();
    const LayerId l = doc.addLayer("L").value();
    for (int f = 1; f < 4; ++f) (void)doc.insertFrame(f);
    for (int f = 0; f < 4; ++f) {
        Image img(8, 8);
        img.fillRect({f, 0, 2, 8}, {200, 50, 50, 255});
        (void)doc.setCel(l, f, Cel{img, 0, 0});
        (void)doc.setFrameDuration(f, f == 3 ? 200 : 100);
    }
    (void)doc.addTag({"run", 0, 3, PlaybackDirection::Forward, 0, "#fff"});
    (void)doc.addTag({"bounce", 1, 3, PlaybackDirection::PingPong, 2, "#fff"});
    return doc;
}
} // namespace

TEST_CASE("Godot TileSet: terrain sets, peering bits and sources for every scheme", "[engine]") {
    for (auto s : {AutotileScheme::Blob47, AutotileScheme::Wang16, AutotileScheme::Cardinal16}) {
        INFO(toString(s));
        std::vector<Tileset> levels{generateTileset(s, tex({40, 80, 200, 255}), tex({220, 200, 120, 255})).value(),
                                    generateTileset(s, Image(), tex({70, 150, 60, 255})).value()};
        const std::string t = godotTileSet(levels, {"a.png", "b.png"}, s == AutotileScheme::Wang16 ? "" : "base.png", {"water", {"sand", "grass"}});
        CHECK(t.rfind("[gd_resource type=\"TileSet\"", 0) == 0);
        const int mode = s == AutotileScheme::Blob47 ? 0 : s == AutotileScheme::Wang16 ? 1 : 2;
        CHECK(count(t, "terrain_set_0/mode = " + std::to_string(mode)) == 1);
        CHECK(count(t, "terrain_set_1/mode = " + std::to_string(mode)) == 1);
        CHECK(count(t, "/terrain_set = 0") >= tileCount(s));
        CHECK(count(t, "/terrain_set = 1") == tileCount(s));
        CHECK(t.find("\"grass\"") != std::string::npos);
        // Sides only where the scheme has them; corners likewise.
        CHECK((t.find("top_side") != std::string::npos) == (s != AutotileScheme::Wang16));
        CHECK((t.find("top_left_corner") != std::string::npos) == (s != AutotileScheme::Cardinal16));
        // Upper levels use -1 (empty) for the uncovered side.
        CHECK(t.find(" = -1") != std::string::npos);
        CHECK(godotTileSet(levels, {"a.png", "b.png"}, "base.png") == godotTileSet(levels, {"a.png", "b.png"}, "base.png"));
    }
}

TEST_CASE("Godot SpriteFrames: one animation per tag, durations from the frames", "[engine]") {
    const Document doc = animated();
    SheetOptions so;
    const auto sheet = buildSpriteSheet(doc, {}, so).value();
    const EngineSheet es = engineSheet(doc, sheet, "hero");
    REQUIRE(es.animations.size() == 2);
    CHECK(es.animations[0].name == "run");
    CHECK(es.animations[0].durationsMs == std::vector<int>{100, 100, 100, 200});
    CHECK(es.animations[1].frames == std::vector<int>{1, 2, 3, 2});   // ping-pong 1..3
    CHECK_FALSE(es.animations[1].loop);                                // repeat 2
    const std::string g = godotSpriteFrames(es, "hero.png");
    CHECK(count(g, "[sub_resource type=\"AtlasTexture\"") == 4);
    CHECK(g.find("\"speed\": 10.0") != std::string::npos);
    CHECK(g.find("\"duration\": 2.0") != std::string::npos);
    CHECK(g.find("&\"run\"") != std::string::npos);
}

TEST_CASE("Unity meta: bottom-left rects, borders, stable GUIDs and IDs", "[engine]") {
    EngineSheet s;
    s.width = 64;
    s.height = 32;
    s.frames.push_back({"button", {0, 0, 16, 10}, {2, 3, 4, 5}, 0.5, 0.5});
    s.frames.push_back({"icon 2", {16, 22, 10, 10}, {}, 0.5, 1.0});
    const std::string guid = unityGuid("acestudio:ui.png");
    CHECK(guid.size() == 32);
    CHECK(guid == unityGuid("acestudio:ui.png"));
    CHECK(guid != unityGuid("acestudio:other.png"));
    const std::string m = unityTextureMeta(s, guid, 16);
    CHECK(m.find("guid: " + guid) != std::string::npos);
    CHECK(m.find("spriteMode: 2") != std::string::npos);
    CHECK(m.find("filterMode: 0") != std::string::npos);
    CHECK(m.find("y: 22\n") != std::string::npos);   // 32 - (0 + 10)
    CHECK(m.find("y: 0\n") != std::string::npos);    // 32 - (22 + 10)
    CHECK(m.find("border: {x: 2, y: 5, z: 4, w: 3}") != std::string::npos);   // left, bottom, right, top
    CHECK(m.find("name: 'icon 2'") != std::string::npos);
    CHECK(m.find("pivot: {x: 0.5, y: 0}") != std::string::npos);
    CHECK(count(m, "internalID: " + std::to_string(unityFileId(guid + "/button"))) == 1);
}

TEST_CASE("Unity AnimationClip: sprite keys in order, loop flag, end time", "[engine]") {
    const Document doc = animated();
    const auto sheet = buildSpriteSheet(doc, {}, SheetOptions{}).value();
    const EngineSheet es = engineSheet(doc, sheet, "hero");
    const std::string guid = unityGuid("acestudio:hero.png");
    const std::string a = unityAnimationClip(es, 0, guid);
    CHECK(a.find("--- !u!74 &7400000") != std::string::npos);
    CHECK(a.find("m_Name: run") != std::string::npos);
    CHECK(a.find("m_SampleRate: 10") != std::string::npos);
    CHECK(a.find("m_StopTime: 0.5") != std::string::npos);   // 100+100+100+200 ms
    CHECK(a.find("m_LoopTime: 1") != std::string::npos);
    CHECK(count(a, "guid: " + guid + ", type: 3}") == 2 * 5);  // 4 keys + end key, twice (curve + mapping)
    CHECK(unityAnimationClip(es, 1, guid).find("m_LoopTime: 0") != std::string::npos);
    CHECK(unityAnimationClip(es, 9, guid).empty());
}

TEST_CASE("Tiled tileset: Wang set type and wang ids per scheme", "[engine]") {
    const auto blob = generateTileset(AutotileScheme::Blob47, tex({40, 80, 200, 255}), tex({220, 200, 120, 255})).value();
    const std::string x = tiledTileset(blob, "t.png", "t", "sand", "water");
    CHECK(x.find("type=\"mixed\"") != std::string::npos);
    CHECK(count(x, "<wangtile ") == 47);
    CHECK(x.find("wangid=\"1,1,1,1,1,1,1,1\"") != std::string::npos);   // full tile
    CHECK(x.find("<wangcolor name=\"water\"") != std::string::npos);
    const auto wang = generateTileset(AutotileScheme::Wang16, Image(), tex({70, 150, 60, 255})).value();
    const std::string w = tiledTileset(wang, "w.png", "w", "grass", "");
    CHECK(w.find("type=\"corner\"") != std::string::npos);
    CHECK(w.find("wangid=\"0,1,0,0,0,0,0,0\"") != std::string::npos);   // tile 2 = top-right corner
    CHECK(w.find("<wangcolor name=\"\"") == std::string::npos);
    const auto card = generateTileset(AutotileScheme::Cardinal16, tex({40, 80, 200, 255}), tex({220, 200, 120, 255})).value();
    CHECK(tiledTileset(card, "c.png", "c", "a", "b").find("type=\"edge\"") != std::string::npos);
}

TEST_CASE("Asset files: everything written, nothing overwritten without permission", "[engine]") {
    const fs::path dir = fs::temp_directory_path() / "acestudio_engine_files";
    fs::remove_all(dir);
    fs::create_directories(dir);
    AssetWriteOptions o;
    o.engines = parseEngineTargets("godot,unity,tiled").value();
    std::vector<Tileset> levels{generateTileset(AutotileScheme::Blob47, tex({40, 80, 200, 255}), tex({220, 200, 120, 255})).value()};
    auto w = writeTilesetFiles(levels, dir / "t.png", o);
    REQUIRE(w);
    for (const char* f : {"t.png", "t.json", "t_base.png", "t.tres", "t.tsx", "t.png.meta", "t_base.png.meta"}) CHECK(fs::exists(dir / f));
    // A second write is refused and changes nothing.
    const auto before = fs::last_write_time(dir / "t.png");
    CHECK_FALSE(writeTilesetFiles(levels, dir / "t.png", o));
    CHECK(fs::last_write_time(dir / "t.png") == before);
    o.overwrite = true;
    CHECK(writeTilesetFiles(levels, dir / "t.png", o));
    // Sheets and UI kits.
    auto s = writeSheetFiles(animated(), {}, SheetOptions{}, dir / "hero", o);
    REQUIRE(s);
    for (const char* f : {"hero.png", "hero.json", "hero.tres", "hero.png.meta", "hero_run.anim", "hero_run.anim.meta", "hero_bounce.anim"})
        CHECK(fs::exists(dir / f));
    auto u = writeUiKitFiles(generateUiKit({}), dir / "ui.png", o);
    REQUIRE(u);
    CHECK(fs::exists(dir / "ui_panel.tres"));
    CHECK_FALSE(fs::exists(dir / "ui_checkbox_on.tres"));   // fixed-size elements get no StyleBox
    CHECK_FALSE(parseEngineTargets("unreal"));
    CHECK(parseEngineTargets("all").value() == (kEngineGodot | kEngineUnity | kEngineTiled));
    fs::remove_all(dir);
}

TEST_CASE("Asset files: dry runs list targets, engine-only sheets", "[engine]") {
    const fs::path dir = fs::temp_directory_path() / "acestudio_engine_dry";
    fs::remove_all(dir);
    fs::create_directories(dir);
    AssetWriteOptions o;
    o.engines = kEngineGodot | kEngineUnity;
    o.dryRun = true;
    auto plan = writeSheetFiles(animated(), {}, SheetOptions{}, dir / "hero.png", o);
    REQUIRE(plan);
    CHECK(plan.value().files.size() >= 6);
    CHECK(plan.value().existing.empty());
    CHECK(fs::is_empty(dir));   // a dry run writes nothing

    // The app writes the sheet itself, then only the engine files.
    std::ofstream(dir / "hero.png") << "x";
    std::ofstream(dir / "hero.tres") << "x";
    o.engineFilesOnly = true;
    plan = writeSheetFiles(animated(), {}, SheetOptions{}, dir / "hero.png", o);
    REQUIRE(plan);
    CHECK(std::find(plan.value().files.begin(), plan.value().files.end(), dir / "hero.png") == plan.value().files.end());
    REQUIRE(plan.value().existing.size() == 1);
    CHECK(plan.value().existing[0] == dir / "hero.tres");
    o.dryRun = false;
    CHECK_FALSE(writeSheetFiles(animated(), {}, SheetOptions{}, dir / "hero.png", o));   // hero.tres exists
    o.overwrite = true;
    auto w = writeSheetFiles(animated(), {}, SheetOptions{}, dir / "hero.png", o);
    REQUIRE(w);
    CHECK_FALSE(fs::exists(dir / "hero.json"));
    CHECK(fs::exists(dir / "hero.png.meta"));
    {
        std::ifstream png(dir / "hero.png");   // closed before the folder is removed (Windows)
        std::string content((std::istreambuf_iterator<char>(png)), {});
        CHECK(content == "x");   // the sheet itself was left alone
    }
    fs::remove_all(dir);
}
