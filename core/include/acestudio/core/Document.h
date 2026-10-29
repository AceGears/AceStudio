#pragma once

#include "acestudio/core/BlendMode.h"
#include "acestudio/core/Image.h"
#include "acestudio/core/Palette.h"
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

enum class LayerKind { Raster, Group };

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

    // ---- Frames ----
    int frameCount() const noexcept { return static_cast<int>(m_frames.size()); }
    const std::vector<Frame>& frames() const noexcept { return m_frames; }
    Status setFrameDuration(int frame, int durationMs);
    Result<int> insertFrame(int at, int durationMs = 100);   // empty cels
    Result<int> duplicateFrame(int frame);
    Status removeFrame(int frame);

    // ---- Cels ----
    const Cel* cel(LayerId layer, int frame) const noexcept;
    Cel* cel(LayerId layer, int frame) noexcept;
    Status setCel(LayerId layer, int frame, Cel cel);
    void clearCel(LayerId layer, int frame) noexcept;
    const std::map<CelKey, Cel>& cels() const noexcept { return m_cels; }

    // ---- Tags ----
    const std::vector<AnimationTag>& tags() const noexcept { return m_tags; }
    Status addTag(AnimationTag tag);
    Status removeTag(std::size_t index);

    // Free-form, namespaced extension data preserved on save: AI
    // generation settings, model provenance, export presets, tileset
    // metadata, per-project preferences.
    nlohmann::json& extensions() noexcept { return m_extensions; }
    const nlohmann::json& extensions() const noexcept { return m_extensions; }

    // Renders all visible layers of a frame into a canvas-sized image.
    Image composite(int frame) const;

    // Checks structural invariants; used after loading untrusted files.
    Status validate() const;

    // Internal: used by the deserializer to restore exact IDs.
    Status restoreLayer(const Layer& layer);
    void restoreFrames(std::vector<Frame> frames) { m_frames = std::move(frames); }

private:
    Document() = default;
    bool isEffectivelyVisible(const Layer& l) const noexcept;
    std::uint8_t effectiveOpacity(const Layer& l) const noexcept;

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
    std::vector<AnimationTag> m_tags;
    nlohmann::json m_extensions = nlohmann::json::object();
};

} // namespace acestudio
