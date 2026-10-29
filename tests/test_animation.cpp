#include <catch2/catch_test_macros.hpp>

#include "acestudio/core/Animation.h"
#include "acestudio/core/Editor.h"
#include "acestudio/core/ProjectIO.h"

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
