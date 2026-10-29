#include "DocumentView.h"
#include "CanvasWidget.h"
#include "QtBridge.h"
#include "ToolState.h"

#include "acestudio/core/Log.h"
#include "acestudio/core/Recovery.h"

#include <QFileInfo>
#include <QVBoxLayout>

namespace acestudio::ui {

namespace {
Rgba8 toRgba(const QColor& c) {
    return {static_cast<std::uint8_t>(c.red()), static_cast<std::uint8_t>(c.green()),
            static_cast<std::uint8_t>(c.blue()), static_cast<std::uint8_t>(c.alpha())};
}
Point toPoint(QPoint p) { return {p.x(), p.y()}; }
} // namespace

DocumentView::DocumentView(Document doc, QString filePath, ProjectInfo info, ToolState* tools, QWidget* parent)
    : QWidget(parent), m_editor(std::move(doc)), m_path(std::move(filePath)), m_info(std::move(info)), m_tools(tools) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_canvas = new CanvasWidget(this);
    layout->addWidget(m_canvas);

    connect(m_canvas, &CanvasWidget::cursorMoved, this, &DocumentView::cursorMoved);
    connect(m_canvas, &CanvasWidget::cursorMoved, this, &DocumentView::updateBrushOutline);
    connect(m_canvas, &CanvasWidget::zoomChanged, this, &DocumentView::zoomChanged);
    connect(m_canvas, &CanvasWidget::toolPressed, this, &DocumentView::onToolPressed);
    connect(m_canvas, &CanvasWidget::toolMoved, this, &DocumentView::onToolMoved);
    connect(m_canvas, &CanvasWidget::toolReleased, this, &DocumentView::onToolReleased);
    if (m_tools)
        connect(m_tools, &ToolState::changed, this, [this] {
            m_canvas->setHandMode(m_tools->tool() == Tool::Hand);
        });

    m_recoveryId = RecoveryStore::newId();
    m_editor.setChangeListener([this] { onEditorChanged(); });
    ensureValidLayer();
    refreshCanvas();
}

DocumentView::~DocumentView() {
    m_stroke.reset();
    if (m_session) m_session->cancel();
    m_editor.setChangeListener({});
}

QString DocumentView::displayName() const {
    return m_path.isEmpty() ? tr("Untitled") : QFileInfo(m_path).fileName();
}

void DocumentView::refreshCanvas() {
    m_composite = document().composite(m_frame);
    m_canvas->setImage(toQImage(m_composite));
}

void DocumentView::refreshRegion(const IntRect& region) {
    const IntRect r = region.intersected({0, 0, document().width(), document().height()});
    if (r.empty()) return;
    if (m_composite.width() != document().width() || m_composite.height() != document().height()) {
        refreshCanvas();
        return;
    }
    document().compositeRegion(m_frame, r, m_composite);
    m_canvas->updateImageRegion(toQImage(m_composite.copy(r)), QPoint(r.x, r.y));
}

void DocumentView::ensureValidLayer() {
    const Document& d = document();
    if (m_layer != kNoLayer && d.layer(m_layer)) return;
    LayerId pick = kNoLayer;
    for (auto it = d.layers().rbegin(); it != d.layers().rend(); ++it)
        if (it->kind == LayerKind::Raster) { pick = it->id; break; }
    if (pick == kNoLayer && !d.layers().empty()) pick = d.layers().back().id;
    if (pick != m_layer) {
        m_layer = pick;
        Q_EMIT currentLayerChanged(m_layer);
    }
}

void DocumentView::onEditorChanged() {
    ++m_changeSerial;
    if (m_frame >= document().frameCount()) {
        m_frame = document().frameCount() - 1;
        Q_EMIT currentFrameChanged(m_frame);
    }
    ensureValidLayer();
    if (m_pendingRegion) {
        refreshRegion(*m_pendingRegion);   // a stroke just committed: only its area changed
        m_pendingRegion.reset();
    } else {
        refreshCanvas();
    }
    Q_EMIT documentChanged();
    const bool mod = isModified();
    if (mod != m_lastModified) {
        m_lastModified = mod;
        Q_EMIT modifiedChanged(mod);
    }
}

void DocumentView::setCurrentFrame(int frame) {
    if (frame < 0 || frame >= document().frameCount() || frame == m_frame || m_session) return;
    m_frame = frame;
    refreshCanvas();
    Q_EMIT currentFrameChanged(m_frame);
}

void DocumentView::setCurrentLayer(LayerId id) {
    if (id == m_layer || !document().layer(id)) return;
    m_layer = id;
    Q_EMIT currentLayerChanged(m_layer);
}

// ------------------------------------------------------------------ selection

void DocumentView::selectAll() {
    m_selection = Mask(document().width(), document().height(), true);
    m_canvas->setSelectionRect(QRect(0, 0, document().width(), document().height()));
}

void DocumentView::deselect() {
    m_selection = Mask();
    m_canvas->setSelectionRect({});
}

void DocumentView::clearSelectionContents() {
    auto s = PaintSession::begin(m_editor, m_layer, m_frame, hasSelection() ? "Delete Selection" : "Clear Layer");
    if (!s) { Q_EMIT statusMessage(qs(s.error().message)); return; }
    auto& session = *s.value();
    if (hasSelection()) session.setSelection(&m_selection);
    for (int y = 0; y < document().height(); ++y)
        for (int x = 0; x < document().width(); ++x) session.writePixel(x, y, Rgba8::transparent());
    session.commit();
}

// ------------------------------------------------------------------ tools

bool DocumentView::beginSession(const QString& label) {
    auto s = PaintSession::begin(m_editor, m_layer, m_frame, label.toStdString());
    if (!s) {
        Q_EMIT statusMessage(qs(s.error().message));
        return false;
    }
    m_session = std::move(s).value();
    m_session->setBrush(m_tools->brush());
    if (hasSelection()) m_session->setSelection(&m_selection);
    return true;
}

void DocumentView::onToolPressed(QPoint px, Qt::MouseButton button, Qt::KeyboardModifiers mods) {
    if (!m_tools || m_session) return;
    Tool tool = m_tools->tool();
    if ((mods & Qt::AltModifier) && (tool == Tool::Pencil || tool == Tool::Fill || tool == Tool::Line ||
                                     tool == Tool::Rectangle || tool == Tool::Ellipse))
        tool = Tool::Eyedropper;   // Alt = temporary eyedropper
    const Point p = toPoint(px);
    m_anchor = p;
    m_strokeColor = toRgba(button == Qt::RightButton ? m_tools->secondary() : m_tools->primary());

    switch (tool) {
    case Tool::Hand: return;
    case Tool::Eyedropper: {
        if (!QRect(0, 0, document().width(), document().height()).contains(px)) return;
        const Rgba8 c = sampleColor(document(), m_frame, p, m_tools->sampleMerged() ? kNoLayer : m_layer);
        const QColor q = toQColor(c);
        if (button == Qt::RightButton) m_tools->setSecondary(q);
        else m_tools->setPrimary(q);
        return;
    }
    case Tool::Select:
        m_selecting = true;
        m_canvas->setSelectionRect(QRect(px, QSize(1, 1)));
        return;
    case Tool::Fill:
        if (!beginSession(toolName(tool))) return;
        fillRegion(*m_session, p, m_strokeColor, m_tools->fillContiguous(), m_tools->fillTolerance());
        m_pendingRegion = m_session->touched();
        m_session->commit();
        m_pendingRegion.reset();
        m_session.reset();
        return;
    case Tool::Pencil:
    case Tool::Eraser: {
        if (!beginSession(toolName(tool))) return;
        m_stroke = std::make_unique<FreehandStroke>(*m_session, m_strokeColor, tool == Tool::Eraser, m_tools->pixelPerfect());
        // Shift-click continues with a straight line from the previous stroke.
        if ((mods & Qt::ShiftModifier) && m_lastStrokeEnd) m_stroke->moveTo(*m_lastStrokeEnd);
        m_stroke->moveTo(p);
        m_lastStrokeEnd = p;
        refreshRegion(m_session->takeDirty());
        return;
    }
    case Tool::Line:
    case Tool::Rectangle:
    case Tool::Ellipse:
        if (!beginSession(toolName(tool))) return;
        applyToolTo(*m_session, p, p, true);
        return;
    }
}

void DocumentView::applyToolTo(PaintSession& s, Point from, Point to, bool first) {
    if (!first) s.revertAll();
    switch (m_tools->tool()) {
    case Tool::Line: drawLine(s, from, to, m_strokeColor); break;
    case Tool::Rectangle: drawRectangle(s, from, to, m_tools->fillShapes(), m_strokeColor); break;
    case Tool::Ellipse: drawEllipse(s, from, to, m_tools->fillShapes(), m_strokeColor); break;
    default: break;
    }
    refreshRegion(s.takeDirty());
}

void DocumentView::onToolMoved(QPoint px, Qt::MouseButtons, Qt::KeyboardModifiers mods) {
    Point p = toPoint(px);
    if (m_selecting) {
        const IntRect r = rectFromCorners(m_anchor, p).intersected({0, 0, document().width(), document().height()});
        m_canvas->setSelectionRect(QRect(r.x, r.y, r.w, r.h));
        return;
    }
    if (!m_session) return;
    if (m_stroke) {
        m_stroke->moveTo(p);
        m_lastStrokeEnd = p;
        refreshRegion(m_session->takeDirty());
        return;
    }
    if (mods & Qt::ShiftModifier) {
        const Tool t = m_tools->tool();
        if (t == Tool::Rectangle || t == Tool::Ellipse) {   // constrain to a square/circle
            const int d = std::max(std::abs(p.x - m_anchor.x), std::abs(p.y - m_anchor.y));
            p = {m_anchor.x + (p.x >= m_anchor.x ? d : -d), m_anchor.y + (p.y >= m_anchor.y ? d : -d)};
        }
    }
    applyToolTo(*m_session, m_anchor, p, false);
}

void DocumentView::onToolReleased(QPoint px, Qt::MouseButton, Qt::KeyboardModifiers) {
    if (m_selecting) {
        m_selecting = false;
        const IntRect r = rectFromCorners(m_anchor, toPoint(px)).intersected({0, 0, document().width(), document().height()});
        if (r.empty() || (r.w == 1 && r.h == 1 && m_anchor == toPoint(px))) {
            deselect();   // a click without dragging clears the selection
            return;
        }
        m_selection = Mask(document().width(), document().height());
        m_selection.fillRect(r, true);
        m_canvas->setSelectionRect(QRect(r.x, r.y, r.w, r.h));
        return;
    }
    if (!m_session) return;
    m_stroke.reset();
    m_pendingRegion = m_session->touched();
    m_session->commit();
    m_pendingRegion.reset();
    m_session.reset();
}

void DocumentView::updateBrushOutline(int x, int y, bool inside) {
    if (!m_tools || !inside) { m_canvas->setBrushOutline({}); return; }
    const Tool t = m_tools->tool();
    int n = 1;
    if (t == Tool::Pencil || t == Tool::Eraser || t == Tool::Line || t == Tool::Rectangle || t == Tool::Ellipse)
        n = m_tools->brush().size;
    if (t == Tool::Hand) { m_canvas->setBrushOutline({}); return; }
    const int off = (n - 1) / 2;
    m_canvas->setBrushOutline(QRect(x - off, y - off, n, n));
}

// ------------------------------------------------------------------ save

QString DocumentView::saveTo(const QString& path) {
    const QString target = path.isEmpty() ? m_path : path;
    if (target.isEmpty()) return tr("No file name given.");
    SaveOptions opts;
    opts.createdUtc = m_info.createdUtc;
    if (auto st = saveProject(document(), toPath(target), opts); !st) return qs(st.error().message);
    m_path = target;
    m_neverSaved = false;
    m_editor.history().setClean();
    m_lastModified = false;
    Q_EMIT modifiedChanged(false);
    return {};
}

} // namespace acestudio::ui
