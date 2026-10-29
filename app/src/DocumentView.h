#pragma once

#include "acestudio/core/Document.h"
#include "acestudio/core/ProjectIO.h"

#include <QString>
#include <QWidget>

namespace acestudio::ui {

class CanvasWidget;

// One open document (one tab): owns the Document, its file identity and
// modification state, and the canvas that displays it.
class DocumentView : public QWidget {
    Q_OBJECT
public:
    DocumentView(Document doc, QString filePath, ProjectInfo info, QWidget* parent = nullptr);

    Document& document() { return m_doc; }
    const Document& document() const { return m_doc; }
    CanvasWidget* canvas() const { return m_canvas; }

    QString filePath() const { return m_path; }
    QString displayName() const;
    bool isModified() const { return m_modified; }
    int currentFrame() const { return m_frame; }

    void setCurrentFrame(int frame);
    // Call after any mutation of document(); repaints and flags modified.
    void notifyDocumentChanged(bool markModified = true);

    // Saves to `path` (empty = current path). Returns an error message or empty.
    QString saveTo(const QString& path);

Q_SIGNALS:
    void documentChanged();
    void currentFrameChanged(int frame);
    void modifiedChanged(bool modified);
    void cursorMoved(int x, int y, bool inside);
    void zoomChanged(double zoom);

private:
    void setModified(bool m);

    Document m_doc;
    QString m_path;
    ProjectInfo m_info;
    CanvasWidget* m_canvas = nullptr;
    int m_frame = 0;
    bool m_modified = false;
};

} // namespace acestudio::ui
