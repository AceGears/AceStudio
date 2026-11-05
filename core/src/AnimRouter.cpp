#include "acestudio/core/AnimRouter.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>

namespace acestudio::anim {

const char* toString(Method m) noexcept {
    switch (m) {
    case Method::Body: return "body";
    case Method::Effect: return "effect";
    case Method::Ai: return "ai";
    }
    return "body";
}

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// Whole-word-ish match: the word starts at the beginning or after a non-letter.
bool hasWord(const std::string& text, std::initializer_list<const char*> words) {
    for (const char* w : words) {
        const std::string_view word(w);
        for (std::size_t pos = text.find(word); pos != std::string::npos; pos = text.find(word, pos + 1))
            if (pos == 0 || !std::isalpha(static_cast<unsigned char>(text[pos - 1]))) return true;
    }
    return false;
}

std::size_t count(const Mask& m) {
    std::size_t n = 0;
    for (int y = 0; y < m.height(); ++y)
        for (int x = 0; x < m.width(); ++x) n += m.get(x, y) ? 1u : 0u;
    return n;
}

std::size_t opaque(const Image& img) {
    std::size_t n = 0;
    for (const Rgba8& c : img.pixels()) n += c.a > 0 ? 1 : 0;
    return n;
}

Route effect(fx::Effect e, motion::Body body, std::string why) {
    Route r;
    r.method = Method::Effect;
    r.effect = e;
    r.body = body;
    r.reason = std::move(why);
    return r;
}

} // namespace

Route routeAnimation(const Image& sprite, std::string_view hint, motion::Action action) {
    Route r;
    const motion::Subject subj = motion::analyzeSubject(sprite);
    r.body = subj.body;
    const std::string h = lower(hint);
    // Water creatures by name: the silhouette alone often reads a fish's
    // fins as feet. The swimming style follows the kind of animal.
    using SS = motion::SwimStyle;
    SS swim = SS::Auto;
    if (hasWord(h, {"eel", "moray", "lamprey", "hagfish", "sea snake", "seasnake", "leech", "serpent"})) swim = SS::Anguilliform;
    else if (hasWord(h, {"tuna", "mackerel", "shark", "dolphin", "porpoise", "swordfish", "marlin", "sailfish", "orca", "bonito"}))
        swim = SS::Thunniform;
    else if (hasWord(h, {"bream", "perch", "piranha", "pompano", "sunfish", "bluegill", "angelfish", "tang", "discus", "butterflyfish",
                         "pufferfish", "puffer"}))
        swim = SS::Carangiform;
    else if (hasWord(h, {"fish", "goldfish", "trout", "salmon", "carp", "koi", "bass", "cod", "pike", "catfish", "minnow", "herring",
                         "guppy", "sardine", "anchovy", "clownfish", "betta", "snapper", "grouper", "barracuda"}))
        swim = SS::Subcarangiform;
    if (swim != SS::Auto && r.body != motion::Body::Biped) {
        r.body = motion::Body::Swimmer;
        r.swimStyle = swim;
        r.method = Method::Body;
        static const char* kWhy[] = {"", "An eel-like swimmer: the whole body undulates, head to tail.",
                                     "A fish: the wave runs through the rear half of the body, the tail fin trailing.",
                                     "A deep-bodied fish: only the rear third bends, the head stays steady.",
                                     "A fast swimmer: the body stays stiff and the tail beats quickly."};
        r.reason = kWhy[static_cast<int>(swim)];
        return r;
    }
    // Living things move their bodies even when their name mentions fire
    // ("fire drake") or water ("water spirit").
    const bool creature = hasWord(h, {"dragon", "drake", "knight", "warrior", "rogue", "mage", "wizard", "troll", "goblin", "orc",
                                      "slime", "wolf", "bird", "fish", "spirit", "elemental", "golem", "hero", "character", "monster",
                                      "beast", "cat", "dog", "horse", "bat", "spider", "insect", "bee", "butterfly"});
    if (!creature) {
        if (hasWord(h, {"fire", "flame", "torch", "campfire", "candle", "brazier", "bonfire", "lava", "ember", "furnace", "forge",
                        "inferno", "blaze", "burning", "hearth", "fireplace"})) {
            Route f = effect(fx::Effect::Fire, r.body, "Fire: flames rise, flicker and break up in the sprite's own colours; the logs stay still.");
            if (hasWord(h, {"torch", "candle", "lantern", "sconce"})) f.fireStyle = fx::FireStyle::Torch;
            else if (hasWord(h, {"furnace", "forge", "kiln", "smelter", "brazier"})) f.fireStyle = fx::FireStyle::Furnace;
            else if (hasWord(h, {"inferno", "blaze", "burning", "bonfire", "wildfire", "raging"})) f.fireStyle = fx::FireStyle::Raging;
            else if (hasWord(h, {"magic", "arcane", "spell", "ghost", "spirit", "cursed", "fel", "soul"})) f.fireStyle = fx::FireStyle::Magic;
            return f;
        }
        if (hasWord(h, {"water", "waterfall", "river", "lake", "pond", "fountain", "stream", "sea", "ocean", "wave"}))
            return effect(fx::Effect::Water, r.body, "Water: the surface ripples and shimmers in a seamless loop.");
        if (hasWord(h, {"flag", "banner", "pennant", "standard", "cape", "curtain", "cloth"}))
            return effect(fx::Effect::Flag, r.body, "Cloth: a wave runs from the pole through the cloth.");
        if (hasWord(h, {"smoke", "steam", "fog", "mist", "chimney", "cloud"}))
            return effect(fx::Effect::Smoke, r.body, "Smoke: it rises, drifts and wraps around seamlessly.");
        if (hasWord(h, {"tree", "bush", "grass", "plant", "flower", "reed", "vine", "palm", "fern", "shrub", "crop", "wheat"}))
            return effect(fx::Effect::Sway, r.body, "Plant: the top sways in the wind while the base stays rooted.");
        if (hasWord(h, {"crystal", "orb", "gem", "rune", "portal", "magic", "glow", "aura", "spell", "potion", "lantern", "lamp"}))
            return effect(fx::Effect::Pulse, r.body, "Magic: it pulses and glows gently.");
    }
    // New poses cannot be made by moving the drawn parts.
    if (action == motion::Action::Attack2 || action == motion::Action::Cast || action == motion::Action::Climb) {
        if (subj.body == motion::Body::Biped || subj.body == motion::Body::Quadruped) {
            r.method = Method::Ai;
            r.reason = "This action needs poses that are not in the drawing; the AI animation pipeline draws them "
                       "from the sprite (body motion is a quick preview).";
            return r;
        }
    }
    // No hint: the pixels. Warm colours alone are not enough (a red potion
    // is 54 % "flame-coloured"; a coin pouch has a bright yellow pile). A
    // fire has a bright yellow core that is taller than it is wide: measured
    // on generated sprites from the test PC, campfire cores 2.3-2.9 times
    // taller than wide, coin piles 0.4-0.5.
    if (!creature && subj.body != motion::Body::Biped && subj.body != motion::Body::Quadruped) {
        const std::size_t all = opaque(sprite);
        const std::size_t hot = count(fx::autoRegion(sprite, fx::Effect::Fire));
        std::size_t core = 0;
        int cx0 = sprite.width(), cy0 = sprite.height(), cx1 = -1, cy1 = -1;
        for (int y = 0; y < sprite.height(); ++y)
            for (int x = 0; x < sprite.width(); ++x) {
                const Rgba8 c = sprite.pixel(x, y);
                if (c.a == 0 || c.r < 225 || c.g < 170 || c.b > 140) continue;
                ++core;
                cx0 = std::min(cx0, x); cx1 = std::max(cx1, x);
                cy0 = std::min(cy0, y); cy1 = std::max(cy1, y);
            }
        const bool tallCore = cx1 >= cx0 && (cy1 - cy0 + 1) * 2 >= (cx1 - cx0 + 1) * 3;
        if (all > 0 && hot * 10 >= all && core * 50 >= all && tallCore)
            return effect(fx::Effect::Fire, r.body, "It has flames (a bright core rising from warm colours), so it is animated as fire.");
    }
    r.method = Method::Body;
    switch (subj.body) {
    case motion::Body::Biped: r.reason = "Two legs: walk and run cycles with planted feet and swinging arms."; break;
    case motion::Body::Quadruped: r.reason = "Four legs: the legs move in diagonal pairs."; break;
    case motion::Body::Swimmer: r.reason = "A swimmer: a wave runs through the body from head to tail, the tail most."; break;
    case motion::Body::Flyer: r.reason = "A flyer: it hovers; Flap folds the wings above the wing line."; break;
    case motion::Body::Blob: r.reason = "No legs: it hops and squashes."; break;
    case motion::Body::Object:
        r.reason = "An object: it rocks gently. For fire, water, plants or flags, choose that effect.";
        break;
    }
    return r;
}

} // namespace acestudio::anim
