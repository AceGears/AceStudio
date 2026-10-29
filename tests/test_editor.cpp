#include <catch2/catch_test_macros.hpp>

#include "acestudio/core/Editor.h"
#include "acestudio/core/ProjectIO.h"

#include <functional>

using namespace acestudio;

namespace {

// Full observable state: metadata JSON plus every cel's pixels and position.
struct Snapshot {
    nlohmann::json meta;
    std::vector<std::tuple<LayerId, int, int, int, Image>> cels;
    bool operator==(const Snapshot&) const = default;
};

Snapshot snap(const Document& d) {
    Snapshot s{documentToJson(d), {}};
    for (const auto& [k, c] : d.cels()) s.cels.emplace_back(k.layer, k.frame, c.x, c.y, c.image);
    return s;
}

Editor makeEditor() {
    auto d = Document::create(8, 8).value();
    const auto a = d.addLayer("A").value();
    const auto b = d.addLayer("B").value();
    REQUIRE(d.insertFrame(1, 50));
    REQUIRE(d.setCel(a, 0, Cel{Image(8, 8, Rgba8::opaque(200, 0, 0)), 0, 0}));
    REQUIRE(d.setCel(b, 0, Cel{Image(2, 2, Rgba8{0, 0, 255, 128}), 3, 3}));
    REQUIRE(d.setCel(b, 1, Cel{Image(1, 1, Rgba8::opaque(0, 255, 0)), 1, 1}));
    REQUIRE(d.addTag({"loop", 0, 1, PlaybackDirection::Forward, 0, "#FFFFFF"}));
    return Editor(std::move(d));
}

// Applies `op`, then checks undo restores the exact prior state and redo
// restores the exact post state, twice over.
void checkRoundTrip(Editor& e, const std::function<void(Editor&)>& op) {
    const Snapshot before = snap(e.document());
    op(e);
    const Snapshot after = snap(e.document());
    REQUIRE_FALSE(before == after);
    REQUIRE(e.history().canUndo());
    for (int i = 0; i < 2; ++i) {
        REQUIRE(e.undo());
        CHECK(snap(e.document()) == before);
        REQUIRE(e.redo());
        CHECK(snap(e.document()) == after);
    }
    REQUIRE(e.document().validate());
}

LayerId idOf(const Editor& e, const std::string& name) {
    for (const auto& l : e.document().layers())
        if (l.name == name) return l.id;
    FAIL("no layer " << name);
    return kNoLayer;
}

} // namespace

TEST_CASE("Editor layer operations undo and redo exactly", "[editor][undo]") {
    Editor e = makeEditor();
    const LayerId a = idOf(e, "A"), b = idOf(e, "B");

    SECTION("add layer above") {
        checkRoundTrip(e, [&](Editor& ed) {
            auto id = ed.addLayer("C", LayerKind::Raster, a);
            REQUIRE(id);
            CHECK(ed.document().layers()[1].id == id.value());   // directly above A
        });
    }
    SECTION("add group at top") { checkRoundTrip(e, [](Editor& ed) { REQUIRE(ed.addLayer("G", LayerKind::Group)); }); }
    SECTION("remove layer with cels") { checkRoundTrip(e, [&](Editor& ed) { REQUIRE(ed.removeLayer(b)); }); }
    SECTION("move layer") { checkRoundTrip(e, [&](Editor& ed) { REQUIRE(ed.moveLayer(b, 0)); }); }
    SECTION("rename") { checkRoundTrip(e, [&](Editor& ed) { REQUIRE(ed.renameLayer(a, "Renamed")); }); }
    SECTION("visibility") { checkRoundTrip(e, [&](Editor& ed) { REQUIRE(ed.setLayerVisible(a, false)); }); }
    SECTION("lock") { checkRoundTrip(e, [&](Editor& ed) { REQUIRE(ed.setLayerLocked(a, true)); }); }
    SECTION("alpha lock") { checkRoundTrip(e, [&](Editor& ed) { REQUIRE(ed.setLayerAlphaLocked(a, true)); }); }
    SECTION("blend") { checkRoundTrip(e, [&](Editor& ed) { REQUIRE(ed.setLayerBlend(b, BlendMode::Screen)); }); }
    SECTION("duplicate") {
        checkRoundTrip(e, [&](Editor& ed) {
            auto id = ed.duplicateLayer(b);
            REQUIRE(id);
            CHECK(ed.document().cel(id.value(), 1) != nullptr);
            CHECK(ed.document().layer(id.value())->name == "B copy");
        });
    }
    SECTION("merge down") {
        checkRoundTrip(e, [&](Editor& ed) {
            const Image flatBefore = ed.document().composite(0);
            REQUIRE(ed.mergeDown(b));
            CHECK(ed.document().layers().size() == 1);
            CHECK(ed.document().composite(0) == flatBefore);   // merging must not change the picture
        });
    }
}

TEST_CASE("Editor frame and palette operations undo and redo exactly", "[editor][undo]") {
    Editor e = makeEditor();
    SECTION("insert frame") { checkRoundTrip(e, [](Editor& ed) { REQUIRE(ed.insertFrame(1, 80)); }); }
    SECTION("duplicate frame") { checkRoundTrip(e, [](Editor& ed) { REQUIRE(ed.duplicateFrame(0)); }); }
    SECTION("remove frame restores cels and tags") { checkRoundTrip(e, [](Editor& ed) { REQUIRE(ed.removeFrame(1)); }); }
    SECTION("frame duration") { checkRoundTrip(e, [](Editor& ed) { REQUIRE(ed.setFrameDuration(0, 333)); }); }
    SECTION("palette edit") { checkRoundTrip(e, [](Editor& ed) { REQUIRE(ed.setPaletteColor(2, Rgba8::opaque(1, 2, 3))); }); }
    SECTION("palette add") { checkRoundTrip(e, [](Editor& ed) { REQUIRE(ed.addPaletteColor(Rgba8::opaque(1, 2, 3))); }); }
    SECTION("palette remove") { checkRoundTrip(e, [](Editor& ed) { REQUIRE(ed.removePaletteColor(0)); }); }
}

TEST_CASE("Editor rejects invalid operations without recording history", "[editor]") {
    Editor e = makeEditor();
    const LayerId a = idOf(e, "A"), b = idOf(e, "B");
    const auto steps = e.history().size();
    CHECK_FALSE(e.removeLayer(999));
    CHECK_FALSE(e.renameLayer(a, ""));
    CHECK_FALSE(e.mergeDown(a));                         // nothing below
    CHECK_FALSE(e.removeFrame(5));
    CHECK_FALSE(e.setFrameDuration(0, 0));
    REQUIRE(e.setLayerVisible(b, false));
    CHECK_FALSE(e.mergeDown(b));                          // hidden layer
    CHECK(e.history().size() == steps + 1);
}

TEST_CASE("Opacity slider drags merge into one undo step", "[editor][undo]") {
    Editor e = makeEditor();
    const LayerId b = idOf(e, "B");
    for (int v = 250; v >= 100; v -= 10) REQUIRE(e.setLayerOpacity(b, static_cast<std::uint8_t>(v), true));
    CHECK(e.history().size() == 1);
    CHECK(e.document().layer(b)->opacity == 100);
    REQUIRE(e.undo());
    CHECK(e.document().layer(b)->opacity == 255);
    // Non-mergeable changes stay separate.
    REQUIRE(e.setLayerOpacity(b, 10));
    REQUIRE(e.setLayerOpacity(b, 20));
    CHECK(e.history().size() == 2);   // the redo branch was discarded by the first new push
}

TEST_CASE("Isolated group compositing applies the group blend mode once", "[document][compositing]") {
    auto d = Document::create(1, 1).value();
    const auto base = d.addLayer("base").value();
    const auto g = d.addLayer("g", LayerKind::Group).value();
    const auto c1 = d.addLayer("c1", LayerKind::Raster, g).value();
    const auto c2 = d.addLayer("c2", LayerKind::Raster, g).value();
    REQUIRE(d.setCel(base, 0, Cel{Image(1, 1, Rgba8::opaque(200, 200, 200)), 0, 0}));
    REQUIRE(d.setCel(c1, 0, Cel{Image(1, 1, Rgba8::opaque(100, 100, 100)), 0, 0}));
    REQUIRE(d.setCel(c2, 0, Cel{Image(1, 1, Rgba8::opaque(50, 50, 50)), 0, 0}));
    d.layer(g)->blend = BlendMode::Multiply;
    // Inside the group c2 (normal) covers c1, giving 50; the group then
    // multiplies onto 200: 200*50/255 = 39. Non-isolated blending would
    // have multiplied each child separately (200*100/255*50/255 = 15).
    const Rgba8 px = d.composite(0).pixel(0, 0);
    CHECK(px.r == 39);
}
