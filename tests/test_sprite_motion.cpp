#include "acestudio/core/SpriteMotion.h"

#include <catch2/catch_test_macros.hpp>

#include <set>

using namespace acestudio;
using namespace acestudio::motion;

namespace {

const Rgba8 kSkin{230, 180, 140, 255}, kShirt{40, 90, 200, 255}, kPants{60, 50, 40, 255}, kInk{20, 20, 30, 255};

// 16 x 24 figure: head, torso, two legs with a gap between them.
Image biped() {
    Image img(20, 28);
    img.fillRect({6, 2, 6, 6}, kSkin);
    img.fillRect({5, 8, 8, 8}, kShirt);
    img.fillRect({5, 16, 3, 9}, kPants);
    img.fillRect({10, 16, 3, 9}, kPants);
    img.fillRect({5, 24, 3, 1}, kInk);
    img.fillRect({10, 24, 3, 1}, kInk);
    return img;
}

Image quadruped() {
    Image img(40, 24);
    img.fillRect({4, 4, 32, 10}, kShirt);    // body
    img.fillRect({32, 0, 6, 6}, kSkin);      // head
    for (int x : {6, 12, 26, 32}) img.fillRect({x, 14, 3, 8}, kPants);
    return img;
}

Image blob() {
    Image img(24, 24);
    for (int y = 0; y < 24; ++y)
        for (int x = 0; x < 24; ++x)
            if ((x - 11.5) * (x - 11.5) + (y - 13.5) * (y - 13.5) <= 100) img.setPixel(x, y, kShirt);
    for (int x = 3; x < 21; ++x) img.setPixel(x, 23, kShirt);   // flat bottom
    return img;
}

std::set<std::uint32_t> colours(const Image& img) {
    std::set<std::uint32_t> s;
    for (const Rgba8& c : img.pixels())
        if (c.a) s.insert((std::uint32_t(c.r) << 24) | (std::uint32_t(c.g) << 16) | (std::uint32_t(c.b) << 8) | c.a);
    return s;
}

} // namespace

TEST_CASE("subjects are classified from the silhouette", "[motion]") {
    const Subject b = analyzeSubject(biped());
    CHECK(b.body == Body::Biped);
    REQUIRE(b.feet.size() == 2);
    CHECK(b.legTop >= 14);
    CHECK(b.legTop <= 17);

    const Subject q = analyzeSubject(quadruped());
    CHECK(q.body == Body::Quadruped);
    CHECK(q.feet.size() == 4);

    CHECK(analyzeSubject(blob()).body == Body::Blob);
}

TEST_CASE("every action renders with the sprite's own colours", "[motion]") {
    const Image src = biped();
    const auto palette = colours(src);
    for (const ActionInfo& info : actions()) {
        Options o;
        o.action = info.action;
        auto clip = animate(src, o);
        REQUIRE(clip);
        const Clip& c = clip.value();
        INFO(info.key);
        REQUIRE(static_cast<int>(c.frames.size()) == info.frames);
        CHECK(c.durations.size() == c.frames.size());
        CHECK(c.loops == info.loops);
        for (const Image& f : c.frames) {
            CHECK(f.width() == c.frames[0].width());
            CHECK(f.height() == c.frames[0].height());
            CHECK_FALSE(f.isFullyTransparent());
            if (info.action != Action::Hurt)
                for (std::uint32_t col : colours(f)) CHECK(palette.count(col) == 1);
        }
    }
}

TEST_CASE("a walk moves the legs and keeps the body", "[motion]") {
    Options o;
    o.action = Action::Walk;
    auto clip = animate(biped(), o);
    REQUIRE(clip);
    const Clip& c = clip.value();
    REQUIRE(c.frames.size() == 8);
    CHECK(c.body == Body::Biped);
    CHECK_FALSE(c.frames[0] == c.frames[2]);   // contact vs passing pose differ
    const ClipCheck chk = checkClip(c);
    CHECK(chk.issues.empty());
    CHECK(chk.maxAreaChange < 0.2);
    CHECK(chk.minNeighbourIoU > 0.5);
}

TEST_CASE("a footless blob hops instead of walking", "[motion]") {
    Options o;
    o.action = Action::Walk;
    auto clip = animate(blob(), o);
    REQUIRE(clip);
    CHECK(clip.value().body == Body::Blob);
    // At least one frame is in the air: its lowest row is above the ground of frame 0.
    const auto ground = [](const Image& f) { const IntRect b = f.opaqueBounds(); return b.y + b.h; };
    const int g0 = ground(clip.value().frames[0]);
    bool airborne = false;
    for (const Image& f : clip.value().frames) airborne = airborne || ground(f) < g0;
    CHECK(airborne);
}

TEST_CASE("death ends lying down and the body override is honoured", "[motion]") {
    Options o;
    o.action = Action::Death;
    auto clip = animate(biped(), o);
    REQUIRE(clip);
    const IntRect last = clip.value().frames.back().opaqueBounds();
    CHECK(last.w > last.h);

    o.action = Action::Walk;
    o.body = Body::Object;
    auto rocked = animate(biped(), o);
    REQUIRE(rocked);
    CHECK(rocked.value().body == Body::Object);
}

TEST_CASE("packs share one frame size and mirroring faces left", "[motion]") {
    Options o;
    auto pack = animatePack(biped(), {Action::Idle, Action::Walk, Action::Jump, Action::Attack1}, o);
    REQUIRE(pack);
    REQUIRE(pack.value().size() == 4);
    for (const Clip& c : pack.value()) {
        CHECK(c.frames[0].width() == pack.value()[0].frames[0].width());
        CHECK(c.frames[0].height() == pack.value()[0].frames[0].height());
    }
    o.action = Action::Idle;
    auto right = animate(biped(), o);
    o.faceLeft = true;
    auto left = animate(biped(), o);
    REQUIRE(right);
    REQUIRE(left);
    const Image& r = right.value().frames[0];
    const Image& l = left.value().frames[0];
    REQUIRE(r.width() == l.width());
    for (int y = 0; y < r.height(); ++y)
        for (int x = 0; x < r.width(); ++x) REQUIRE(r.pixel(x, y) == l.pixel(r.width() - 1 - x, y));
}

TEST_CASE("an empty sprite is refused", "[motion]") {
    CHECK_FALSE(animate(Image(8, 8), Options{}));
    CHECK(actionFromKey("attack2") == Action::Attack2);
    CHECK_FALSE(actionFromKey("moonwalk"));
}

TEST_CASE("a wing flap turns the part above the wing line about its root", "[motion]") {
    Image dragon(30, 30);
    dragon.fillRect({8, 16, 16, 10}, kShirt);   // body
    dragon.fillRect({10, 2, 8, 14}, kSkin);     // wing, above the line
    Options o;
    o.action = Action::Flap;
    o.wingLine = 16.0 / 24.0;                    // the body's top, as a fraction of the opaque height (2..26)
    auto clip = animate(dragon, o);
    REQUIRE(clip);
    const auto& fr = clip.value().frames;
    REQUIRE(fr.size() == 8);   // fast downstroke, slower recovery
    auto skinTop = [&](const Image& f) {
        for (int y = 0; y < f.height(); ++y)
            for (int x = 0; x < f.width(); ++x)
                if (f.pixel(x, y) == kSkin) return y;
        return f.height();
    };
    auto count = [](const Image& f, Rgba8 c) {
        int n = 0;
        for (const Rgba8& p : f.pixels()) n += p == c ? 1 : 0;
        return n;
    };
    // The wing (part rig, ADR-ANIM-001) turns down and back up: its top
    // comes down, and it keeps its pixels (turned, not folded flat).
    bool lowered = false;
    const int wing0 = count(dragon, kSkin);
    for (const Image& f : fr) {
        lowered = lowered || skinTop(f) >= skinTop(fr[0]) + 4;
        CHECK(count(f, kSkin) * 10 >= wing0 * 7);
    }
    CHECK(lowered);
    // With the rig the app's own checks pass: no area swing beyond the
    // limit, no flicker, a clean loop.
    const ClipCheck chk = checkClip(clip.value());
    INFO(chk.maxAreaChange << " " << chk.minNeighbourIoU << " " << chk.loopIoU);
    for (const auto& i : chk.issues) UNSCOPED_INFO(i);
    CHECK(chk.issues.empty());
}

TEST_CASE("a wing mask chooses exactly which pixels flap", "[motion]") {
    Image dragon(30, 30);
    dragon.fillRect({8, 16, 16, 10}, kShirt);   // body
    dragon.fillRect({10, 2, 8, 14}, kSkin);     // wing
    dragon.fillRect({20, 6, 6, 10}, kPants);    // head, also above the line
    Options o;
    o.action = Action::Flap;
    o.wingLine = 14.0 / 24.0;
    o.wingMask = Mask(30, 30);
    for (int y = 2; y < 16; ++y)
        for (int x = 10; x < 18; ++x) o.wingMask.set(x, y, true);
    auto clip = animate(dragon, o);
    REQUIRE(clip);
    // The head never moves (only the body's bob, which is whole pixels): its pixel count stays.
    for (const Image& f : clip.value().frames) {
        int head = 0;
        for (const Rgba8& c : f.pixels()) head += c == kPants;
        CHECK(head == 60);
    }
}

namespace {
// A side-view fish facing left: round head end, thin tail stalk, forked tail fin,
// and two short belly fins that must not count as legs.
Image fish() {
    Image img(40, 22);
    for (int y = 0; y < 22; ++y)
        for (int x = 0; x < 40; ++x) {
            const double bx = (x - 13.0) / 13.0, by = (y - 10.0) / 7.0;
            if (bx * bx + by * by <= 1.0) img.setPixel(x, y, kShirt);           // body, x 0..26
        }
    img.fillRect({26, 9, 4, 3}, kShirt);                                          // tail stalk
    for (int i = 0; i < 7; ++i) img.fillRect({30 + i, 9 - i, 2, 3 + 2 * i}, kSkin);   // tail fin, opening right
    img.fillRect({8, 17, 3, 1}, kSkin);                                           // belly fins, tapering
    img.setPixel(9, 18, kSkin);
    img.fillRect({16, 17, 3, 1}, kSkin);
    img.setPixel(17, 18, kSkin);
    img.setPixel(3, 9, kInk);                                                     // eye
    return img;
}
} // namespace

TEST_CASE("a fish is a swimmer: fins are not legs, and its head is found", "[motion]") {
    const Subject s = analyzeSubject(fish());
    CHECK(s.body == Body::Swimmer);
    CHECK(s.feet.empty());
    CHECK(s.headLeft);
    CHECK(spriteFacesLeft(fish()));
    Image right(40, 22);
    const Image f = fish();
    for (int y = 0; y < 22; ++y)
        for (int x = 0; x < 40; ++x) right.setPixel(39 - x, y, f.pixel(x, y));
    CHECK_FALSE(spriteFacesLeft(right));
    // The legged test figures are unchanged.
    CHECK(analyzeSubject(biped()).body == Body::Biped);
    CHECK(analyzeSubject(quadruped()).body == Body::Quadruped);
}

TEST_CASE("a swimmer's swim bends the tail and keeps the head", "[motion]") {
    Options o;
    o.action = Action::Swim;
    auto clip = animate(fish(), o);
    REQUIRE(clip);
    const Clip& c = clip.value();
    CHECK(c.body == Body::Swimmer);
    REQUIRE(c.frames.size() == 12);   // a longer cycle for smoothness
    CHECK(c.durations[0] == 70);
    const ClipCheck chk = checkClip(c);
    for (const auto& i : chk.issues) INFO(i);
    CHECK(chk.issues.empty());
    // The eye (head end) hardly moves; the tail tip moves a lot.
    auto rowOf = [](const Image& img, Rgba8 colour, bool lastColumn) {
        int bestX = lastColumn ? -1 : 1 << 20, row = -1;
        for (int y = 0; y < img.height(); ++y)
            for (int x = 0; x < img.width(); ++x)
                if (img.pixel(x, y) == colour && (lastColumn ? x > bestX : x < bestX)) { bestX = x; row = y; }
        return row;
    };
    int eyeMin = 1 << 20, eyeMax = -1, tailMin = 1 << 20, tailMax = -1;
    for (const Image& f : c.frames) {
        const int e = rowOf(f, kInk, false), t = rowOf(f, kSkin, true);
        eyeMin = std::min(eyeMin, e); eyeMax = std::max(eyeMax, e);
        tailMin = std::min(tailMin, t); tailMax = std::max(tailMax, t);
    }
    CHECK(eyeMax - eyeMin <= 1);
    CHECK(tailMax - tailMin >= 2);
}

TEST_CASE("rotating a sprite keeps its pixels and colours", "[motion]") {
    const Image f = fish();
    const auto palette = colours(f);
    // A quarter turn is exact: a transposed copy.
    const Image q = rotateSprite(f, 3.14159265358979323846 / 2);
    const IntRect b = q.opaqueBounds();
    CHECK(b.w == f.opaqueBounds().h);
    CHECK(b.h == f.opaqueBounds().w);
    std::size_t before = 0, after = 0;
    for (const Rgba8& c : f.pixels()) before += c.a > 0;
    for (const Rgba8& c : q.pixels()) after += c.a > 0;
    CHECK(after == before);
    // 45 degrees: no new colours (no blending), and about the same area.
    const Image d = rotateSprite(f, 3.14159265358979323846 / 4);
    for (std::uint32_t c : colours(d)) CHECK(palette.count(c) == 1);
    std::size_t diag = 0;
    for (const Rgba8& c : d.pixels()) diag += c.a > 0;
    CHECK(diag > before * 8 / 10);
    CHECK(diag < before * 12 / 10);
    // Same size in, same size out: rotated animation frames stay aligned.
    CHECK(rotateSprite(f, 0.7).width() == rotateSprite(fish(), 0.7).width());
}

namespace {
// Front view: the legs touch (one run at the feet), arms hang beside the body.
Image frontKnight() {
    Image img(32, 48);
    img.fillRect({12, 4, 8, 9}, kSkin);      // head
    img.fillRect({11, 13, 10, 15}, kShirt);  // torso
    img.fillRect({8, 14, 3, 15}, kSkin);     // arms
    img.fillRect({21, 14, 3, 15}, kSkin);
    img.fillRect({12, 28, 8, 14}, kPants);   // legs, touching
    img.fillRect({11, 42, 10, 3}, kInk);     // boots
    return img;
}
int lowestRow(const Image& img) {
    for (int y = img.height() - 1; y >= 0; --y)
        for (int x = 0; x < img.width(); ++x)
            if (img.pixel(x, y).a > 0) return y;
    return -1;
}
} // namespace

TEST_CASE("Touching legs seen from the front are still a biped with arms", "[motion]") {
    const Subject s = analyzeSubject(frontKnight());
    CHECK(s.body == Body::Biped);
    CHECK(s.frontView);
    CHECK(s.feet.size() == 2);
    CHECK(s.arms.size() == 2);
    // A tree (narrow trunk under a wide canopy) is not taken for legs.
    Image tree(32, 48);
    tree.fillRect({2, 2, 28, 26}, kShirt);
    tree.fillRect({13, 28, 6, 18}, kPants);
    CHECK(analyzeSubject(tree).body != Body::Biped);
}

TEST_CASE("Walk and run keep a foot on the ground in every frame", "[motion]") {
    for (const Image& sprite : {biped(), frontKnight()})
        for (Action a : {Action::Walk, Action::Run}) {
            Options o;
            o.action = a;
            o.frames = 8;
            auto clip = animate(sprite, o);
            REQUIRE(clip);
            const int ground = lowestRow(clip.value().frames.front());
            for (const Image& f : clip.value().frames) CHECK(lowestRow(f) == ground);
            const ClipCheck chk = checkClip(clip.value());
            CHECK(chk.floatingFrames == 0);
            for (const auto& issue : chk.issues) UNSCOPED_INFO(issue);
            CHECK(chk.issues.empty());
        }
}

TEST_CASE("The clip check reports floating feet and single-frame flicker", "[motion]") {
    Clip c;
    c.name = "walk";
    c.body = Body::Biped;
    const Image a = biped();
    Image lifted(a.width(), a.height());
    lifted.blit(a, 0, -2);
    Image speckled = a;
    for (int i = 0; i < 40; ++i) speckled.setPixel((i * 7) % a.width(), (i * 5) % 20, kInk);
    c.frames = {a, lifted, a, speckled};
    c.durations = {100, 100, 100, 100};
    const ClipCheck chk = checkClip(c);
    CHECK(chk.floatingFrames == 1);
    CHECK(chk.flicker > 0.08);
    CHECK(chk.issues.size() >= 2);
}

TEST_CASE("A three-quarter stance (far foot higher) walks with every foot grounded", "[motion]") {
    // Legs apart, the far foot ending two rows above the near one (as a
    // generated 64 px rogue on the test PC).
    Image img(24, 44);
    img.fillRect({7, 2, 8, 10}, kSkin);
    img.fillRect({6, 12, 11, 14}, kShirt);
    img.fillRect({6, 26, 3, 14}, kPants);   // far leg, ends at row 39
    img.fillRect({13, 26, 3, 16}, kPants);  // near leg, ends at row 41
    const Subject s = analyzeSubject(img);
    REQUIRE(s.body == Body::Biped);
    REQUIRE(s.footBottoms.size() == 2);
    CHECK(s.footBottoms[1] - s.footBottoms[0] == 2);
    Options o;
    o.action = Action::Walk;
    o.frames = 8;
    auto clip = animate(img, o);
    REQUIRE(clip);
    CHECK(clip.value().footStagger == 2);
    const ClipCheck chk = checkClip(clip.value());
    for (const auto& issue : chk.issues) UNSCOPED_INFO(issue);
    CHECK(chk.floatingFrames == 0);
}

namespace {
// Is `part` (a rectangle of `src`) found unchanged somewhere in `frame`?
bool foundIntact(const Image& src, const IntRect& part, const Image& frame) {
    for (int oy = 0; oy + part.h <= frame.height(); ++oy)
        for (int ox = 0; ox + part.w <= frame.width(); ++ox) {
            bool ok = true;
            for (int y = 0; y < part.h && ok; ++y)
                for (int x = 0; x < part.w && ok; ++x) {
                    const Rgba8 a = src.pixel(part.x + x, part.y + y);
                    if (a.a && frame.pixel(ox + x, oy + y) != a) ok = false;
                }
            if (ok) return true;
        }
    return false;
}

// A side-view walker, toes pointing right: boots stick out forward.
Image sideWalker(bool faceLeft) {
    Image img(28, 44);
    img.fillRect({9, 2, 9, 10}, kSkin);
    img.fillRect({8, 12, 12, 14}, kShirt);
    img.fillRect({9, 26, 3, 14}, kPants);
    img.fillRect({15, 26, 3, 14}, kPants);
    img.fillRect({9, 40, 5, 3}, kInk);    // boots, toes to the right
    img.fillRect({15, 40, 5, 3}, kInk);
    if (!faceLeft) return img;
    Image m(img.width(), img.height());
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) m.setPixel(img.width() - 1 - x, y, img.pixel(x, y));
    return m;
}
} // namespace

TEST_CASE("The walk moves pixels, it never redraws them", "[motion][rig]") {
    const Image src = sideWalker(false);
    for (Action a : {Action::Walk, Action::Run, Action::Idle}) {
        Options o;
        o.action = a;
        o.frames = 8;
        auto clip = animate(src, o);
        REQUIRE(clip);
        for (const Image& f : clip.value().frames) {
            // The head and each part of the torso are a whole-pixel copy in
            // every frame (the shoulders, waist and pelvis shift against each
            // other, ADR-ANIM-001 humanoid tests 2 and 4).
            CHECK(foundIntact(src, {9, 2, 9, 10}, f));
            CHECK(foundIntact(src, {8, 14, 12, 4}, f));
            CHECK(foundIntact(src, {8, 18, 12, 4}, f));
            CHECK(foundIntact(src, {8, 22, 12, 2}, f));
            // Each boot keeps its exact shape.
            CHECK(foundIntact(src, {9, 40, 5, 3}, f));
            CHECK(foundIntact(src, {15, 40, 5, 3}, f));
            // Only the sprite's own colours.
            CHECK(colours(f).size() <= colours(src).size());
        }
    }
}

TEST_CASE("The idle breathes without moving the feet", "[motion][rig]") {
    const Image src = sideWalker(false);
    Options o;
    o.action = Action::Idle;
    o.frames = 8;
    auto clip = animate(src, o);
    REQUIRE(clip);
    const auto& fr = clip.value().frames;
    // The boots sit at the same place in every frame; the head rises in some.
    const IntRect boots0 = fr[0].opaqueBounds();
    bool rose = false;
    for (const Image& f : fr) {
        CHECK(lowestRow(f) == lowestRow(fr[0]));
        rose = rose || f.opaqueBounds().y < boots0.y;
    }
    CHECK(rose);
}

TEST_CASE("A walker steps the way it faces", "[motion][rig]") {
    for (bool left : {false, true}) {
        const Image src = sideWalker(left);
        const Subject s = analyzeSubject(src);
        CHECK(s.facing == (left ? -1 : 1));
        Options o;
        o.action = Action::Walk;
        o.frames = 8;
        auto clip = animate(src, o);
        REQUIRE(clip);
        // In the first half of the swing the lifted foot moves forward: the
        // boot pixels' centre in the lowest rows shifts towards the facing.
        auto soleCentre = [](const Image& f) {
            double sx = 0;
            int n = 0;
            const int y = lowestRow(f);
            for (int x = 0; x < f.width(); ++x)
                if (f.pixel(x, y).a) { sx += x; ++n; }
            return n ? sx / n : 0.0;
        };
        // Frames 1-3: one foot planted (the other lifted). A planted foot
        // moves back as the body goes forward.
        const double d = soleCentre(clip.value().frames[3]) - soleCentre(clip.value().frames[1]);
        CHECK((left ? d > 0 : d < 0));
    }
}

namespace {
// A side-view fish facing right: body, eye, tail fin on the left.
Image fish(int length, int depth) {
    Image img(length + 8, depth + 8);
    const double cx = 4 + length * 0.55, cy = 4 + depth / 2.0, rx = length * 0.45, ry = depth / 2.0;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x)
            if ((x - cx) * (x - cx) / (rx * rx) + (y - cy) * (y - cy) / (ry * ry) <= 1.0) img.setPixel(x, y, kShirt);
    const int tailX = static_cast<int>(cx - rx);
    for (int x = 4; x <= tailX; ++x)
        for (int y = static_cast<int>(cy) - (tailX - x + 1); y <= static_cast<int>(cy) + (tailX - x + 1); ++y)
            if (y >= 0 && y < img.height()) img.setPixel(x, y, kPants);
    img.setPixel(static_cast<int>(cx + rx * 0.6), static_cast<int>(cy) - 1, kInk);
    return img;
}
// How far a column's pixels move up or down over the clip.
int columnTravel(const std::vector<Image>& frames, double u) {
    int lo = 1 << 20, hi = -(1 << 20);
    for (const Image& f : frames) {
        const IntRect bb = f.opaqueBounds();
        const int x = bb.x + static_cast<int>(u * (bb.w - 1));
        int top = -1;
        for (int y = 0; y < f.height() && top < 0; ++y)
            if (f.pixel(x, y).a) top = y;
        if (top < 0) continue;
        lo = std::min(lo, top);
        hi = std::max(hi, top);
    }
    return hi - lo;
}
} // namespace

TEST_CASE("A fish swims with its body length kept, the tail most", "[motion][rig][swim]") {
    // The part rig (ADR-ANIM-001 5c): the body bends by whole-pixel column
    // moves without dropping columns (constant length, ADR fish test 1); the
    // tail fin beats behind the stalk and narrows; the head is nearly still.
    for (auto [len, depth] : {std::pair{40, 16}, std::pair{60, 12}, std::pair{30, 22}}) {
        INFO("fish " << len << "x" << depth);
        const Image src = fish(len, depth);
        Options o;
        o.action = Action::Swim;
        o.body = Body::Swimmer;
        o.frames = 12;
        auto clip = animate(src, o);
        REQUIRE(clip);
        const auto& fr = clip.value().frames;
        long srcCount = 0;
        for (const Rgba8& c : src.pixels()) srcCount += c.a ? 1 : 0;
        const IntRect sb = src.opaqueBounds();
        std::set<int> widths;
        int headX = -1;
        for (const Image& f : fr) {
            // Only the sprite's colours; the pixel count stays close.
            CHECK(colours(f).size() <= colours(src).size());
            long n = 0;
            for (const Rgba8& c : f.pixels()) n += c.a ? 1 : 0;
            CHECK(n * 10 <= srcCount * 11);
            CHECK(n * 10 >= srcCount * 8);
            const IntRect fb = f.opaqueBounds();
            widths.insert(fb.w);
            // The head end does not move along the body ...
            const int hx = fb.x + fb.w - 1;
            if (headX < 0) headX = hx;
            CHECK(hx == headX);
            // ... and the body is never shorter than the source minus what
            // the tail fin's foreshortening takes (under a fifth).
            CHECK(fb.w * 5 >= sb.w * 4);
            CHECK(fb.w <= sb.w + 2);
        }
        // The tail fin narrows and widens as it beats (an eel-like body,
        // five times as long as deep, has no separate tail fin: it undulates).
        if (len < depth * 4) CHECK(widths.size() >= 2);
        // The tail (left end) moves further than the head (right end).
        CHECK(columnTravel(fr, 0.05) > columnTravel(fr, 0.95));
        CHECK(columnTravel(fr, 0.05) >= 1);
        const ClipCheck chk = checkClip(clip.value());
        for (const auto& i : chk.issues) UNSCOPED_INFO(i);
        CHECK(chk.issues.empty());
    }
}
