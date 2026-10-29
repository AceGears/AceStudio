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

TEST_CASE("Canvas operations undo and redo exactly", "[editor][undo][canvas]") {
    Editor e = makeEditor();
    SECTION("resize larger with offset") {
        checkRoundTrip(e, [](Editor& ed) {
            REQUIRE(ed.resizeCanvas(12, 10, 2, 1));
            CHECK(ed.document().width() == 12);
            CHECK(ed.document().composite(0).pixel(2, 1) == Rgba8::opaque(200, 0, 0));
        });
    }
    SECTION("crop") {
        checkRoundTrip(e, [](Editor& ed) {
            REQUIRE(ed.cropCanvas({3, 3, 2, 2}));
            CHECK(ed.document().width() == 2);
        });
    }
    SECTION("flip horizontal") {
        checkRoundTrip(e, [](Editor& ed) {
            const Image before = ed.document().composite(1);
            REQUIRE(ed.flipCanvas(true));
            CHECK(ed.document().composite(1).pixel(6, 1) == before.pixel(1, 1));
        });
    }
    SECTION("flip vertical") { checkRoundTrip(e, [](Editor& ed) { REQUIRE(ed.flipCanvas(false)); }); }
    SECTION("rotate 90 on a non-square canvas") {
        REQUIRE(e.resizeCanvas(8, 5, 0, 0));
        checkRoundTrip(e, [](Editor& ed) {
            const Image before = ed.document().composite(1);
            REQUIRE(ed.rotateCanvas90(true));
            CHECK(ed.document().width() == 5);
            CHECK(ed.document().height() == 8);
            // (x, y) -> (H-1-y, x)
            CHECK(ed.document().composite(1).pixel(5 - 1 - 1, 1) == before.pixel(1, 1));
        });
    }
    SECTION("rotate 180") { checkRoundTrip(e, [](Editor& ed) { REQUIRE(ed.rotateCanvas180()); }); }
    CHECK_FALSE(e.resizeCanvas(0, 5, 0, 0));
    CHECK_FALSE(e.cropCanvas({100, 100, 5, 5}));
}

TEST_CASE("Layer transforms honour selection and frames", "[editor][undo][transform]") {
    Editor e = makeEditor();
    const LayerId b = idOf(e, "B");
    SECTION("flip whole layer on all frames") {
        checkRoundTrip(e, [&](Editor& ed) {
            REQUIRE(ed.transformLayer(b, 0, true, nullptr, "Flip", [](const Image& i) { return flipHorizontal(i); }));
            CHECK(ed.document().cel(b, 1)->x == 6);   // 1px cel at x=1 mirrored to x=6 on an 8px canvas
        });
    }
    SECTION("outline inside a selection only") {
        Mask sel(8, 8);
        sel.fillRect({2, 2, 4, 4}, true);
        checkRoundTrip(e, [&](Editor& ed) {
            REQUIRE(ed.transformLayer(b, 0, false, &sel, "Outline", [](const Image& i) {
                return outline(i, Rgba8::opaque(1, 1, 1), OutlineMode::Outside, false);
            }));
            CHECK(ed.document().renderLayer(b, 0).pixel(3, 2) == Rgba8::opaque(1, 1, 1));
            CHECK(ed.document().renderLayer(b, 1).pixel(1, 1) == Rgba8::opaque(0, 255, 0));   // other frame untouched
        });
    }
    SECTION("scale up re-centres on the selection") {
        checkRoundTrip(e, [&](Editor& ed) {
            REQUIRE(ed.transformLayer(b, 1, false, nullptr, "Scale", [](const Image& i) {
                return scaleNearest(i, i.width() * 2, i.height() * 2).value();
            }));
        });
    }
    REQUIRE(e.setLayerLocked(b, true));
    CHECK_FALSE(e.transformLayer(b, 0, false, nullptr, "x", [](const Image& i) { return i; }));
}
