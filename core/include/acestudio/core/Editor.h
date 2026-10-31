#pragma once

// Editor = Document + undo history. All interactive and scripted edits go
// through here so that every change is validated, undoable and observable.
// Shared by the desktop app, and later the CLI, Lua and MCP front ends.

#include "acestudio/core/Document.h"
#include "acestudio/core/Raster.h"
#include "acestudio/core/Transform.h"
#include "acestudio/core/UndoStack.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace acestudio {

class Editor {
public:
    using ChangeListener = std::function<void()>;

    explicit Editor(Document doc);

    const Document& document() const noexcept { return m_doc; }
    UndoStack& history() noexcept { return m_history; }
    const UndoStack& history() const noexcept { return m_history; }

    // Called after every successful change, undo or redo.
    void setChangeListener(ChangeListener l) { m_listener = std::move(l); }

    Status undo();
    Status redo();

    // ---- Layers ----
    // Adds a layer directly above `above` (same parent), or at the top of
    // the stack when `above` is kNoLayer.
    Result<LayerId> addLayer(std::string name, LayerKind kind = LayerKind::Raster, LayerId above = kNoLayer);
    Status removeLayer(LayerId id);
    Status moveLayer(LayerId id, std::size_t newIndex);
    Status renameLayer(LayerId id, std::string name);
    Status setLayerVisible(LayerId id, bool visible);
    Status setLayerLocked(LayerId id, bool locked);
    Status setLayerAlphaLocked(LayerId id, bool locked);
    // Consecutive opacity changes to the same layer merge into one undo step
    // when `mergeable` is true (slider drags).
    Status setLayerOpacity(LayerId id, std::uint8_t opacity, bool mergeable = false);
    Status setLayerBlend(LayerId id, BlendMode mode);
    // Copies a layer, or a group with all its children, with cels and masks.
    Result<LayerId> duplicateLayer(LayerId id);
    // Merges a raster or adjustment layer into the raster layer directly
    // beneath it (same parent). The result is exactly what the pair
    // composites to: blend mode, opacity, masks and clipping are baked, and
    // the merged layer has no mask.
    Status mergeDown(LayerId id);
    // Whether mergeDown(id) would succeed; `why` receives the reason if not.
    bool canMergeDown(LayerId id, std::string* why = nullptr) const;

    // ---- Masks, clipping, adjustment layers ----
    // New mask: all white, or white only inside `reveal` (from a selection).
    // New raster layer above `above` holding `image` at (x, y) on `frame`, as
    // one undo step. The cel may extend past the canvas (nothing is cropped).
    Result<LayerId> addImageLayer(std::string name, Image image, LayerId above, int frame, int x = 0, int y = 0);
    Status addLayerMask(LayerId id, const Mask* reveal = nullptr);
    // Removes the mask; with applyToPixels its values are multiplied into
    // the layer's alpha first (raster layers).
    Status removeLayerMask(LayerId id, bool applyToPixels);
    Status setLayerMaskEnabled(LayerId id, bool enabled);
    Status setLayerClipped(LayerId id, bool clipped);
    Result<LayerId> addAdjustmentLayer(Adjustment adj, LayerId above = kNoLayer);
    Status setLayerAdjustment(LayerId id, Adjustment adj, bool mergeable = false);

    // ---- Document-wide ----
    Status setTileMode(TileMode mode);
    // Replaces the selection (an empty or null mask clears it); undoable.
    Status setSelection(Mask selection, const std::string& label);

    // ---- Frames ----
    Result<int> insertFrame(int at, int durationMs = 100);
    Result<int> duplicateFrame(int frame);
    Status removeFrame(int frame);
    Status setFrameDuration(int frame, int durationMs);
    Status moveFrame(int from, int to);
    // Range operations on frames [first, last], each a single undo step.
    Status removeFrames(int first, int last);
    // Any set of frames (need not be contiguous), one undo step each.
    Status removeFrames(const std::vector<int>& frames);
    Status setFrameDurations(const std::vector<int>& frames, int durationMs);
    // Copies the block and inserts it right after `last`; returns the first copy.
    Result<int> duplicateFrames(int first, int last);
    // Moves the block so it starts at `to` (0 <= to <= frameCount - block size).
    Status moveFrames(int first, int last, int to);
    // Frames of one layer share a cel (the first frame's) / stop sharing it.
    Status linkCels(LayerId layer, const std::vector<int>& frames);
    Status unlinkCels(LayerId layer, const std::vector<int>& frames);
    // Sets the duration of every frame in [first, last].
    Status setFrameDurations(int first, int last, int durationMs);

    // ---- Animation tags ----
    Result<std::size_t> addTag(AnimationTag tag);
    Status updateTag(std::size_t index, AnimationTag tag);
    Status removeTag(std::size_t index);

    // ---- Canvas (whole document) ----
    // New size; existing content is shifted by (offsetX, offsetY).
    Status resizeCanvas(int width, int height, int offsetX, int offsetY);
    Status cropCanvas(const IntRect& rect);
    // Bounds of every visible-or-hidden raster pixel (alpha > 0) on any frame,
    // in canvas coordinates; empty when the document has no pixels.
    IntRect contentBounds() const;
    // Resizes the canvas to contentBounds() (pixels off the canvas come into view).
    Status fitCanvasToContent();
    Status flipCanvas(bool horizontal);
    Status rotateCanvas90(bool clockwise);
    Status rotateCanvas180();

    // ---- Layer content transforms ----
    // Applies `fn` to the current layer's pixels inside `selection` (or the
    // whole canvas when the selection is null/empty). The transformed block is
    // re-centred on the selection's bounding box. allFrames applies it to
    // every frame in one undo step.
    using ImageFn = std::function<Image(const Image&)>;
    // `offset` moves the pasted result; `area` replaces the lifted rectangle
    // (default: the selection bounds, or the canvas).
    Status transformLayer(LayerId layer, int frame, bool allFrames, const Mask* selection,
                          const std::string& label, const ImageFn& fn, Point offset = {0, 0},
                          std::optional<IntRect> area = std::nullopt);

    // ---- Palette ----
    Status setPaletteColor(std::size_t index, Rgba8 color);
    Status addPaletteColor(Rgba8 color);
    Status removePaletteColor(std::size_t index);
    Status replacePalette(Palette palette, const std::string& label = "Replace Palette");

    // ---- Colour mode ----
    // Converts every cel. Indexed: pixels snap to the nearest opaque palette
    // colour (alpha < 128 becomes transparent, else opaque); with `dither`
    // an ordered pattern spreads the error. Grayscale: Rec.709 luma, alpha
    // kept. RGBA: pixels unchanged.
    Status convertColorMode(ColorMode mode, bool dither = false);

    // ---- Low level (used by PaintSession) ----
    Document& mutableDocument() noexcept { return m_doc; }
    void recordApplied(std::unique_ptr<Command> cmd);
    Status apply(std::unique_ptr<Command> cmd, bool allowMerge = false);
    void notifyChanged() { if (m_listener) m_listener(); }

private:
    Status grouped(const std::string& label, const std::function<Status()>& steps);
    Result<LayerId> addLayerWith(std::string name, LayerKind kind, LayerId above,
                                 std::function<void(Layer&)> customize, const std::string& label);
    Document m_doc;
    UndoStack m_history;
    ChangeListener m_listener;
};

} // namespace acestudio
