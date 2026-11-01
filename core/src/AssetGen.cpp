#include "acestudio/core/AssetGen.h"

#include "acestudio/core/AnimationQA.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <cmath>
#include <set>
#include <vector>

namespace acestudio::gen {

const std::vector<TypeInfo>& assetTypes() {
    static const std::vector<TypeInfo> k = {
        {AssetType::Character, "character", "Character"},     {AssetType::Npc, "npc", "NPC"},
        {AssetType::Monster, "monster", "Monster"},           {AssetType::Boss, "boss", "Boss"},
        {AssetType::Pet, "pet", "Pet"},                       {AssetType::Animal, "animal", "Animal"},
        {AssetType::Weapon, "weapon", "Weapon"},              {AssetType::Armor, "armor", "Armor"},
        {AssetType::Item, "item", "Inventory item"},          {AssetType::Loot, "loot", "Loot"},
        {AssetType::Building, "building", "Building"},        {AssetType::Furniture, "furniture", "Furniture"},
        {AssetType::Prop, "prop", "Environment prop"},        {AssetType::Tileset, "tileset", "Tileset texture"},
        {AssetType::Terrain, "terrain", "Terrain texture"},   {AssetType::Background, "background", "Background"},
        {AssetType::UiElement, "ui", "UI element"},           {AssetType::Effect, "effect", "Effect"},
        {AssetType::Portrait, "portrait", "Portrait"},        {AssetType::AnimatedObject, "object", "Animated object"},
    };
    return k;
}

const std::vector<StyleInfo>& artStyles() {
    static const std::vector<StyleInfo> k = {
        {ArtStyle::DetailedPixel, "detailed-pixel", "Detailed fantasy pixel art"},
        {ArtStyle::RpgPixel, "rpg-pixel", "Stylized RPG pixel art"},
        {ArtStyle::PlatformerPixel, "platformer-pixel", "Vibrant platformer pixel art"},
        {ArtStyle::RetroPixel, "retro-pixel", "Retro 8-bit pixel art"},
        {ArtStyle::HdPainted, "hd-painted", "HD 2D, painted"},
        {ArtStyle::HdCel, "hd-cel", "HD 2D, cel-shaded"},
    };
    return k;
}

const std::vector<PerspectiveInfo>& perspectives() {
    static const std::vector<PerspectiveInfo> k = {
        {Perspective::Side, "side", "Side view"},
        {Perspective::Front, "front", "Front view"},
        {Perspective::ThreeQuarter, "three-quarter", "Three-quarter view"},
        {Perspective::TopDown, "top-down", "Top-down"},
        {Perspective::Isometric, "isometric", "Isometric"},
    };
    return k;
}

std::optional<AssetType> assetTypeFromKey(std::string_view key) {
    for (const auto& t : assetTypes())
        if (key == t.key) return t.type;
    return std::nullopt;
}
std::optional<ArtStyle> artStyleFromKey(std::string_view key) {
    for (const auto& s : artStyles())
        if (key == s.key) return s.style;
    return std::nullopt;
}
std::optional<Perspective> perspectiveFromKey(std::string_view key) {
    for (const auto& p : perspectives())
        if (key == p.key) return p.perspective;
    return std::nullopt;
}

bool isPixelStyle(ArtStyle s) { return s != ArtStyle::HdPainted && s != ArtStyle::HdCel; }

bool isSingleSubject(AssetType t) {
    return t != AssetType::Tileset && t != AssetType::Terrain && t != AssetType::Background;
}

bool wantsTileable(AssetType t) { return t == AssetType::Tileset || t == AssetType::Terrain; }

namespace {

const char* typePhrase(AssetType t) {
    switch (t) {
    case AssetType::Character: return "full-body game character";
    case AssetType::Npc: return "full-body non-player character";
    case AssetType::Monster: return "full-body monster creature";
    case AssetType::Boss: return "large imposing boss creature, full body";
    case AssetType::Pet: return "small companion pet creature, full body";
    case AssetType::Animal: return "animal, full body";
    case AssetType::Weapon: return "single weapon, game item icon";
    case AssetType::Armor: return "single armor piece, game item icon";
    case AssetType::Item: return "single inventory item icon";
    case AssetType::Loot: return "single treasure loot item";
    case AssetType::Building: return "single building exterior";
    case AssetType::Furniture: return "single piece of furniture";
    case AssetType::Prop: return "single environment prop";
    case AssetType::Tileset: return "seamless tileable texture for a tileset";
    case AssetType::Terrain: return "seamless tileable terrain texture";
    case AssetType::Background: return "wide game background scene";
    case AssetType::UiElement: return "game user interface element";
    case AssetType::Effect: return "visual effect";
    case AssetType::Portrait: return "character portrait, head and shoulders";
    case AssetType::AnimatedObject: return "single interactive game object";
    }
    return "game asset";
}

const char* stylePhrase(ArtStyle s) {
    switch (s) {
    case ArtStyle::DetailedPixel:
        return "detailed fantasy pixel art, strong silhouette, clean pixel clusters, deliberate colour ramps, rich material "
               "shading, controlled highlights, professional lighting";
    case ArtStyle::RpgPixel: return "stylized 16-bit RPG pixel art, readable shapes, limited palette, clean dark outline";
    case ArtStyle::PlatformerPixel: return "vibrant side-scrolling platformer pixel art, bright saturated colours, crisp outline";
    case ArtStyle::RetroPixel: return "8-bit retro pixel art, very limited palette, chunky pixels";
    case ArtStyle::HdPainted: return "high-definition hand-painted 2D game art, detailed rendering, soft lighting";
    case ArtStyle::HdCel: return "clean cel-shaded 2D game art, bold line art, flat colours with soft shading";
    }
    return "";
}

const char* perspectivePhrase(Perspective p) {
    switch (p) {
    case Perspective::Side: return "side view";
    case Perspective::Front: return "front view";
    case Perspective::ThreeQuarter: return "three-quarter view";
    case Perspective::TopDown: return "top-down view";
    case Perspective::Isometric: return "isometric view";
    }
    return "";
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == ',' || s.back() == '.' || s.back() == '\n')) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\n')) ++i;
    return s.substr(i);
}

} // namespace

Prompt composePrompt(const Spec& spec, Backend backend, bool styleLora) {
    Prompt p;
    std::string out;
    const bool pixel = isPixelStyle(spec.style);
    // The pixel-art LoRAs are trained on these trigger words.
    if (pixel && styleLora) out += backend == Backend::Klein ? "pixel art sprite, " : "pixel art, ";
    const bool sheet = spec.sheetColumns > 0 && spec.sheetRows > 0;
    if (sheet) {
        out += gridPrompt(trim(spec.description), spec.sheetColumns, spec.sheetRows);
    } else {
        out += trim(spec.description);
        out += ", ";
        out += typePhrase(spec.type);
    }
    if (spec.type != AssetType::Background && spec.type != AssetType::Terrain && spec.type != AssetType::Tileset) {
        out += ", ";
        out += perspectivePhrase(spec.perspective);
    }
    out += ", ";
    out += stylePhrase(spec.style);
    if (!spec.styleNotes.empty()) {
        out += ", ";
        out += trim(spec.styleNotes);
    }
    if (sheet) {
        out += ", on a plain white background";
    } else if (isSingleSubject(spec.type)) {
        // Measured with SDXL: "plain background" alone was ignored, which
        // breaks framing and background removal.
        out += ", one single subject, centered, entire subject visible with margin around it, isolated on a plain white background";
        if (pixel && backend == Backend::Klein) out += ", game asset";
    }
    if (wantsTileable(spec.type)) out += ", seamless, evenly lit, no perspective, top-down texture";
    if (backend == Backend::Klein)
        for (std::size_t i = 0; i < spec.referenceRoles.size(); ++i) {
            const std::string n = std::to_string(i + 1);
            const std::string& role = spec.referenceRoles[i];
            if (role == "character")
                out += ". Keep the character from reference image " + n +
                       " exactly: same face, hair, body proportions, outfit, colours and equipment";
            else if (role == "creature")
                out += ". The same creature as reference image " + n + ": same anatomy, markings and colours";
            else if (role == "equipment")
                out += ". The same item design as reference image " + n;
            else if (role == "environment")
                out += ". Match the environment, materials and lighting of reference image " + n;
            else if (role == "animation")
                out += ". In the pose shown in reference image " + n;
            else if (role == "composition")
                out += ". Use the composition and framing of reference image " + n;
            else   // style
                out += ". Drawn in the art style of reference image " + n +
                       " (rendering, outline, shading, palette), but with a different subject; do not copy its subject";
        }
    p.positive = out;
    if (backend == Backend::Sdxl) {
        p.negative = "blurry, jpeg artifacts, text, watermark, signature, deformed, extra limbs";
        if (isSingleSubject(spec.type))
            p.negative += ", sprite sheet, multiple objects, collage, grid, cropped, cut off, floor, ground, scenery, background texture";
        if (pixel) p.negative += ", anti-aliasing, gradient banding, photo";
    }
    return p;
}

const std::vector<PresetInfo>& qualityPresets() {
    static const std::vector<PresetInfo> k = {
        {QualityPreset::FastDraft, "fast", "Fast Draft", "Fewest steps at a reduced size: for trying ideas.", 4, 18, 768},
        {QualityPreset::Balanced, "balanced", "Balanced", "The model's standard settings.", 4, 30, 1024},
        {QualityPreset::HighQuality, "high", "High Quality", "More steps: finer detail and fewer artefacts, slower.", 6, 40, 1024},
        {QualityPreset::Maximum, "max", "Maximum Quality", "Most steps; the slowest. Best for final assets.", 8, 50, 1024},
    };
    return k;
}

const PresetInfo& presetInfo(QualityPreset p) {
    for (const auto& i : qualityPresets())
        if (i.preset == p) return i;
    return qualityPresets()[1];
}

std::optional<QualityPreset> presetFromKey(std::string_view key) {
    for (const auto& i : qualityPresets())
        if (key == i.key) return i.preset;
    return std::nullopt;
}

std::pair<int, int> generationSize(int outW, int outH, QualityPreset preset) {
    const int longSide = presetInfo(preset).longSide;
    outW = std::max(1, outW);
    outH = std::max(1, outH);
    double aspect = static_cast<double>(outW) / outH;
    aspect = std::clamp(aspect, 0.25, 4.0);   // keep both sides within the models' range
    int w, h;
    if (aspect >= 1) {
        w = longSide;
        h = static_cast<int>(std::lround(longSide / aspect));
    } else {
        h = longSide;
        w = static_cast<int>(std::lround(longSide * aspect));
    }
    auto r16 = [](int v) { return std::max(256, (v + 8) / 16 * 16); };
    return {r16(w), r16(h)};
}

// ------------------------------------------------------------------ checks

int QualityReport::score() const {
    int s = 100;
    bool unusable = false;
    for (const Issue& i : issues) {
        s -= i.severity == Severity::Problem ? 25 : i.severity == Severity::Warning ? 8 : 0;
        // Nothing usable at all: rank below every real result, however few other issues.
        unusable = unusable || i.code == "empty" || i.code == "noise";
    }
    return unusable ? 0 : std::max(0, s);
}

QualityReport checkAsset(const Image& img, const CheckOptions& opt) {
    QualityReport r;
    r.width = img.width();
    r.height = img.height();
    const int W = img.width(), H = img.height();
    auto add = [&](Severity s, const char* code, std::string msg) { r.issues.push_back({s, code, std::move(msg)}); };
    if (W == 0 || H == 0) {
        add(Severity::Problem, "empty", "The image is empty.");
        return r;
    }
    if ((opt.expectWidth && opt.expectWidth != W) || (opt.expectHeight && opt.expectHeight != H))
        add(Severity::Problem, "size",
            "Size is " + std::to_string(W) + "×" + std::to_string(H) + ", expected " + std::to_string(opt.expectWidth) + "×" +
                std::to_string(opt.expectHeight) + ".");

    const auto idx = [W](int x, int y) { return static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x); };
    std::vector<std::uint8_t> opaque(static_cast<std::size_t>(W) * static_cast<std::size_t>(H));
    std::set<std::uint32_t> colours;
    long nOpaque = 0, nSoft = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const Rgba8 p = img.pixel(x, y);
            if (p.a != 0 && p.a != 255) ++nSoft;
            if (p.a >= 128) {
                opaque[idx(x, y)] = 1;
                ++nOpaque;
                colours.insert((static_cast<std::uint32_t>(p.r) << 16) | (p.g << 8) | p.b);
                if (x == 0 || y == 0 || x == W - 1 || y == H - 1) r.touchesEdge = true;
            }
        }
    r.colors = static_cast<int>(colours.size());
    // Noise: in real images (pixel art or painted) a pixel's brightness
    // predicts its neighbour's; in a failed generation it does not.
    {
        double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
        long n = 0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x + 1 < W; ++x) {
                if (!opaque[idx(x, y)] || !opaque[idx(x + 1, y)]) continue;
                const double a = img.pixel(x, y).luma(), b = img.pixel(x + 1, y).luma();
                sx += a; sy += b; sxx += a * a; syy += b * b; sxy += a * b;
                ++n;
            }
        if (n > 64) {
            const double nn = static_cast<double>(n);
            const double vx = sxx / nn - (sx / nn) * (sx / nn), vy = syy / nn - (sy / nn) * (sy / nn);
            if (vx > 4 && vy > 4) {
                r.neighbourCorrelation = (sxy / nn - (sx / nn) * (sy / nn)) / std::sqrt(vx * vy);
                if (*r.neighbourCorrelation < 0.3)
                    add(Severity::Problem, "noise", "Looks like noise, not a picture: the generation probably failed.");
            }
        }
    }
    r.coverage = static_cast<double>(nOpaque) / (static_cast<double>(W) * static_cast<double>(H));
    r.hardAlpha = nSoft == 0;

    if (nOpaque == 0) {
        add(Severity::Problem, "empty", "Nothing visible: the whole image is transparent.");
        return r;
    }
    // Stray pixels and pieces (8-connected components).
    long stray = 0;
    auto at = [&](int x, int y) { return x >= 0 && y >= 0 && x < W && y < H && opaque[idx(x, y)]; };
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (at(x, y) && !at(x - 1, y) && !at(x + 1, y) && !at(x, y - 1) && !at(x, y + 1)) ++stray;
    r.strayRatio = static_cast<double>(stray) / static_cast<double>(nOpaque);
    std::vector<int> label(opaque.size(), 0);
    std::vector<std::pair<int, int>> stack;
    int pieces = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            if (!at(x, y) || label[idx(x, y)]) continue;
            int size = 0;
            stack.push_back({x, y});
            label[idx(x, y)] = 1;
            while (!stack.empty()) {
                auto [cx, cy] = stack.back();
                stack.pop_back();
                ++size;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = cx + dx, ny = cy + dy;
                        if (at(nx, ny) && !label[idx(nx, ny)]) {
                            label[idx(nx, ny)] = 1;
                            stack.push_back({nx, ny});
                        }
                    }
            }
            if (size > 2) ++pieces;
        }
    r.pieces = pieces;

    if (opt.expectTransparent && opt.singleSubject) {
        if (r.coverage > 0.97) add(Severity::Problem, "opaque", "No transparent background: the subject was not cut out.");
        else if (r.touchesEdge) add(Severity::Warning, "cropped", "The subject touches the edge: parts may be cut off.");
        if (r.coverage < 0.04) add(Severity::Warning, "empty", "The subject is very small in the frame.");
    }
    if (opt.pixelArt) {
        if (!r.hardAlpha)
            add(Severity::Warning, "soft-alpha",
                std::to_string(nSoft) + " semi-transparent pixels: pixel art usually needs fully opaque or fully clear pixels.");
        if (r.strayRatio > 0.03)
            add(Severity::Warning, "stray",
                std::to_string(static_cast<int>(std::lround(r.strayRatio * 100))) + "% isolated stray pixels (noise).");
        if (opt.singleSubject && pieces > 3)
            add(Severity::Warning, "pieces", "The sprite is split into " + std::to_string(pieces) + " separate pieces.");
        if (opt.maxColors > 0 && r.colors > opt.maxColors)
            add(Severity::Warning, "colors",
                std::to_string(r.colors) + " colours, more than the " + std::to_string(opt.maxColors) + " allowed.");
    }
    if (opt.stylePalette && opt.stylePalette->size() > 0) {
        // Mean distance of each colour used to the nearest style colour (RGB, 0..1).
        double sum = 0;
        for (std::uint32_t c : colours) {
            const Rgba8 p{static_cast<std::uint8_t>(c >> 16), static_cast<std::uint8_t>(c >> 8), static_cast<std::uint8_t>(c), 255};
            double best = 1e9;
            for (std::size_t i = 0; i < opt.stylePalette->size(); ++i) {
                const Rgba8 q = opt.stylePalette->at(i);
                const double d = std::sqrt(std::pow(p.r - q.r, 2) + std::pow(p.g - q.g, 2) + std::pow(p.b - q.b, 2)) / 441.673;
                best = std::min(best, d);
            }
            sum += best;
        }
        r.paletteDistance = sum / static_cast<double>(colours.size());
        if (*r.paletteDistance > 0.08)
            add(Severity::Warning, "palette", "Colours drift from the style palette (mean distance " +
                                                   std::to_string(static_cast<int>(std::lround(*r.paletteDistance * 100))) + "%).");
    }
    return r;
}

// ------------------------------------------------------------------ sheets

std::string gridPrompt(const std::string& subject, int cols, int rows) {
    return "a character sheet of " + std::to_string(cols * rows) + " different " + subject + ", arranged in a grid of " +
           std::to_string(cols) + " columns and " + std::to_string(rows) +
           " rows, every one a different species with its own body shape, size and colours (no two alike), each one complete"
           " and separate with empty space around it, all the same scale, plain flat background";
}

std::string bestiaryKinds(const std::string& subject, int index, int count) {
    std::string lower;
    for (char ch : subject) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    bool generic = false;
    for (const char* w : {"monster", "creature", "enem", "bestiary", "beast", "mob"})
        generic = generic || lower.find(w) != std::string::npos;
    if (!generic || count <= 0) return {};
    // Generic kinds of fantasy creature (no named franchise monsters).
    static const char* const kinds[] = {
        "slime", "skeleton warrior", "giant bat", "cave spider", "goblin", "orc brute", "wraith", "treasure-chest mimic",
        "mushroom creature", "fire elemental", "stone golem", "harpy", "lizard warrior", "dire wolf", "ghost", "imp",
        "floating eyeball", "walking tree", "giant scorpion", "wyvern", "kobold", "zombie", "lich", "gargoyle",
        "bull-headed brute", "one-eyed giant", "giant crab", "plague rat", "sea serpent", "basilisk lizard", "troll", "ogre",
        "snow yeti", "salamander", "fire bird", "griffin", "banshee", "giant beetle", "living armour", "jellyfish",
        "werewolf", "vampire bat lord", "sand worm", "ice elemental", "toad demon", "bone dragon", "crystal golem",
        "swamp hag", "cactus creature", "clockwork knight", "shadow cat", "giant snail", "moth spirit", "boar beast",
        "will-o'-the-wisp", "centipede", "rock crab", "frog warrior", "storm eagle", "lava slug"};
    constexpr int n = static_cast<int>(sizeof(kinds) / sizeof(kinds[0]));
    std::string out;
    for (int i = 0; i < count; ++i) {
        if (i) out += ", ";
        out += kinds[(index * count + i) % n];
    }
    return out;
}

std::vector<GridCell> extractGrid(const Image& sheet, int cols, int rows, int minPixels) {
    std::vector<GridCell> out;
    if (sheet.isNull() || cols < 1 || rows < 1 || cols > 16 || rows > 16) return out;
    // Is the sheet already cut out? Otherwise treat the border colour as background.
    bool transparent = false;
    for (const Rgba8& c : sheet.pixels())
        if (c.a < 255) { transparent = true; break; }
    const Image clean = transparent ? sheet : removeBorderBackground(sheet, 28);
    const int W = clean.width(), H = clean.height();
    std::vector<int> colInk(static_cast<std::size_t>(W), 0), rowInk(static_cast<std::size_t>(H), 0);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (clean.pixel(x, y).a > 0) { ++colInk[static_cast<std::size_t>(x)]; ++rowInk[static_cast<std::size_t>(y)]; }
    auto cuts = [](const std::vector<int>& ink, int n) {
        const int len = static_cast<int>(ink.size());
        std::vector<int> c{0};
        for (int i = 1; i < n; ++i) {
            const int expected = len * i / n;
            const int window = std::max(1, len / n * 15 / 100);
            int best = expected, bestInk = std::numeric_limits<int>::max(), bestDist = 0;
            for (int p = std::max(c.back() + 1, expected - window); p <= std::min(len - 1, expected + window); ++p) {
                const int v = ink[static_cast<std::size_t>(p)], d = std::abs(p - expected);
                if (v < bestInk || (v == bestInk && d < bestDist)) { best = p; bestInk = v; bestDist = d; }
            }
            c.push_back(best);
        }
        c.push_back(len);
        return c;
    };
    const std::vector<int> xs = cuts(colInk, cols), ys = cuts(rowInk, rows);
    auto cellOf = [](const std::vector<int>& c, int v) {
        int i = 0;
        while (i + 2 < static_cast<int>(c.size()) && v >= c[static_cast<std::size_t>(i) + 1]) ++i;
        return i;
    };

    // Subjects are found in the whole sheet, not per cell: models often ignore
    // the grid (on the test PC a 7 x 7 request came back as fewer, larger
    // creatures across the cell lines, and cutting fixed cells gave heads and
    // feet). The grid only splits a subject that clearly spans several cells
    // (creatures drawn touching each other).
    const Mask sil = silhouette(clean);
    std::vector<int> label(static_cast<std::size_t>(W) * static_cast<std::size_t>(H), -1);
    std::vector<int> sizes;
    std::vector<IntRect> box;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x);
            if (!sil.get(x, y) || label[i] >= 0) continue;
            const int id = static_cast<int>(sizes.size());
            sizes.push_back(0);
            IntRect b{x, y, 1, 1};
            std::vector<std::pair<int, int>> stack{{x, y}};
            label[i] = id;
            while (!stack.empty()) {
                auto [px, py] = stack.back();
                stack.pop_back();
                ++sizes.back();
                const int x1 = std::max(b.x + b.w, px + 1), y1 = std::max(b.y + b.h, py + 1);
                b.x = std::min(b.x, px); b.y = std::min(b.y, py); b.w = x1 - b.x; b.h = y1 - b.y;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = px + dx, ny = py + dy;
                        if (nx < 0 || ny < 0 || nx >= W || ny >= H || !sil.get(nx, ny)) continue;
                        const std::size_t j = static_cast<std::size_t>(ny) * static_cast<std::size_t>(W) + static_cast<std::size_t>(nx);
                        if (label[j] < 0) { label[j] = id; stack.push_back({nx, ny}); }
                    }
            }
            box.push_back(b);
        }
    if (sizes.empty()) return out;
    const double cellW = static_cast<double>(W) / cols, cellH = static_cast<double>(H) / rows;
    // A subject covers at least 3 % of a cell; smaller pieces (a detached horn,
    // a tail tip) join the nearest subject when close, else are dropped.
    const int subjectMin = std::max(minPixels, static_cast<int>(cellW * cellH * 0.03));
    const long joinDist = static_cast<long>(std::max(cellW, cellH) * 0.15);
    std::vector<int> owner(sizes.size(), -1);
    std::vector<int> subjects;
    for (std::size_t i = 0; i < sizes.size(); ++i)
        if (sizes[i] >= subjectMin) { owner[i] = static_cast<int>(subjects.size()); subjects.push_back(static_cast<int>(i)); }
    if (subjects.empty()) return out;
    std::vector<IntRect> sbox(subjects.size());
    for (std::size_t k = 0; k < subjects.size(); ++k) sbox[k] = box[static_cast<std::size_t>(subjects[k])];
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        if (owner[i] >= 0 || sizes[i] < std::max(2, minPixels / 4)) continue;
        long best = std::numeric_limits<long>::max();
        int to = -1;
        for (std::size_t k = 0; k < subjects.size(); ++k) {
            const IntRect& a = box[i];
            const IntRect& b = sbox[k];
            const long dx = std::max({0, b.x - (a.x + a.w), a.x - (b.x + b.w)});
            const long dy = std::max({0, b.y - (a.y + a.h), a.y - (b.y + b.h)});
            if (dx * dx + dy * dy < best) { best = dx * dx + dy * dy; to = static_cast<int>(k); }
        }
        if (to >= 0 && best <= joinDist * joinDist) owner[i] = to;
    }
    struct Piece { int col, row; IntRect rect; Image image; };
    std::vector<Piece> pieces;
    auto emit = [&](int k, const IntRect& area) {
        Image part(area.w, area.h);
        for (int y = area.y; y < area.y + area.h; ++y)
            for (int x = area.x; x < area.x + area.w; ++x) {
                const int l = label[static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x)];
                if (l >= 0 && owner[static_cast<std::size_t>(l)] == k) part.setPixel(x - area.x, y - area.y, clean.pixel(x, y));
            }
        const IntRect b = part.opaqueBounds();
        if (b.empty()) return;
        int n = 0;
        for (const Rgba8& c : part.pixels()) n += c.a > 0;
        if (n < subjectMin) return;
        const IntRect r{area.x + b.x, area.y + b.y, b.w, b.h};
        pieces.push_back({cellOf(xs, r.x + r.w / 2), cellOf(ys, r.y + r.h / 2), r, part.copy(b)});
    };
    for (std::size_t k = 0; k < subjects.size(); ++k) {
        // The subject's extent with the pieces that joined it.
        IntRect ext = sbox[k];
        for (std::size_t i = 0; i < sizes.size(); ++i)
            if (owner[i] == static_cast<int>(k)) {
                const IntRect& b = box[i];
                const int x1 = std::max(ext.x + ext.w, b.x + b.w), y1 = std::max(ext.y + ext.h, b.y + b.h);
                ext.x = std::min(ext.x, b.x); ext.y = std::min(ext.y, b.y); ext.w = x1 - ext.x; ext.h = y1 - ext.y;
            }
        const bool wide = ext.w > cellW * 1.6, tall = ext.h > cellH * 1.6;
        if (!wide && !tall) {
            emit(static_cast<int>(k), ext);
            continue;
        }
        // Spans several cells: cut it along the grid lines it crosses.
        const int c0 = wide ? cellOf(xs, ext.x) : 0, c1 = wide ? cellOf(xs, ext.x + ext.w - 1) : 0;
        const int r0 = tall ? cellOf(ys, ext.y) : 0, r1 = tall ? cellOf(ys, ext.y + ext.h - 1) : 0;
        for (int r = r0; r <= r1; ++r)
            for (int c = c0; c <= c1; ++c) {
                const int ax0 = wide ? std::max(ext.x, xs[static_cast<std::size_t>(c)]) : ext.x;
                const int ax1 = wide ? std::min(ext.x + ext.w, xs[static_cast<std::size_t>(c) + 1]) : ext.x + ext.w;
                const int ay0 = tall ? std::max(ext.y, ys[static_cast<std::size_t>(r)]) : ext.y;
                const int ay1 = tall ? std::min(ext.y + ext.h, ys[static_cast<std::size_t>(r) + 1]) : ext.y + ext.h;
                if (ax1 > ax0 && ay1 > ay0) emit(static_cast<int>(k), {ax0, ay0, ax1 - ax0, ay1 - ay0});
            }
    }
    // Reading order: by row, then left to right.
    std::stable_sort(pieces.begin(), pieces.end(), [](const Piece& a, const Piece& b) {
        return a.row != b.row ? a.row < b.row : a.rect.x + a.rect.w / 2 < b.rect.x + b.rect.w / 2;
    });
    for (Piece& p : pieces) out.push_back({p.col, p.row, p.rect, std::move(p.image)});
    return out;
}

} // namespace acestudio::gen
