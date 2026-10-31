#pragma once

#include "acestudio/core/BlendMode.h"
#include "acestudio/core/Image.h"
#include "acestudio/core/Palette.h"
#include "acestudio/core/Raster.h"
#include "acestudio/core/Result.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace acestudio {

enum class ColorMode { Rgba, Indexed, Grayscale };
std::string_view toString(ColorMode m) noexcept;
std::optional<ColorMode> colorModeFromString(std::string_view s) noexcept;

using LayerId = std::uint32_t;
inline constexpr LayerId kNoLayer = 0;

enum class LayerKind { Raster, Group, Adjustment };

// Non-destructive colour adjustments. An adjustment layer changes what is
// composited beneath it in the same group; parameters by type:
//   BrightnessContrast  a = brightness -100..100, b = contrast -100..100
//   HueSaturation       a = hue -180..180, b = saturation -100..100, c = lightness -100..100
//   Invert              (none)
//   Posterize           a = levels 2..64
//   Threshold           a = level 0..255 (luma at or above becomes white)
enum class AdjustmentType { BrightnessContrast, HueSaturation, Invert, Posterize, Threshold, Levels, Curves };
std::string_view toString(AdjustmentType t) noexcept;
std::optional<AdjustmentType> adjustmentTypeFromString(std::string_view s) noexcept;

struct CurvePoint {
    std::uint8_t x = 0, y = 0;
    bool operator==(const CurvePoint&) const = default;
};

// Parameters by type:
//   BrightnessContrast  a = brightness -100..100, b = contrast -100..100
//   HueSaturation       a = hue -180..180, b = saturation -100..100, c = lightness -100..100
//   Posterize           a = levels 2..64;   Threshold  a = level 0..255
//   Levels              a = input black 0..254, b = input white (a+1)..255, c = gamma x100 (10..999)
//   Curves              points: 2..16 control points, x strictly increasing (RGB together)
struct Adjustment {
    Adjustment() = default;
    Adjustment(AdjustmentType t, int pa = 0, int pb = 0, int pc = 0) : type(t), a(pa), b(pb), c(pc) {}
    Adjustment(AdjustmentType t, std::vector<CurvePoint> pts) : type(t), points(std::move(pts)) {}

    AdjustmentType type = AdjustmentType::BrightnessContrast;
    int a = 0, b = 0, c = 0;
    std::vector<CurvePoint> points;
    bool operator==(const Adjustment&) const = default;
};
Status validateAdjustment(const Adjustment& adj);

// Seamless-tile editing: painting wraps around the chosen canvas edges and
// the canvas shows neighbouring repeats.
enum class TileMode { None, Horizontal, Vertical, Both };
std::string_view toString(TileMode m) noexcept;
std::optional<TileMode> tileModeFromString(std::string_view s) noexcept;
inline bool wrapsX(TileMode m) noexcept { return m == TileMode::Horizontal || m == TileMode::Both; }
inline bool wrapsY(TileMode m) noexcept { return m == TileMode::Vertical || m == TileMode::Both; }

struct Layer {
    LayerId id = kNoLayer;
    LayerId parent = kNoLayer;          // kNoLayer = top level
    LayerKind kind = LayerKind::Raster;
    std::string name;
    bool visible = true;
    bool locked = false;
    bool alphaLocked = false;
    std::uint8_t opacity = 255;
    BlendMode blend = BlendMode::Normal;
    // Clipping mask: limited to the alpha of the nearest non-clipped layer
    // below it in the same group (the base); hidden when the base is hidden.
    bool clipped = false;
    // Layer mask (raster, group or adjustment layers): per-frame greyscale
    // cels; white shows, black hides. Frames without a mask cel show fully.
    bool hasMask = false;
    bool maskEnabled = true;
    Adjustment adjustment;              // LayerKind::Adjustment only
};

struct Frame {
    int durationMs = 100;
};

enum class PlaybackDirection { Forward, Reverse, PingPong };
std::string_view toString(PlaybackDirection d) noexcept;
std::optional<PlaybackDirection> directionFromString(std::string_view s) noexcept;

struct AnimationTag {
    std::string name;
    int fromFrame = 0;                  // inclusive
    int toFrame = 0;                    // inclusive
    PlaybackDirection direction = PlaybackDirection::Forward;
    int repeat = 0;                     // 0 = loop forever
    std::string color = "#4F8CFF";
};

// A cel is the image content of one raster layer on one frame.
// It is positioned within the canvas so small sprites need not
// store full-canvas buffers.
struct Cel {
    Image image;
    int x = 0;
    int y = 0;
};

struct CelKey {
    LayerId layer = kNoLayer;
    int frame = 0;
    auto operator<=>(const CelKey&) const = default;
};

// The in-memory sprite document. Pure data + invariant-preserving
// mutators; no I/O, no UI. Every mutation that can fail returns Status.
class Document {
public:
    static constexpr int kMaxFrames = 10000;
    static constexpr int kMaxLayers = 1000;

    static Result<Document> create(int width, int height, ColorMode mode = ColorMode::Rgba);

    int width() const noexcept { return m_width; }
    // Changes the canvas size only; cels keep their pixels and positions.
    Status setCanvasSize(int width, int height);
    // Moves every cel by (dx, dy).
    void offsetCels(int dx, int dy) noexcept;
    int height() const noexcept { return m_height; }
    ColorMode colorMode() const noexcept { return m_colorMode; }
    void setColorMode(ColorMode m) noexcept { m_colorMode = m; }

    double pixelAspectX() const noexcept { return m_pixelAspectX; }
    double pixelAspectY() const noexcept { return m_pixelAspectY; }
    Status setPixelAspect(double x, double y);

    const std::string& colorProfile() const noexcept { return m_colorProfile; }
    void setColorProfile(std::string p) { m_colorProfile = std::move(p); }

    Palette& palette() noexcept { return m_palette; }
    const Palette& palette() const noexcept { return m_palette; }

    // ---- Layers (stored bottom -> top) ----
    const std::vector<Layer>& layers() const noexcept { return m_layers; }
    const Layer* layer(LayerId id) const noexcept;
    Layer* layer(LayerId id) noexcept;
    Result<LayerId> addLayer(std::string name, LayerKind kind = LayerKind::Raster, LayerId parent = kNoLayer);
    Status removeLayer(LayerId id);     // removes children of groups and their cels
    Status moveLayer(LayerId id, std::size_t newIndex);
    std::optional<std::size_t> layerIndex(LayerId id) const noexcept;
    // Layer plus all nested descendants, in stack order (bottom -> top).
    std::vector<LayerId> subtree(LayerId id) const;
    // Re-inserts a fully specified layer (exact id) at a stack index; used by undo.
    Status insertLayerAt(const Layer& layer, std::size_t index);
    bool isLayerEffectivelyVisible(LayerId id) const noexcept;
    // True if `id` is a raster layer whose pixels may be edited (not locked,
    // no locked ancestor).
    bool isLayerEditable(LayerId id) const noexcept;

    // ---- Frames ----
    int frameCount() const noexcept { return static_cast<int>(m_frames.size()); }
    const std::vector<Frame>& frames() const noexcept { return m_frames; }
    Status setFrameDuration(int frame, int durationMs);
    Result<int> insertFrame(int at, int durationMs = 100);   // empty cels
    Result<int> duplicateFrame(int frame);
    Status removeFrame(int frame);
    // Moves a frame (with its cels) to a new position; tag ranges stay put.
    Status moveFrame(int from, int to);

    // ---- Cels ----
    const Cel* cel(LayerId layer, int frame) const noexcept;
    Cel* cel(LayerId layer, int frame) noexcept;
    // Setting or clearing a linked cel changes every frame in its link group.
    Status setCel(LayerId layer, int frame, Cel cel);
    void clearCel(LayerId layer, int frame) noexcept;
    const std::map<CelKey, Cel>& cels() const noexcept { return m_cels; }

    // ---- Linked cels ----
    // Frames of one raster layer can share a cel: they always hold identical
    // pixels and position. Group ids are opaque (0 = not linked).
    std::uint32_t celLink(LayerId layer, int frame) const noexcept;
    std::vector<int> linkedFrames(LayerId layer, int frame) const;   // the group, sorted (just `frame` if unlinked)
    // Links the frames (at least two, in range) to share the cel of the first one.
    Status linkCels(LayerId layer, const std::vector<int>& frames);
    void unlinkCels(LayerId layer, const std::vector<int>& frames);
    // After editing a linked cel in place, copies it to the rest of its group.
    void syncLinked(LayerId layer, int frame);
    const std::map<CelKey, std::uint32_t>& links() const noexcept { return m_links; }
    // Restores a link table exactly (undo, file loading); no cel copying.
    void restoreLinks(std::map<CelKey, std::uint32_t> links);

    // ---- Layer masks ----
    // Mask cels store the mask value in the red channel (R = G = B, A = 255).
    const Cel* maskCel(LayerId layer, int frame) const noexcept;
    Cel* maskCel(LayerId layer, int frame) noexcept;
    Status setMaskCel(LayerId layer, int frame, Cel cel);
    void clearMaskCel(LayerId layer, int frame) noexcept;
    const std::map<CelKey, Cel>& maskCels() const noexcept { return m_maskCels; }
    // Mask value 0..255 at a canvas pixel (255 when the layer has no mask,
    // it is disabled, or the frame has no mask cel there).
    std::uint8_t maskValue(LayerId layer, int frame, int x, int y) const noexcept;
    // Canvas-sized greyscale image of the mask (white where none).
    Image renderMask(LayerId layer, int frame) const;

    // ---- Tiling ----
    TileMode tileMode() const noexcept { return m_tileMode; }
    void setTileMode(TileMode m) noexcept { m_tileMode = m; }

    // ---- Selection (transient: not saved, but undoable through Editor) ----
    const Mask& selection() const noexcept { return m_selection; }
    void setSelection(Mask m) { m_selection = std::move(m); }
    bool hasSelection() const noexcept { return !m_selection.isNull(); }

    // ---- Tags ----
    const std::vector<AnimationTag>& tags() const noexcept { return m_tags; }
    Status addTag(AnimationTag tag);
    Status removeTag(std::size_t index);
    Status replaceTags(std::vector<AnimationTag> tags);   // validated as a whole

    // Free-form, namespaced extension data preserved on save: AI
    // generation settings, model provenance, export presets, tileset
    // metadata, per-project preferences.
    nlohmann::json& extensions() noexcept { return m_extensions; }
    const nlohmann::json& extensions() const noexcept { return m_extensions; }

    // Renders all visible layers of a frame into a canvas-sized image.
    // Groups are composited in isolation and then blended with their own
    // blend mode and opacity (W3C group semantics).
    Image composite(int frame) const;
    // Recomposites only `region` of `canvas` (a canvas-sized image).
    void compositeRegion(int frame, const IntRect& region, Image& canvas) const;
    // One raster layer's cel placed on a canvas-sized transparent image.
    Image renderLayer(LayerId id, int frame) const;

    // Checks structural invariants; used after loading untrusted files.
    Status validate() const;

    // Internal: used by the deserializer to restore exact IDs.
    Status restoreLayer(const Layer& layer);
    void restoreFrames(std::vector<Frame> frames) { m_frames = std::move(frames); }

private:
    Document() = default;
    // Composites children of `parent` into `out`, which covers canvas area
    // starting at (originX, originY).
    void compositeChildren(LayerId parent, int frame, Image& out, int originX, int originY, int depth) const;
    // Multiplies `img` (covering the canvas from (ox, oy)) alpha by the mask.
    void applyMask(const Layer& l, int frame, Image& img, int ox, int oy) const;
    template <typename Fn> void remapFrames(Fn fn);

    int m_width = 0;
    int m_height = 0;
    ColorMode m_colorMode = ColorMode::Rgba;
    double m_pixelAspectX = 1.0;
    double m_pixelAspectY = 1.0;
    std::string m_colorProfile = "sRGB";
    Palette m_palette;
    std::vector<Layer> m_layers;
    LayerId m_nextLayerId = 1;
    std::vector<Frame> m_frames;
    std::map<CelKey, Cel> m_cels;
    std::map<CelKey, std::uint32_t> m_links;
    void dropSingletonLinks();
    std::map<CelKey, Cel> m_maskCels;
    TileMode m_tileMode = TileMode::None;
    Mask m_selection;
    std::vector<AnimationTag> m_tags;
    nlohmann::json m_extensions = nlohmann::json::object();
};

} // namespace acestudio
