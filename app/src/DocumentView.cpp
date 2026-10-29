#include "DocumentView.h"
#include "CanvasWidget.h"
#include "QtBridge.h"

#include <QFileInfo>
#include <QVBoxLayout>

namespace acestudio::ui {

DocumentView::DocumentView(Document doc, QString filePath, ProjectInfo info, QWidget* parent)
    : QWidget(parent), m_doc(std::move(doc)), m_path(std::move(filePath)), m_info(std::move(info)) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_canvas = new CanvasWidget(this);
    layout->addWidget(m_canvas);
    connect(m_canvas, &CanvasWidget::cursorMoved, this, &DocumentView::cursorMoved);
    connect(m_canvas, &CanvasWidget::zoomChanged, this, &DocumentView::zoomChanged);
    m_canvas->setImage(toQImage(m_doc.composite(m_frame)));
}

QString DocumentView::displayName() const {
    return m_path.isEmpty() ? tr("Untitled") : QFileInfo(m_path).fileName();
}

void DocumentView::setCurrentFrame(int frame) {
    if (frame < 0 || frame >= m_doc.frameCount() || frame == m_frame) return;
    m_frame = frame;
    m_canvas->setImage(toQImage(m_doc.composite(m_frame)));
    Q_EMIT currentFrameChanged(m_frame);
}

void DocumentView::notifyDocumentChanged(bool markModified) {
    if (m_frame >= m_doc.frameCount()) m_frame = m_doc.frameCount() - 1;
    m_canvas->setImage(toQImage(m_doc.composite(m_frame)));
    if (markModified) setModified(true);
    Q_EMIT documentChanged();
}

QString DocumentView::saveTo(const QString& path) {
    const QString target = path.isEmpty() ? m_path : path;
    if (target.isEmpty()) return tr("No file name given.");
    SaveOptions opts;
    opts.createdUtc = m_info.createdUtc;
    if (auto st = saveProject(m_doc, toPath(target), opts); !st) return qs(st.error().message);
    m_path = target;
    setModified(false);
    return {};
}

void DocumentView::setModified(bool m) {
    if (m == m_modified) return;
    m_modified = m;
    Q_EMIT modifiedChanged(m);
}

} // namespace acestudio::ui
