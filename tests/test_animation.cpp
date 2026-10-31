#include <catch2/catch_test_macros.hpp>

#include "acestudio/core/Animation.h"
#include "acestudio/core/Editor.h"
#include "acestudio/core/Paint.h"
#include "acestudio/core/ProjectIO.h"

#include <map>

using namespace acestudio;

namespace {
Editor fourFrames() {
    auto d = Document::create(4, 1).value();
    const auto l = d.addLayer("L").value();
    for (int f = 1; f < 4; ++f) REQUIRE(d.insertFrame(f, 100 + f * 10));
    for (int f = 0; f < 4; ++f) REQUIRE(d.setCel(l, f, Cel{Image(1, 1, Rgba8::opaque(static_cast<std::uint8_t>(f * 50), 0, 0)), f, 0}));
    return Editor(std::move(d));
}
int marker(const Document& d, int frame) {   // which original frame is now at `frame`
    return d.composite(frame).pixel(0, 0).a ? 0
         : d.composite(frame).pixel(1, 0).a ? 1
         : d.composite(frame).pixel(2, 0).a ? 2 : 3;
}
} // namespace

TEST_CASE("Tag cycles for each direction", "[animation]") {
    CHECK(tagCycle({"a", 1, 3, PlaybackDirection::Forward, 0, ""}) == std::vector<int>{1, 2, 3});
    CHECK(tagCycle({"a", 1, 3, PlaybackDirection::Reverse, 0, ""}) == std::vector<int>{3, 2, 1});
    CHECK(tagCycle({"a", 0, 3, PlaybackDirection::PingPong, 0, ""}) == std::vector<int>{0, 1, 2, 3, 2, 1});
    CHECK(tagCycle({"a", 2, 2, PlaybackDirection::PingPong, 0, ""}) == std::vector<int>{2});
    CHECK(tagSequence({"a", 0, 1, PlaybackDirection::Forward, 2, ""}) == std::vector<int>{0, 1, 0, 1});
    CHECK(tagSequence({"a", 0, 2, PlaybackDirection::PingPong, 1, ""}) == std::vector<int>{0, 1, 2, 1, 0});
}

TEST_CASE("AnimationPlayer loops tags and stops finite repeats", "[animation]") {
    auto d = Document::create(1, 1).value();
    for (int f = 1; f < 6; ++f) REQUIRE(d.insertFrame(f));
    REQUIRE(d.addTag({"walk", 1, 3, PlaybackDirection::Forward, 0, ""}));
    REQUIRE(d.addTag({"once", 4, 5, PlaybackDirection::Forward, 1, ""}));

    AnimationPlayer p;
    p.reset(d, 2);
    CHECK(p.currentFrame() == 2);
    std::vector<int> seen;
    for (int i = 0; i < 5; ++i) { REQUIRE(p.advance()); seen.push_back(p.currentFrame()); }
    CHECK(seen == std::vector<int>{3, 1, 2, 3, 1});

    p.reset(d, 4);
    CHECK(p.advance());
    CHECK(p.currentFrame() == 5);
    CHECK_FALSE(p.advance());
    CHECK(p.finished());

    p.reset(d, 0);   // untagged frame: plays everything
    CHECK(p.sequence().size() == 6);
    p.resetAll(d, 2);   // tags ignored
    CHECK(p.currentFrame() == 2);
    for (int i = 0; i < 4; ++i) REQUIRE(p.advance());
    CHECK(p.currentFrame() == 0);
}

TEST_CASE("Moving frames carries cels and undoes exactly", "[animation][editor]") {
    Editor e = fourFrames();
    const auto before = documentToJson(e.document());
    REQUIRE(e.moveFrame(0, 3));
    CHECK(marker(e.document(), 3) == 0);
    CHECK(marker(e.document(), 0) == 1);
    CHECK(e.document().frames()[3].durationMs == 100);   // duration travels with the frame
    REQUIRE(e.undo());
    CHECK(documentToJson(e.document()) == before);
    for (int f = 0; f < 4; ++f) CHECK(marker(e.document(), f) == f);
    REQUIRE(e.moveFrame(3, 1));
    CHECK(marker(e.document(), 1) == 3);
    CHECK(marker(e.document(), 2) == 1);
    CHECK_FALSE(e.moveFrame(0, 9));
}

TEST_CASE("Tag editing is validated and undoable", "[animation][editor]") {
    Editor e = fourFrames();
    auto idx = e.addTag({"idle", 0, 1, PlaybackDirection::PingPong, 0, "#FF0000"});
    REQUIRE(idx);
    CHECK(e.document().tags().size() == 1);
    REQUIRE(e.updateTag(idx.value(), {"idle", 0, 3, PlaybackDirection::Reverse, 2, "#00FF00"}));
    CHECK(e.document().tags()[0].toFrame == 3);
    CHECK_FALSE(e.updateTag(idx.value(), {"", 0, 1, PlaybackDirection::Forward, 0, ""}));
    CHECK_FALSE(e.updateTag(idx.value(), {"x", 2, 9, PlaybackDirection::Forward, 0, ""}));
    CHECK_FALSE(e.addTag({"x", 3, 1, PlaybackDirection::Forward, 0, ""}));
    REQUIRE(e.undo());
    CHECK(e.document().tags()[0].toFrame == 1);
    REQUIRE(e.removeTag(0));
    CHECK(e.document().tags().empty());
    REQUIRE(e.undo());
    CHECK(e.document().tags().size() == 1);

    REQUIRE(e.setFrameDurations(1, 3, 40));
    CHECK(e.document().frames()[2].durationMs == 40);
    CHECK(e.document().frames()[0].durationMs == 100);
    REQUIRE(e.undo());
    CHECK(e.document().frames()[2].durationMs == 120);
}

TEST_CASE("Onion skin shows tinted, fading neighbours only", "[animation][onion]") {
    Editor e = fourFrames();   // frame f has one red-ish pixel at x = f
    OnionSkin o;
    o.before = 2;
    o.after = 1;
    o.opacity = 60;
    const Image ghosts = renderOnionSkin(e.document(), 2, o);
    CHECK(ghosts.pixel(2, 0).a == 0);                 // current frame is not a ghost
    CHECK(ghosts.pixel(1, 0).a == 153);               // previous: 60 %
    CHECK(ghosts.pixel(0, 0).a == 76);                // two back: 30 %
    CHECK(ghosts.pixel(3, 0).a == 153);               // next: 60 %
    CHECK(ghosts.pixel(3, 0).b > ghosts.pixel(3, 0).r);   // later frames tinted blue
    o.tint = false;
    o.after = 0;
    CHECK(renderOnionSkin(e.document(), 2, o).pixel(3, 0).a == 0);
    o.wrap = true;
    o.before = 1;
    CHECK(renderOnionSkin(e.document(), 0, o).pixel(3, 0).a > 0);   // wraps to the last frame
    o.opacity = 0;
    CHECK(renderOnionSkin(e.document(), 2, o).isFullyTransparent());
}

TEST_CASE("Onion skin with a composite provider matches on-demand rendering", "[animation][onion]") {
    auto d = Document::create(6, 6).value();
    const LayerId l = d.addLayer("L").value();
    for (int i = 0; i < 4; ++i) {
        if (i) REQUIRE(d.insertFrame(i));
        REQUIRE(d.setCel(l, i, Cel{Image(2, 2, Rgba8::opaque(static_cast<std::uint8_t>(40 * i), 100, 200)), i, i}));
    }
    OnionSkin o;
    o.before = 2;
    o.after = 1;
    const Image direct = renderOnionSkin(d, 2, o);
    std::map<int, Image> cache;
    int calls = 0;
    auto provider = [&](int f) -> const Image& {
        ++calls;
        auto it = cache.find(f);
        if (it == cache.end()) it = cache.emplace(f, d.composite(f)).first;
        return it->second;
    };
    CHECK(renderOnionSkin(d, 2, o, provider) == direct);
    CHECK(calls == 3);
    CHECK(onionSkinFrames(d, 2, o) == std::vector<int>{0, 1, 3});
    o.tint = false;
    CHECK(renderOnionSkin(d, 2, o, provider) == renderOnionSkin(d, 2, o));
}

namespace {
// Frame i is identified by its duration (100 + i) and a 1x1 cel of that red value.
Editor framesEditor(int n) {
    auto d = Document::create(2, 2).value();
    const LayerId l = d.addLayer("L").value();
    for (int i = 0; i < n; ++i) {
        if (i) REQUIRE(d.insertFrame(i));
        REQUIRE(d.setFrameDuration(i, 100 + i));
        REQUIRE(d.setCel(l, i, Cel{Image(1, 1, Rgba8::opaque(static_cast<std::uint8_t>(i), 0, 0)), 0, 0}));
    }
    return Editor(std::move(d));
}
std::vector<int> order(const Editor& e) {
    std::vector<int> v;
    for (const auto& f : e.document().frames()) v.push_back(f.durationMs - 100);
    return v;
}
} // namespace

TEST_CASE("Frame range operations are single undo steps", "[animation][frames]") {
    Editor e = framesEditor(6);
    const std::size_t steps = e.history().size();
    REQUIRE(e.moveFrames(1, 3, 3));
    CHECK(order(e) == std::vector<int>{0, 4, 5, 1, 2, 3});
    CHECK(e.history().size() == steps + 1);
    const LayerId l = e.document().layers()[0].id;
    CHECK(e.document().cel(l, 3)->image.pixel(0, 0).r == 1);    // cels travel with frames
    REQUIRE(e.undo());
    CHECK(order(e) == std::vector<int>{0, 1, 2, 3, 4, 5});
    REQUIRE(e.moveFrames(3, 5, 0));
    CHECK(order(e) == std::vector<int>{3, 4, 5, 0, 1, 2});
    REQUIRE(e.undo());

    auto first = e.duplicateFrames(1, 2);
    REQUIRE(first);
    CHECK(first.value() == 3);
    CHECK(order(e) == std::vector<int>{0, 1, 2, 1, 2, 3, 4, 5});
    CHECK(e.history().undoLabel() == "Duplicate 2 Frames");
    REQUIRE(e.undo());
    CHECK(order(e) == std::vector<int>{0, 1, 2, 3, 4, 5});

    REQUIRE(e.removeFrames(2, 4));
    CHECK(order(e) == std::vector<int>{0, 1, 5});
    REQUIRE(e.undo());
    CHECK(order(e) == std::vector<int>{0, 1, 2, 3, 4, 5});
    CHECK(e.document().cel(l, 3)->image.pixel(0, 0).r == 3);
    REQUIRE(e.redo());
    CHECK(order(e) == std::vector<int>{0, 1, 5});

    CHECK_FALSE(e.removeFrames(0, 2));          // would remove every frame
    CHECK_FALSE(e.moveFrames(0, 1, 2));         // past the end
    CHECK_FALSE(e.duplicateFrames(2, 1));
}

TEST_CASE("Linked cels share pixels through painting, frame edits and undo", "[animation][links]") {
    Editor e = framesEditor(4);
    const LayerId l = e.document().layers()[0].id;
    const Rgba8 red1 = e.document().cel(l, 1)->image.pixel(0, 0);
    REQUIRE(e.linkCels(l, {1, 2, 3}));
    CHECK(e.document().linkedFrames(l, 2) == std::vector<int>{1, 2, 3});
    CHECK(e.document().cel(l, 3)->image.pixel(0, 0) == red1);    // frame 1's cel is shared
    CHECK(e.document().validate());

    // Painting on frame 2 shows on 1 and 3, not on 0; undo restores all.
    {
        auto s = PaintSession::begin(e, l, 2, "Dot");
        REQUIRE(s);
        s.value()->writePixel(1, 1, Rgba8::opaque(9, 9, 9));
        REQUIRE(s.value()->commit());
    }
    for (int f : {1, 2, 3}) CHECK(e.document().composite(f).pixel(1, 1) == Rgba8::opaque(9, 9, 9));
    CHECK(e.document().composite(0).pixel(1, 1).a == 0);
    CHECK(e.document().validate());
    REQUIRE(e.undo());
    for (int f : {1, 2, 3}) CHECK(e.document().composite(f).pixel(1, 1).a == 0);
    REQUIRE(e.redo());
    CHECK(e.document().composite(3).pixel(1, 1) == Rgba8::opaque(9, 9, 9));

    // Moving and deleting frames keeps the links; deleting down to one member dissolves the group.
    REQUIRE(e.moveFrame(3, 0));
    CHECK(e.document().linkedFrames(l, 0) == std::vector<int>{0, 2, 3});
    REQUIRE(e.removeFrame(2));
    CHECK(e.document().linkedFrames(l, 0) == std::vector<int>{0, 2});
    REQUIRE(e.undo());
    CHECK(e.document().linkedFrames(l, 0) == std::vector<int>{0, 2, 3});
    CHECK(e.document().validate());

    // Duplicated frames are independent copies.
    REQUIRE(e.duplicateFrame(0));
    CHECK(e.document().celLink(l, 1) == 0);
    REQUIRE(e.undo());

    // Unlink: each frame keeps its own copy; undo relinks.
    REQUIRE(e.unlinkCels(l, {0, 2, 3}));
    CHECK(e.document().celLink(l, 0) == 0);
    CHECK(e.document().cel(l, 2)->image == e.document().cel(l, 3)->image);
    REQUIRE(e.undo());
    CHECK(e.document().celLink(l, 3) != 0);

    // Undo of the link restores each frame's own pixels.
    while (e.history().canUndo()) REQUIRE(e.undo());
    CHECK(e.document().links().empty());
    CHECK(e.document().cel(l, 3)->image.pixel(0, 0).r == 3);
    CHECK_FALSE(e.linkCels(l, {1}));
}

TEST_CASE("Linked cels survive layer delete, duplicate and merge", "[animation][links]") {
    Editor e = framesEditor(3);
    const LayerId l = e.document().layers()[0].id;
    REQUIRE(e.linkCels(l, {0, 1}));
    const LayerId copy = e.duplicateLayer(l).value();
    CHECK(e.document().linkedFrames(copy, 1) == std::vector<int>{0, 1});
    REQUIRE(e.removeLayer(l));
    CHECK(e.document().celLink(l, 0) == 0);
    REQUIRE(e.undo());
    CHECK(e.document().linkedFrames(l, 0) == std::vector<int>{0, 1});
    CHECK(e.document().validate());
    // Merging an empty layer down keeps the links (results stay identical).
    const LayerId top = e.addLayer("Empty", LayerKind::Raster, l).value();
    REQUIRE(e.mergeDown(top));
    CHECK(e.document().linkedFrames(l, 0) == std::vector<int>{0, 1});
    CHECK(e.document().validate());
}

TEST_CASE("Non-contiguous frame sets: delete and durations in one step", "[animation][frames]") {
    Editor e = framesEditor(6);
    REQUIRE(e.removeFrames(std::vector<int>{4, 1, 2}));
    CHECK(order(e) == std::vector<int>{0, 3, 5});
    CHECK(e.history().undoLabel() == "Delete 3 Frames");
    REQUIRE(e.undo());
    CHECK(order(e) == std::vector<int>{0, 1, 2, 3, 4, 5});
    REQUIRE(e.setFrameDurations(std::vector<int>{0, 5}, 250));
    CHECK(e.document().frames()[0].durationMs == 250);
    CHECK(e.document().frames()[5].durationMs == 250);
    CHECK(e.document().frames()[1].durationMs == 101);
    REQUIRE(e.undo());
    CHECK(e.document().frames()[5].durationMs == 105);
    CHECK_FALSE(e.removeFrames(std::vector<int>{0, 1, 2, 3, 4, 5}));
}

TEST_CASE("addAnimationLayer adds frames, a layer and a tag in one step", "[animation]") {
    auto d = Document::create(4, 4).value();
    (void)d.addLayer("Base");
    Editor e(std::move(d));
    const std::size_t steps = e.history().size();
    std::vector<Image> frames;
    for (int i = 0; i < 3; ++i) frames.push_back(Image(4, 4, Rgba8{static_cast<std::uint8_t>(50 * i + 10), 0, 0, 255}));
    auto id = e.addAnimationLayer("Knight walk", frames, 0, 80, "walk");
    REQUIRE(id);
    CHECK(e.history().size() == steps + 1);
    CHECK(e.history().undoLabel() == "Add Knight walk");
    CHECK(e.document().frameCount() == 3);
    CHECK(e.document().frames()[2].durationMs == 80);
    CHECK(e.document().cel(id.value(), 2)->image.pixel(0, 0).r == 110);
    REQUIRE(e.document().tags().size() == 1);
    CHECK(e.document().tags()[0].name == "walk");
    CHECK(e.document().tags()[0].toFrame == 2);
    REQUIRE(e.undo());
    CHECK(e.document().frameCount() == 1);
    CHECK(e.document().layer(id.value()) == nullptr);
    CHECK(e.document().tags().empty());
    REQUIRE(e.redo());
    CHECK(e.document().cel(id.value(), 1)->image.pixel(0, 0).r == 60);
    CHECK_FALSE(e.addAnimationLayer("x", {}, 0, 100));
    // setCelImage on its own
    REQUIRE(e.setCelImage(id.value(), 0, Image()));
    CHECK(e.document().cel(id.value(), 0) == nullptr);
    REQUIRE(e.undo());
    CHECK(e.document().cel(id.value(), 0) != nullptr);
}
