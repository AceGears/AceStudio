#include "acestudio/core/AssetGen.h"

#include <algorithm>
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
    out += trim(spec.description);
    out += ", ";
    out += typePhrase(spec.type);
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
    if (isSingleSubject(spec.type)) {
        // Measured with SDXL: "plain background" alone was ignored, which
        // breaks framing and background removal.
        out += ", one single subject, centered, entire subject visible with margin around it, isolated on a plain white background";
        if (pixel && backend == Backend::Klein) out += ", game asset";
    }
    if (wantsTileable(spec.type)) out += ", seamless, evenly lit, no perspective, top-down texture";
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
    for (const Issue& i : issues) s -= i.severity == Severity::Problem ? 25 : i.severity == Severity::Warning ? 8 : 0;
    return std::max(0, s);
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

} // namespace acestudio::gen
