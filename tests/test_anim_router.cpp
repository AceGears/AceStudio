#include "acestudio/core/AnimRouter.h"

#include <catch2/catch_test_macros.hpp>

using namespace acestudio;
using namespace acestudio::anim;

namespace {
const Rgba8 kBrown{110, 70, 40, 255}, kGreen{40, 120, 50, 255}, kBlue{40, 90, 200, 255}, kSkin{230, 180, 140, 255};

Image biped() {
    Image img(20, 28);
    img.fillRect({6, 2, 6, 6}, kSkin);
    img.fillRect({5, 8, 8, 8}, kBlue);
    img.fillRect({5, 16, 3, 9}, kBrown);
    img.fillRect({10, 16, 3, 9}, kBrown);
    return img;
}
Image campfire() {
    Image img(24, 24);
    img.fillRect({4, 18, 16, 4}, kBrown);                  // logs
    img.fillRect({8, 4, 8, 14}, Rgba8{250, 140, 30, 255});  // flame
    img.fillRect({10, 9, 4, 8}, Rgba8{255, 230, 90, 255});
    return img;
}
Image tree() {
    Image img(24, 32);
    img.fillRect({2, 2, 20, 18}, kGreen);
    img.fillRect({10, 20, 4, 12}, kBrown);
    return img;
}
} // namespace

TEST_CASE("The router picks effects from the asset's name", "[router]") {
    CHECK(routeAnimation(tree(), "Oak tree").effect == fx::Effect::Sway);
    CHECK(routeAnimation(tree(), "oak tree").method == Method::Effect);
    CHECK(routeAnimation(campfire(), "campfire_64").effect == fx::Effect::Fire);
    CHECK(routeAnimation(tree(), "castle banner").effect == fx::Effect::Flag);
    CHECK(routeAnimation(tree(), "waterfall").effect == fx::Effect::Water);
    CHECK(routeAnimation(tree(), "magic crystal").effect == fx::Effect::Pulse);
    // A creature named after an element still moves its body.
    CHECK(routeAnimation(biped(), "fire knight").method == Method::Body);
    // "fireplace" style substrings inside other words do not count.
    CHECK(routeAnimation(biped(), "bonfire").method == Method::Effect);
    CHECK(routeAnimation(biped(), "sapphire golem").method == Method::Body);
}

TEST_CASE("Without a name, the pixels decide", "[router]") {
    CHECK(routeAnimation(campfire(), "").effect == fx::Effect::Fire);
    // A campfire whose stone ring is most of the sprite (flames ~17 %, as a
    // generated one on the test PC) is still fire.
    Image ring(40, 40);
    ring.fillRect({2, 30, 36, 8}, Rgba8{130, 130, 135, 255});
    ring.fillRect({6, 26, 28, 6}, kBrown);
    ring.fillRect({16, 10, 8, 16}, Rgba8{240, 110, 30, 255});
    ring.fillRect({18, 14, 4, 11}, Rgba8{255, 225, 100, 255});
    CHECK(routeAnimation(ring, "").method == Method::Effect);
    // Warm but not fire: a red potion (no bright core) and a coin pouch (a
    // wide yellow pile).
    Image potion(24, 24);
    potion.fillRect({8, 2, 8, 4}, kBrown);
    potion.fillRect({4, 6, 16, 16}, Rgba8{220, 40, 40, 255});
    CHECK(routeAnimation(potion, "").method != Method::Effect);
    Image pouch(24, 24);
    pouch.fillRect({3, 8, 18, 15}, kBrown);
    pouch.fillRect({4, 4, 16, 5}, Rgba8{250, 200, 60, 255});
    CHECK(routeAnimation(pouch, "").method != Method::Effect);
    const Route b = routeAnimation(biped(), "");
    CHECK(b.method == Method::Body);
    CHECK(b.body == motion::Body::Biped);
    CHECK_FALSE(b.reason.empty());
    // Poses the rig cannot draw go to the AI pipeline.
    CHECK(routeAnimation(biped(), "", motion::Action::Cast).method == Method::Ai);
}

TEST_CASE("Water creatures swim by name, in their own style", "[router][swim]") {
    Image fish(40, 20);
    fish.fillRect({8, 4, 28, 12}, Rgba8{230, 120, 40, 255});
    fish.fillRect({2, 2, 6, 16}, Rgba8{200, 80, 30, 255});
    using SS = motion::SwimStyle;
    const std::pair<const char*, SS> cases[] = {{"Moray Eel", SS::Anguilliform}, {"goldfish_02", SS::Subcarangiform},
                                                {"Blue Tuna", SS::Thunniform},   {"great white shark", SS::Thunniform},
                                                {"piranha", SS::Carangiform},    {"river fish", SS::Subcarangiform}};
    for (const auto& [name, style] : cases) {
        const Route r = routeAnimation(fish, name);
        INFO(name);
        CHECK(r.method == Method::Body);
        CHECK(r.body == motion::Body::Swimmer);
        CHECK(r.swimStyle == style);
    }
    // A fish-named character with legs still walks.
    CHECK(routeAnimation(biped(), "fishman").body == motion::Body::Biped);
}
