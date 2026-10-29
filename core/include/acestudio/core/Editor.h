#pragma once

// Editor = Document + undo history. All interactive and scripted edits go
// through here so that every change is validated, undoable and observable.
// Shared by the desktop app, and later the CLI, Lua and MCP front ends.

#include "acestudio/core/Document.h"
#include "acestudio/core/UndoStack.h"

#include <functional>
#include <memory>
#include <string>

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
    Result<LayerId> duplicateLayer(LayerId id);
    // Merges a raster layer into the raster layer directly beneath it
    // (same parent), honouring blend mode and opacity on every frame.
    Status mergeDown(LayerId id);

    // ---- Frames ----
    Result<int> insertFrame(int at, int durationMs = 100);
    Result<int> duplicateFrame(int frame);
    Status removeFrame(int frame);
    Status setFrameDuration(int frame, int durationMs);

    // ---- Palette ----
    Status setPaletteColor(std::size_t index, Rgba8 color);
    Status addPaletteColor(Rgba8 color);
    Status removePaletteColor(std::size_t index);

    // ---- Low level (used by PaintSession) ----
    Document& mutableDocument() noexcept { return m_doc; }
    void recordApplied(std::unique_ptr<Command> cmd);
    Status apply(std::unique_ptr<Command> cmd, bool allowMerge = false);
    void notifyChanged() { if (m_listener) m_listener(); }

private:
    Document m_doc;
    UndoStack m_history;
    ChangeListener m_listener;
};

} // namespace acestudio
