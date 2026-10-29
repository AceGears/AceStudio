#pragma once

#include "acestudio/core/Editor.h"
#include "acestudio/core/Paint.h"
#include "acestudio/core/ProjectIO.h"

#include <QPointer>
#include <QString>
#include <QWidget>

#include <memory>
#include <optional>

namespace acestudio::ui {

class CanvasWidget;
class ToolState;

// One open document (one tab): owns the Editor (document + undo history),
// the current layer/frame/selection, and turns canvas input into edits.
class DocumentView : public QWidget {
    Q_OBJECT
public:
    DocumentView(Document doc, QString filePath, ProjectInfo info, ToolState* tools, QWidget* parent = nullptr);
    ~DocumentView() override;

    Editor& editor() { return m_editor; }
    const Document& document() const { return m_editor.document(); }
    CanvasWidget* canvas() const { return m_canvas; }

    QString filePath() const { return m_path; }
    QString displayName() const;
    bool isModified() const { return !m_editor.history().isClean() || m_neverSaved; }
    int currentFrame() const { return m_frame; }
    LayerId currentLayer() const { return m_layer; }

    void setCurrentFrame(int frame);
    void setCurrentLayer(LayerId id);

    // Selection (rectangular for now). Empty mask = no selection.
    bool hasSelection() const { return !m_selection.isNull(); }
    void selectAll();
    void deselect();
    // Erases the selection on the current layer, or the whole cel if none.
    void clearSelectionContents();

    // Saves to `path` (empty = current path). Returns an error message or empty.
    QString saveTo(const QString& path);
    // Marks a freshly created document as unsaved even with empty history.
    void markNeverSaved() { m_neverSaved = true; m_changeSerial++; Q_EMIT modifiedChanged(true); }

    // Crash-recovery bookkeeping.
    const std::string& recoveryId() const { return m_recoveryId; }
    void setRecoveryId(std::string id) { m_recoveryId = std::move(id); }
    bool needsAutosave() const { return isModified() && m_changeSerial != m_autosavedSerial; }
    void markAutosaved() { m_autosavedSerial = m_changeSerial; }
    // True while a stroke or selection drag is in progress.
    bool isBusy() const { return m_session != nullptr || m_selecting; }

Q_SIGNALS:
    void documentChanged();
    void currentFrameChanged(int frame);
    void currentLayerChanged(acestudio::LayerId layer);
    void modifiedChanged(bool modified);
    void cursorMoved(int x, int y, bool inside);
    void zoomChanged(double zoom);
    void statusMessage(const QString& text);

private:
    void onEditorChanged();
    void refreshCanvas();
    void refreshRegion(const IntRect& r);
    void ensureValidLayer();
    void onToolPressed(QPoint px, Qt::MouseButton button, Qt::KeyboardModifiers mods);
    void onToolMoved(QPoint px, Qt::MouseButtons buttons, Qt::KeyboardModifiers mods);
    void onToolReleased(QPoint px, Qt::MouseButton button, Qt::KeyboardModifiers mods);
    void updateBrushOutline(int x, int y, bool inside);
    void applyToolTo(PaintSession& s, Point from, Point to, bool first);
    bool beginSession(const QString& label);

    Editor m_editor;
    QString m_path;
    ProjectInfo m_info;
    QPointer<ToolState> m_tools;
    CanvasWidget* m_canvas = nullptr;
    int m_frame = 0;
    LayerId m_layer = kNoLayer;
    Mask m_selection;
    bool m_lastModified = false;
    bool m_neverSaved = false;
    Image m_composite;                      // cached flattened current frame
    std::optional<IntRect> m_pendingRegion; // next editor change only touched this area
    std::string m_recoveryId;
    std::uint64_t m_changeSerial = 0;
    std::uint64_t m_autosavedSerial = 0;

    // Active gesture state.
    std::unique_ptr<PaintSession> m_session;
    std::unique_ptr<FreehandStroke> m_stroke;
    Point m_anchor;
    Rgba8 m_strokeColor;
    bool m_selecting = false;
    std::optional<Point> m_lastStrokeEnd;
};

} // namespace acestudio::ui
