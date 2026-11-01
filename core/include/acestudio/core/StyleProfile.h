#pragma once
// Style profiles: a reusable visual identity built from example images.
//
// Measured from the references (deterministic, no AI):
//   palette, pixel scale (how many screen pixels one art pixel spans), outline
//   treatment, shading depth (tones per hue), light direction, saturation,
//   brightness and detail level.
// Chosen by the user: perspective and free-form notes (shape language etc.).
//
// A profile drives generation three ways: its reference images condition the
// model (FLUX.2 klein), its words go into the prompt, and its palette
// constrains the pixel reduction. Results are then compared with it
// (styleSimilarity), so "Lock Game Style" is checked, not assumed.

#include "acestudio/core/Image.h"
#include "acestudio/core/Palette.h"
#include "acestudio/core/Result.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace acestudio::style {

// What a reference image is for. Each role influences only its own aspect.
enum class RefRole { Character, Style, Palette, Environment, Creature, Equipment, Animation, Composition };
const char* toString(RefRole r);
std::optional<RefRole> refRoleFromString(std::string_view s);
const char* roleLabel(RefRole r);

enum class Outline { None, Dark, Coloured };
enum class Shading { Flat, Cel, Soft };

struct StyleFeatures {
    Palette palette;              // up to 32 colours
    int pixelScale = 1;           // 1 = native pixel art; >1 = upscaled art
    Outline outline = Outline::None;
    double outlineRatio = 0;      // share of edge pixels that are outline-dark
    Shading shading = Shading::Flat;
    double tonesPerHue = 0;       // average distinct lightness levels per hue band
    double lightX = 0, lightY = 0;   // direction the light comes FROM (-1..1; y < 0 = from above)
    double saturation = 0;        // mean HSV saturation of opaque pixels, 0..1
    double value = 0;             // mean HSV value, 0..1
    double detail = 0;            // edges per opaque pixel, 0..1
    int sampleCount = 0;          // reference images measured
    nlohmann::json toJson() const;
    static StyleFeatures fromJson(const nlohmann::json& j);
};

// Measures one image (references are measured one by one and merged).
StyleFeatures analyze(const Image& img);
StyleFeatures merge(const std::vector<StyleFeatures>& parts);
// Words for a prompt ("dark 1-pixel outline, cel shading with 3 tones, light from the top left, muted colours").
std::string describe(const StyleFeatures& f);
// 0..1: how close a result is to the profile (palette, outline, shading, saturation, value).
double styleSimilarity(const StyleFeatures& profile, const StyleFeatures& result);

struct ProfileReference {
    std::string file;             // file name inside the profile folder
    RefRole role = RefRole::Style;
};

struct StyleProfile {
    std::string id;               // folder name
    std::string name;
    std::string perspective;      // gen::Perspective key, or empty
    std::string notes;            // the user's own words
    int strength = 2;             // 1 low (words + palette only), 2 medium (+ reference images), 3 high (+ strict palette)
    int variation = 1;            // 0 strict, 1 some, 2 loose: how much a result may differ before it is flagged
    std::vector<ProfileReference> references;
    StyleFeatures features;
    nlohmann::json toJson() const;
    static Result<StyleProfile> fromJson(const nlohmann::json& j);
};

// Profiles live in <dir>/<id>/profile.json with their reference PNGs next to it.
class StyleLibrary {
public:
    explicit StyleLibrary(std::filesystem::path dir) : m_dir(std::move(dir)) {}
    const std::filesystem::path& dir() const { return m_dir; }
    std::vector<StyleProfile> list() const;
    Result<StyleProfile> load(const std::string& id) const;
    // Creates a profile from images (copied into its folder), measuring them.
    Result<StyleProfile> create(const std::string& name, const std::vector<std::pair<Image, RefRole>>& refs);
    Status save(const StyleProfile& p) const;
    // Adds images to a profile and re-measures it.
    Result<StyleProfile> addReferences(const std::string& id, const std::vector<std::pair<Image, RefRole>>& refs);
    Status remove(const std::string& id) const;
    Result<Image> referenceImage(const StyleProfile& p, const ProfileReference& r) const;
    // Re-measures from the stored references (palette etc.).
    Result<StyleProfile> remeasure(const std::string& id) const;

private:
    std::filesystem::path m_dir;
};

} // namespace acestudio::style
