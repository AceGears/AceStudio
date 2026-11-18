#include "acestudio/core/DirectionSet.h"
#include "acestudio/core/MotionLibrary.h"
#include "acestudio/core/PngCodec.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>

using namespace acestudio;
using namespace acestudio::directions;

namespace {

Image fixture(const char* name) {
    auto img = readPng(std::filesystem::path(ACESTUDIO_SOURCE_DIR) / "tests" / "data" / "anim" / name);
    REQUIRE(img);
    return img.value();
}

Image flipped(const Image& img) {
    Image out(img.width(), img.height());
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) out.setPixel(img.width() - 1 - x, y, img.pixel(x, y));
    return out;
}

Image dot(int w, int h) {
    Image img(w, h);
    img.setPixel(w / 2, h - 1, Rgba8{255, 0, 0, 255});
    return img;
}

std::vector<DrawnView> fourViews() {
    return {{"s", 0, dot(8, 8)}, {"se", 45, dot(8, 8)}, {"e", 90, dot(8, 8)}, {"n", 180, dot(8, 8)}};
}

Image cropped(const Image& img) { return img.copy(img.opaqueBounds()); }

// Two silhouettes' overlap over their union, bottom-left corners together.
double iou(const Image& a, const Image& b) {
    int both = 0, any = 0;
    const int w = std::max(a.width(), b.width()), h = std::max(a.height(), b.height());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const bool p = a.pixel(x, a.height() - h + y).a > 0, q = b.pixel(x, b.height() - h + y).a > 0;
            both += p && q;
            any += p || q;
        }
    return any ? static_cast<double>(both) / any : 1;
}

} // namespace

TEST_CASE("Direction names and turns", "[directions]") {
    CHECK(viewTurn("S") == 0);
    CHECK(viewTurn("front") == 0);
    CHECK(viewTurn("front-left") == -45);
    CHECK(viewTurn("nw") == -135);
    CHECK(viewTurn("back") == 180);
    CHECK(viewTurn("E") == 90);
    CHECK_FALSE(viewTurn("up"));
    CHECK_FALSE(viewTurn(""));
    CHECK(compassName(100) == "e");
    CHECK(compassName(-170) == "n");
    CHECK(directionName(45.4) == "se");
    CHECK(directionName(30) == "turn30");
    CHECK(directionName(-60) == "turn-60");
    CHECK(wrapTurn(270) == -90);
    CHECK(wrapTurn(-180) == 180);
    CHECK(wrapTurn(540) == 180);
    CHECK(compassTurns(8).size() == 8);
    CHECK(compassTurns(4) == std::vector<double>{0, 90, 180, -90});
}

TEST_CASE("The nearest drawn view is chosen, mirrors for the other side", "[directions]") {
    const auto views = fourViews();
    auto c = chooseView(views, 10);
    CHECK(c.view == 0);
    CHECK_FALSE(c.mirrored);
    CHECK(c.offBy == 10);

    c = chooseView(views, -90);   // facing left: the right view mirrored
    CHECK(c.view == 2);
    CHECK(c.mirrored);
    CHECK(c.turn == -90);
    CHECK(c.offBy == 0);

    c = chooseView(views, -45);
    CHECK(c.view == 1);
    CHECK(c.mirrored);

    c = chooseView(views, 0);     // the front's mirror is also the front: the drawn art wins
    CHECK(c.view == 0);
    CHECK_FALSE(c.mirrored);

    c = chooseView(views, 135);   // halfway between two drawn views
    CHECK(c.offBy == 45);
    CHECK_FALSE(c.mirrored);

    c = chooseView(views, -90, false);
    CHECK_FALSE(c.mirrored);
    CHECK(c.offBy == 90);

    // The mirrored art is the drawn art flipped.
    std::vector<DrawnView> one{{"e", 90, fixture("knight32.png")}};
    c = chooseView(one, -90);
    CHECK(choiceArt(one, c) == flipped(one[0].art));
    CHECK(choiceArt(one, chooseView(one, 90)) == one[0].art);
}

TEST_CASE("Direction sets refuse bad views", "[directions]") {
    CHECK_FALSE(checkViews({}));
    CHECK(checkViews(fourViews()));
    auto views = fourViews();
    views.push_back({"again", 0.5, dot(8, 8)});
    auto st = checkViews(views);
    REQUIRE_FALSE(st);
    CHECK(st.error().message.find("same turn") != std::string::npos);
    CHECK_FALSE(checkViews({{"s", 0, Image(8, 8)}}));          // nothing drawn
    CHECK_FALSE(checkViews({{"", 0, dot(8, 8)}}));             // no name
    CHECK_FALSE(checkViews({{"s", std::nan(""), dot(8, 8)}}));
    std::vector<DrawnView> many;
    for (int i = 0; i <= kMaxViews; ++i) many.push_back({"v" + std::to_string(i), i * 20.0, dot(8, 8)});
    CHECK_FALSE(checkViews(many));

    auto walk = mocap::builtInPreset("walk");
    REQUIRE(walk);
    DirectionOptions o;
    CHECK_FALSE(retargetDirections(fourViews(), walk.value(), o));   // no directions
    o.turns = {0, 0.5};
    CHECK_FALSE(retargetDirections(fourViews(), walk.value(), o));   // twice
}

TEST_CASE("A motion is made in eight directions from two drawn views", "[directions]") {
    // The rogue stands for the front view and the knight for the right side:
    // the art is only chosen and placed here, so any two drawings will do.
    const std::vector<DrawnView> views{{"front", 0, fixture("rogue64.png")}, {"right", 90, fixture("knight32.png")}};
    auto walk = mocap::builtInPreset("walk");
    REQUIRE(walk);
    DirectionOptions o;
    o.turns = compassTurns(8);
    o.retarget.fps = 8;
    auto made = retargetDirections(views, walk.value(), o);
    REQUIRE(made);
    const auto& dirs = made.value();
    REQUIRE(dirs.size() == 8);
    const std::size_t frames = dirs[0].result.clip.frames.size();
    CHECK(frames >= 4);
    for (const DirectionClip& d : dirs) {
        INFO(d.direction);
        CHECK(d.result.clip.frames.size() == frames);
        CHECK(d.result.camera.turn == d.turn);
        CHECK(d.result.clip.name == d.direction);
    }
    auto find = [&](const std::string& n) -> const DirectionClip& {
        for (const auto& d : dirs)
            if (d.direction == n) return d;
        FAIL("no direction " << n);
        return dirs[0];
    };
    CHECK(find("s").viewName == "front");
    CHECK_FALSE(find("s").approximateView);
    CHECK(find("e").viewName == "right");
    CHECK_FALSE(find("e").approximateView);
    CHECK(find("w").viewName == "right (mirrored)");
    CHECK_FALSE(find("w").approximateView);
    CHECK(find("sw").viewName == "front");   // tie with the mirrored right: drawn art wins
    CHECK(find("se").approximateView);       // 45 degrees from both drawn views
    CHECK(find("n").approximateView);        // no back view drawn
    bool notedMirror = false;
    for (const auto& n : find("w").result.notes) notedMirror = notedMirror || n.find("mirror") != std::string::npos;
    CHECK(notedMirror);
    bool notedTurn = false;
    for (const auto& n : find("n").result.notes) notedTurn = notedTurn || n.find("at turn 90") != std::string::npos;
    CHECK(notedTurn);

    // Facing left is the right-facing walk seen in a mirror.
    // (Frames are cropped to the figure: the rig pads the two sides unevenly.)
    const Image left0 = cropped(find("w").result.clip.frames[0]);
    const Image right0 = flipped(cropped(find("e").result.clip.frames[0]));
    INFO(left0.width() << "x" << left0.height() << " vs " << right0.width() << "x" << right0.height());
    CHECK(std::abs(left0.width() - right0.width()) <= 1);
    CHECK(std::abs(left0.height() - right0.height()) <= 1);
    CHECK(iou(left0, right0) > 0.8);

    // Without mirrors, facing left falls back to a drawn view, flagged.
    o.allowMirror = false;
    o.turns = {-90};
    auto plain = retargetDirections(views, walk.value(), o);
    REQUIRE(plain);
    CHECK_FALSE(plain.value()[0].choice.mirrored);
    CHECK(plain.value()[0].approximateView);
}

TEST_CASE("A turn clip cuts between drawn views on one ground row", "[directions]") {
    const Image rogue = fixture("rogue64.png");
    const std::vector<DrawnView> views{{"s", 0, rogue}, {"e", 90, fixture("knight32.png")}, {"n", 180, flipped(rogue)}};
    auto clip = turnClip(views, 0, 180);
    REQUIRE(clip);
    const motion::Clip& c = clip.value();
    REQUIRE(c.frames.size() == 3);   // s, e, n: the in-between steps show the same art
    CHECK_FALSE(c.loops);
    const int bottom = c.frames[0].opaqueBounds().y + c.frames[0].opaqueBounds().h;
    for (const Image& f : c.frames) {
        CHECK(f.width() == c.frames[0].width());
        CHECK(f.height() == c.frames[0].height());
        CHECK(f.opaqueBounds().y + f.opaqueBounds().h == bottom);
        CHECK(std::abs((f.opaqueBounds().x + f.opaqueBounds().w / 2.0) - f.width() / 2.0) <= 1);
    }
    CHECK(c.durations == std::vector<int>{100, 100, 100});

    // To the left: the front, then the right view mirrored.
    auto left = turnClip(views, 0, -90, 45, 80);
    REQUIRE(left);
    REQUIRE(left.value().frames.size() == 2);
    CHECK(left.value().durations[0] == 80);

    CHECK_FALSE(turnClip(views, 0, 90, 0));
    CHECK_FALSE(turnClip(views, 0, 90, 45, 5));
    CHECK_FALSE(turnClip({}, 0, 90));
}

TEST_CASE("Direction clips go onto one document, one tag each", "[directions]") {
    const std::vector<DrawnView> views{{"front", 0, fixture("rogue64.png")}, {"right", 90, fixture("knight32.png")}};
    auto walk = mocap::builtInPreset("walk");
    auto jump = mocap::builtInPreset("jump");
    REQUIRE(walk);
    REQUIRE(jump);
    DirectionOptions o;
    o.turns = compassTurns(4);
    o.retarget.fps = 8;
    auto walks = retargetDirections(views, walk.value(), o);
    REQUIRE(walks);
    o.turns = {90};
    o.retarget.loop = false;
    auto jumps = retargetDirections(views, jump.value(), o);
    REQUIRE(jumps);
    auto turn = turnClip(views, 0, 90);
    REQUIRE(turn);

    std::vector<NamedClip> clips;
    std::size_t total = 0;
    for (const auto& d : walks.value()) {
        clips.push_back({"walk_" + d.direction, &d.result.clip});
        total += d.result.clip.frames.size();
    }
    clips.push_back({"jump_e", &jumps.value()[0].result.clip});
    clips.push_back({"turn_s_e", &turn.value()});
    total += jumps.value()[0].result.clip.frames.size() + turn.value().frames.size();
    auto doc = clipsDocument(clips);
    REQUIRE(doc);
    const Document& d = doc.value();
    CHECK(static_cast<std::size_t>(d.frameCount()) == total);
    REQUIRE(d.tags().size() == 6);
    CHECK(d.tags()[0].name == "walk_s");
    CHECK(d.tags()[0].fromFrame == 0);
    CHECK(d.tags()[0].repeat == 0);
    CHECK(d.tags()[4].name == "jump_e");
    CHECK(d.tags()[4].repeat == 1);
    CHECK(d.tags()[5].toFrame == d.frameCount() - 1);
    REQUIRE(d.layers().size() == 1);
    CHECK(d.layers()[0].name == "Directions");
    // Every frame has art, and every cel fits on the canvas.
    for (int f = 0; f < d.frameCount(); ++f) {
        const Cel* cel = d.cel(d.layers()[0].id, f);
        REQUIRE(cel);
        CHECK(cel->x >= 0);
        CHECK(cel->y >= 0);
        CHECK(cel->x + cel->image.width() <= d.width());
        CHECK(cel->y + cel->image.height() <= d.height());
    }
    CHECK(d.frames()[0].durationMs == walks.value()[0].result.clip.durations[0]);

    clips.push_back({"walk_s", &turn.value()});
    CHECK_FALSE(clipsDocument(clips));   // the tag again
    CHECK_FALSE(clipsDocument({}));
}
