#pragma once
// The unified asset generator's model-independent parts: what to ask the
// model for (prompt composition by asset type, style and perspective), how
// hard to work (quality presets), at what size to generate, and the automatic
// checks every result gets before the user sees it.

#include "acestudio/core/Image.h"
#include "acestudio/core/Palette.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace acestudio::gen {

enum class AssetType {
    Character, Npc, Monster, Boss, Pet, Animal, Weapon, Armor, Item, Loot, Building, Furniture, Prop,
    Tileset, Terrain, Background, UiElement, Effect, Portrait, AnimatedObject
};
enum class ArtStyle { DetailedPixel, RpgPixel, PlatformerPixel, RetroPixel, HdPainted, HdCel };
enum class Perspective { Side, Front, ThreeQuarter, TopDown, Isometric };
enum class QualityPreset { FastDraft, Balanced, HighQuality, Maximum };
enum class Backend { Klein, Sdxl };

struct TypeInfo { AssetType type; const char* key; const char* label; };
struct StyleInfo { ArtStyle style; const char* key; const char* label; };
struct PerspectiveInfo { Perspective perspective; const char* key; const char* label; };
const std::vector<TypeInfo>& assetTypes();
const std::vector<StyleInfo>& artStyles();
const std::vector<PerspectiveInfo>& perspectives();
std::optional<AssetType> assetTypeFromKey(std::string_view key);
std::optional<ArtStyle> artStyleFromKey(std::string_view key);
std::optional<Perspective> perspectiveFromKey(std::string_view key);

bool isPixelStyle(ArtStyle s);
// Types drawn as one isolated subject (and cut out when transparency is on);
// the others (tilesets, terrain, backgrounds) fill the frame.
bool isSingleSubject(AssetType t);
// Terrain and tileset textures should repeat seamlessly.
bool wantsTileable(AssetType t);

struct Spec {
    std::string description;      // the user's words
    AssetType type = AssetType::Character;
    ArtStyle style = ArtStyle::DetailedPixel;
    Perspective perspective = Perspective::Side;
    bool transparent = true;
    std::string styleNotes;       // from a locked style profile (palette, outline, shading words)
    // Roles of the reference images sent with the request, in order
    // (style::toString keys: character, style, environment, ...). Each role
    // adds an instruction that limits what the model takes from that image.
    std::vector<std::string> referenceRoles;
    // A sheet of different subjects in one image (bestiary grids); 0 = one subject.
    int sheetColumns = 0, sheetRows = 0;
};

struct Prompt {
    std::string positive;
    std::string negative;         // SDXL only; FLUX.2 klein takes no negative prompt
};
Prompt composePrompt(const Spec& spec, Backend backend, bool styleLora);

struct PresetInfo {
    QualityPreset preset;
    const char* key;
    const char* label;
    const char* description;
    int kleinSteps, sdxlSteps;
    int longSide;                 // generation size of the longer side, px
};
const std::vector<PresetInfo>& qualityPresets();
const PresetInfo& presetInfo(QualityPreset p);
std::optional<QualityPreset> presetFromKey(std::string_view key);

// Generation size for an output of `outW` x `outH`: the target's aspect at
// the preset's long side, rounded to multiples of 16 (SDXL needs 8, klein 16).
std::pair<int, int> generationSize(int outW, int outH, QualityPreset p);

// ------------------------------------------------------------------ checks

enum class Severity { Info, Warning, Problem };
struct Issue {
    Severity severity;
    std::string code;      // stable id: "cropped", "colors", "soft-alpha", "stray", "pieces", "opaque", "empty", "size", "palette", "noise"
    std::string message;
};
struct QualityReport {
    int width = 0, height = 0;
    int colors = 0;               // distinct opaque colours
    double coverage = 0;          // opaque fraction of the canvas
    int pieces = 0;               // 8-connected opaque components larger than 2 px
    double strayRatio = 0;        // opaque pixels with no opaque 4-neighbour, of all opaque
    bool hardAlpha = true;        // every alpha is 0 or 255
    bool touchesEdge = false;     // opaque pixels on the border
    std::optional<double> paletteDistance;   // mean distance to the style palette (0..1), when given
    std::optional<double> neighbourCorrelation;   // brightness of horizontal neighbours, -1..1 (low = noise)
    std::vector<Issue> issues;
    int score() const;            // 100 minus penalties, for sorting variations
};
struct CheckOptions {
    bool pixelArt = true;
    bool expectTransparent = true;
    bool singleSubject = true;
    int expectWidth = 0, expectHeight = 0;   // 0 = any
    int maxColors = 0;                      // 0 = no limit
    const Palette* stylePalette = nullptr;
};
QualityReport checkAsset(const Image& img, const CheckOptions& opt);

// ------------------------------------------------------------------ sheets

// Prompt words for a sheet of `cols` x `rows` different subjects in one image.
std::string gridPrompt(const std::string& subject, int cols, int rows);

struct GridCell {
    int column = 0, row = 0;
    IntRect rect;                 // where it was cut from the sheet
    Image image;                  // cropped to its content, background removed
};
// Cuts a generated sheet into its cells. The cut lines are placed in the
// emptiest column/row near each expected line (models rarely space a grid
// exactly), the background is removed from each cell's border inwards when
// the sheet is opaque, stray specks under `minPixels` are dropped, and empty
// cells are left out.
std::vector<GridCell> extractGrid(const Image& sheet, int cols, int rows, int minPixels = 16);

} // namespace acestudio::gen
