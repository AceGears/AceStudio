#include "DocumentView.h"
#include "CanvasWidget.h"
#include "QtBridge.h"
#include "ToolState.h"

#include "acestudio/core/Log.h"
#include "acestudio/core/Recovery.h"

#include <QFileInfo>
#include <QPainterPath>
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
    connect(m_canvas, &CanvasWidget::toolDoubleClicked, this, &DocumentView::onToolDoubleClicked);
    connect(m_canvas, &CanvasWidget::cursorMoved, this, &DocumentView::onHover);
    connect(m_canvas, &CanvasWidget::commitRequested, this, [this] { finishGesture(true); });
    connect(m_canvas, &CanvasWidget::cancelRequested, this, [this] { finishGesture(false); });
    connect(m_canvas, &CanvasWidget::nudgeRequested, this, [this](int dx, int dy) {
        if (m_tools && m_tools->tool() == Tool::Move) nudge(dx, dy);
    });
    if (m_tools)
        connect(m_tools, &ToolState::changed, this, [this] {
            m_canvas->setHandMode(m_tools->tool() == Tool::Hand);
            // Switching tools finishes a multi-click shape.
            const Tool t = m_tools->tool();
            if (m_curveStage >= 0 && t != Tool::Curve) finishGesture(true);
            if (!m_polygon.empty() && t != Tool::Polygon) finishGesture(true);
        });

    m_recoveryId = RecoveryStore::newId();
    m_editor.setChangeListener([this] { onEditorChanged(); });
    ensureValidLayer();
    refreshCanvas();
}

DocumentView::~DocumentView() {
    m_stroke.reset();
    m_clone.reset();
    if (m_session) m_session->cancel();
    m_editor.setChangeListener({});
}

QString DocumentView::displayName() const {
    return m_path.isEmpty() ? tr("Untitled") : QFileInfo(m_path).fileName();
}

void DocumentView::refreshCanvas() {
    m_canvas->setTiling(wrapsX(document().tileMode()), wrapsY(document().tileMode()));
    m_composite = document().composite(m_frame);
    m_canvas->setImage(toQImage(m_composite));
    m_canvas->setUnderlay(m_onion ? toQImage(renderOnionSkin(document(), m_frame, *m_onion)) : QImage());
}

void DocumentView::setOnionSkin(std::optional<OnionSkin> opts) {
    m_onion = opts;
    refreshCanvas();
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
    if (m_editMask && (!document().layer(m_layer) || !document().layer(m_layer)->hasMask)) setEditingMask(false);
    updateSelectionOutline();   // undo/redo and canvas transforms change it
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
    finishGesture(true);
    m_layer = id;
    if (m_editMask && !document().layer(id)->hasMask) setEditingMask(false);
    Q_EMIT currentLayerChanged(m_layer);
}

void DocumentView::setEditingMask(bool on) {
    if (on && (!document().layer(m_layer) || !document().layer(m_layer)->hasMask)) on = false;
    if (on == m_editMask) return;
    finishGesture(true);
    m_editMask = on;
    Q_EMIT editingMaskChanged(on);
    Q_EMIT statusMessage(on ? tr("Painting on the layer mask: white shows, black hides") : tr("Painting on the layer"));
}

// ------------------------------------------------------------------ selection

void DocumentView::selectAll() {
    (void)m_editor.setSelection(Mask(document().width(), document().height(), true), "Select All");
}

void DocumentView::deselect() {
    if (hasSelection()) (void)m_editor.setSelection(Mask(), "Deselect");
}

void DocumentView::applySelection(const Mask& shape, SelectionOp op) {
    Mask m = hasSelection() ? document().selection() : Mask(document().width(), document().height());
    combineMask(m, shape, op);
    static const char* labels[] = {"Select", "Add to Selection", "Subtract from Selection", "Intersect Selection"};
    (void)m_editor.setSelection(std::move(m), labels[static_cast<int>(op)]);
}

void DocumentView::updateSelectionOutline() {
    QPainterPath path;
    if (hasSelection()) {
        // Boundary edges between selected and unselected pixels.
        const Mask& m = document().selection();
        for (int y = 0; y <= m.height(); ++y) {
            int runStart = -1;
            for (int x = 0; x <= m.width(); ++x) {
                const bool edge = x < m.width() && m.get(x, y) != m.get(x, y - 1);
                if (edge && runStart < 0) runStart = x;
                if (!edge && runStart >= 0) { path.moveTo(runStart, y); path.lineTo(x, y); runStart = -1; }
            }
        }
        for (int x = 0; x <= m.width(); ++x) {
            int runStart = -1;
            for (int y = 0; y <= m.height(); ++y) {
                const bool edge = y < m.height() && m.get(x, y) != m.get(x - 1, y);
                if (edge && runStart < 0) runStart = y;
                if (!edge && runStart >= 0) { path.moveTo(x, runStart); path.lineTo(x, y); runStart = -1; }
            }
        }
    }
    m_canvas->setSelectionOutline(path);
}

SelectionOp DocumentView::opFor(Qt::KeyboardModifiers mods) const {
    const bool shift = mods & Qt::ShiftModifier, alt = mods & Qt::AltModifier;
    if (shift && alt) return SelectionOp::Intersect;
    if (shift) return SelectionOp::Add;
    if (alt) return SelectionOp::Subtract;
    return m_tools ? m_tools->selectionOp() : SelectionOp::Replace;
}

void DocumentView::clearSelectionContents() {
    // While editing a mask, clearing hides (mask black) instead of erasing pixels.
    auto s = PaintSession::begin(m_editor, m_layer, m_frame,
                                 m_editMask ? (hasSelection() ? "Hide Selection (Mask)" : "Hide All (Mask)")
                                            : (hasSelection() ? "Delete Selection" : "Clear Layer"),
                                 m_editMask ? PaintTarget::Mask : PaintTarget::Pixels);
    if (!s) { Q_EMIT statusMessage(qs(s.error().message)); return; }
    auto& session = *s.value();
    if (hasSelection()) session.setSelection(&document().selection());
    for (int y = 0; y < document().height(); ++y)
        for (int x = 0; x < document().width(); ++x) session.writePixel(x, y, Rgba8::transparent());
    session.commit();
}

// ------------------------------------------------------------------ tools

bool DocumentView::beginSession(const QString& label, Qt::MouseButton button, bool useSelection) {
    const PaintTarget target = m_editMask ? PaintTarget::Mask : PaintTarget::Pixels;
    auto s = PaintSession::begin(m_editor, m_layer, m_frame, (m_editMask ? label + tr(" (Mask)") : label).toStdString(), target);
    if (!s) {
        Q_EMIT statusMessage(qs(s.error().message));
        return false;
    }
    m_session = std::move(s).value();
    m_session->setBrush(m_tools->brush());
    if (useSelection && hasSelection()) m_session->setSelection(&document().selection());
    if (m_tools->brush().ink == Ink::Shading) {
        // Ramp: palette entries from the primary to the secondary colour
        // (or the whole palette if either is missing from it).
        const auto& pal = document().palette().colors();
        auto find = [&](const QColor& q) {
            for (std::size_t i = 0; i < pal.size(); ++i)
                if (pal[i].r == q.red() && pal[i].g == q.green() && pal[i].b == q.blue()) return static_cast<long>(i);
            return -1L;
        };
        const long a = find(m_tools->primary()), b = find(m_tools->secondary());
        std::vector<Rgba8> ramp;
        if (a >= 0 && b >= 0 && a != b) {
            const long step = a < b ? 1 : -1;
            for (long i = a;; i += step) { ramp.push_back(pal[static_cast<std::size_t>(i)]); if (i == b) break; }
        } else {
            ramp = pal;
        }
        m_session->setShading(std::move(ramp), button == Qt::RightButton ? -1 : 1);
    }
    return true;
}

void DocumentView::endSession(bool commit) {
    m_stroke.reset();
    m_clone.reset();
    if (!m_session) return;
    if (commit) {
        m_pendingRegion = m_session->touched();
        m_session->commit();
        m_pendingRegion.reset();
    } else {
        m_session->cancel();
    }
    m_session.reset();
    m_canvas->setOverlay({});
}

void DocumentView::finishGesture(bool commit) {
    if (m_curveStage >= 0) {
        m_curveStage = -1;
        endSession(commit);
        return;
    }
    if (!m_polygon.empty()) {
        if (commit && m_session && m_polygon.size() >= 2) {
            m_session->revertAll();
            drawPolygon(*m_session, m_polygon, m_tools && m_tools->fillShapes(), m_strokeColor);
        }
        m_polygon.clear();
        endSession(commit && m_session != nullptr);
        return;
    }
    if (m_moving) finishMove(commit);
}

void DocumentView::redrawCurve() {
    if (!m_session) return;
    m_session->revertAll();
    if (m_curveStage == 0) drawLine(*m_session, m_curveA, m_curveB, m_strokeColor);
    else drawCurve(*m_session, m_curveA, m_curveB, m_curveC1, m_curveC2, m_strokeColor);
    refreshRegion(m_session->takeDirty());
    QPainterPath guide;
    if (m_curveStage >= 1) {
        auto handle = [&](Point from, Point c) {
            guide.moveTo(from.x + 0.5, from.y + 0.5);
            guide.lineTo(c.x + 0.5, c.y + 0.5);
            guide.addRect(QRectF(c.x, c.y, 1, 1));
        };
        handle(m_curveA, m_curveC1);
        handle(m_curveB, m_curveC2);
    }
    m_canvas->setOverlay(guide);
}

void DocumentView::redrawPolygonPreview(std::optional<Point> cursor) {
    if (!m_session || m_polygon.empty()) return;
    m_session->revertAll();
    for (std::size_t i = 1; i < m_polygon.size(); ++i) drawLine(*m_session, m_polygon[i - 1], m_polygon[i], m_strokeColor);
    if (m_polygon.size() == 1) m_session->stamp(m_polygon[0].x, m_polygon[0].y, m_strokeColor);
    refreshRegion(m_session->takeDirty());
    QPainterPath guide;
    if (cursor) {
        guide.moveTo(m_polygon.back().x + 0.5, m_polygon.back().y + 0.5);
        guide.lineTo(cursor->x + 0.5, cursor->y + 0.5);
        guide.lineTo(m_polygon.front().x + 0.5, m_polygon.front().y + 0.5);
    }
    guide.addRect(QRectF(m_polygon.front().x - 0.5, m_polygon.front().y - 0.5, 2, 2));   // click here to close
    m_canvas->setOverlay(guide);
}

void DocumentView::onHover(int x, int y, bool) {
    if (!m_polygon.empty()) redrawPolygonPreview(Point{x, y});
}

void DocumentView::onToolDoubleClicked(QPoint) {
    if (!m_polygon.empty()) finishGesture(true);
}

// ------------------------------------------------------------------ move tool

void DocumentView::beginMove(Point at, bool copy) {
    if (!beginSession(copy ? tr("Duplicate Selection") : (hasSelection() ? tr("Move Selection") : tr("Move Layer")),
                      Qt::LeftButton, /*useSelection=*/false))
        return;
    const int W = document().width(), H = document().height();
    m_moveMask = hasSelection() ? document().selection() : Mask(W, H, true);
    const Image& src = m_session->original();
    m_moveImage = Image(W, H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (m_moveMask.get(x, y)) m_moveImage.setPixel(x, y, src.pixel(x, y));
    m_moveCopy = copy;
    m_moveOffset = {0, 0};
    m_anchor = at;
    m_moving = true;
}

void DocumentView::updateMove(Point to) {
    if (!m_moving || !m_session) return;
    m_moveOffset = {to.x - m_anchor.x, to.y - m_anchor.y};
    PaintSession& s = *m_session;
    s.revertAll();
    const int W = document().width(), H = document().height();
    const bool mask = s.target() == PaintTarget::Mask;
    if (!m_moveCopy)
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                if (m_moveMask.get(x, y)) s.writeExact(x, y, mask ? Rgba8{255, 255, 255, 255} : Rgba8::transparent());
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (m_moveMask.get(x, y) && (mask || m_moveImage.pixel(x, y).a > 0))
                s.writeExact(x + m_moveOffset.x, y + m_moveOffset.y, m_moveImage.pixel(x, y));
    refreshRegion(s.takeDirty());
    // Preview the selection outline at its new position.
    if (hasSelection()) {
        QPainterPath p;
        const IntRect b = m_moveMask.bounds();
        p.addRect(QRectF(b.x + m_moveOffset.x, b.y + m_moveOffset.y, b.w, b.h));
        m_canvas->setSelectionPreview(p);
    }
}

void DocumentView::finishMove(bool commit) {
    if (!m_moving) return;
    m_moving = false;
    m_canvas->setSelectionPreview({});
    const Point off = m_moveOffset;
    const bool moved = commit && (off.x != 0 || off.y != 0);
    endSession(moved);
    if (moved && hasSelection()) {
        // The selection travels with the pixels (a second, separate undo step).
        const int W = document().width(), H = document().height();
        Mask shifted(W, H);
        const bool wx = wrapsX(document().tileMode()), wy = wrapsY(document().tileMode());
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                if (m_moveMask.get(x, y)) {
                    int nx = x + off.x, ny = y + off.y;
                    if (wx) nx = ((nx % W) + W) % W;
                    if (wy) ny = ((ny % H) + H) % H;
                    shifted.set(nx, ny, true);
                }
        (void)m_editor.setSelection(std::move(shifted), "Move Selection Outline");
    }
    m_moveImage = Image();
    m_moveMask = Mask();
}

void DocumentView::nudge(int dx, int dy) {
    if (m_session || m_selecting) return;
    beginMove({0, 0}, false);
    if (!m_moving) return;
    updateMove({dx, dy});
    finishMove(true);
}

void DocumentView::onToolPressed(QPoint px, Qt::MouseButton button, Qt::KeyboardModifiers mods) {
    if (!m_tools) return;
    Tool tool = m_tools->tool();
    // Multi-click tools keep their session between clicks.
    if (m_curveStage >= 1 && tool == Tool::Curve) {
        const Point p = toPoint(px);
        if (m_curveStage == 1) m_curveC1 = m_curveC2 = p;
        else m_curveC2 = p;
        redrawCurve();
        return;
    }
    if (!m_polygon.empty() && tool == Tool::Polygon) {
        const Point p = toPoint(px);
        const Point first = m_polygon.front();
        if (m_polygon.size() >= 3 && std::abs(p.x - first.x) <= 1 && std::abs(p.y - first.y) <= 1) {
            finishGesture(true);   // clicked the first point: close
            return;
        }
        if (!(p == m_polygon.back())) m_polygon.push_back(p);
        redrawPolygonPreview(std::nullopt);
        return;
    }
    if (m_session) return;
    if ((mods & Qt::AltModifier) && (tool == Tool::Pencil || tool == Tool::Fill || tool == Tool::Line ||
                                     tool == Tool::Rectangle || tool == Tool::Ellipse))
        tool = Tool::Eyedropper;   // Alt = temporary eyedropper
    const Point p = toPoint(px);
    m_anchor = p;
    m_strokeColor = toRgba(button == Qt::RightButton ? m_tools->secondary() : m_tools->primary());

    switch (tool) {
    case Tool::Hand: return;
    case Tool::Move:
        beginMove(p, mods & Qt::AltModifier);
        return;
    case Tool::Curve:
        if (!beginSession(toolName(tool), button)) return;
        m_curveStage = 0;
        m_curveA = m_curveB = p;
        redrawCurve();
        return;
    case Tool::Polygon:
        if (!beginSession(toolName(tool), button)) return;
        m_polygon = {p};
        redrawPolygonPreview(std::nullopt);
        return;
    case Tool::Clone: {
        if (mods & Qt::AltModifier) {
            m_cloneSource = p;
            m_cloneOffset.reset();
            Q_EMIT statusMessage(tr("Clone source set at %1, %2").arg(p.x).arg(p.y));
            return;
        }
        if (!m_cloneSource) { Q_EMIT statusMessage(tr("Alt-click to set the clone source first")); return; }
        if (!m_cloneOffset) m_cloneOffset = Point{p.x - m_cloneSource->x, p.y - m_cloneSource->y};   // aligned from now on
        Image source = m_tools->sampleMerged() ? document().composite(m_frame)
                                               : (m_editMask ? document().renderMask(m_layer, m_frame) : document().renderLayer(m_layer, m_frame));
        if (!beginSession(toolName(tool), button)) return;
        m_clone = std::make_unique<CloneStroke>(*m_session, std::move(source), *m_cloneOffset);
        m_clone->moveTo(p);
        refreshRegion(m_session->takeDirty());
        return;
    }
    case Tool::Eyedropper: {
        if (!QRect(0, 0, document().width(), document().height()).contains(px)) return;
        const Rgba8 c = sampleColor(document(), m_frame, p, m_tools->sampleMerged() ? kNoLayer : m_layer);
        const QColor q = toQColor(c);
        if (button == Qt::RightButton) m_tools->setSecondary(q);
        else m_tools->setPrimary(q);
        return;
    }
    case Tool::Select: {
        m_selecting = true;
        m_gestureOp = opFor(mods);
        QPainterPath preview;
        preview.addRect(QRectF(px.x(), px.y(), 1, 1));
        m_canvas->setSelectionPreview(preview);
        return;
    }
    case Tool::Lasso:
        m_selecting = true;
        m_gestureOp = opFor(mods);
        m_lasso = {p};
        return;
    case Tool::MagicWand: {
        if (!QRect(0, 0, document().width(), document().height()).contains(px)) return;
        const Image src = m_tools->sampleMerged() ? document().composite(m_frame) : document().renderLayer(m_layer, m_frame);
        applySelection(floodRegion(src, p, m_tools->fillContiguous(), m_tools->fillTolerance()), opFor(mods));
        return;
    }
    case Tool::Gradient:
        if (!beginSession(toolName(tool), button)) return;
        m_gradientEnd = toRgba(button == Qt::RightButton ? m_tools->primary() : m_tools->secondary());
        drawGradient(*m_session, p, p, m_strokeColor, m_gradientEnd, m_tools->gradientShape(), m_tools->gradientDither());
        refreshRegion(m_session->takeDirty());
        return;
    case Tool::Fill:
        if (!beginSession(toolName(tool), button)) return;
        fillRegion(*m_session, p, m_strokeColor, m_tools->fillContiguous(), m_tools->fillTolerance());
        m_pendingRegion = m_session->touched();
        m_session->commit();
        m_pendingRegion.reset();
        m_session.reset();
        return;
    case Tool::Pencil:
    case Tool::Eraser: {
        if (!beginSession(toolName(tool), button)) return;
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
        if (!beginSession(toolName(tool), button)) return;
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
        QPainterPath preview;
        if (m_tools->tool() == Tool::Lasso) {
            if (!m_lasso.empty() && m_lasso.back() == p) return;
            m_lasso.push_back(p);
            preview.moveTo(m_lasso[0].x + 0.5, m_lasso[0].y + 0.5);
            for (std::size_t i = 1; i < m_lasso.size(); ++i) preview.lineTo(m_lasso[i].x + 0.5, m_lasso[i].y + 0.5);
        } else {
            const IntRect r = rectFromCorners(m_anchor, p).intersected({0, 0, document().width(), document().height()});
            preview.addRect(QRectF(r.x, r.y, r.w, r.h));
        }
        m_canvas->setSelectionPreview(preview);
        return;
    }
    if (m_moving) { updateMove(p); return; }
    if (m_curveStage >= 0) {
        if (m_curveStage == 0) m_curveB = p;
        else if (m_curveStage == 1) m_curveC1 = m_curveC2 = p;
        else m_curveC2 = p;
        redrawCurve();
        return;
    }
    if (!m_polygon.empty()) return;
    if (!m_session) return;
    if (m_clone) {
        m_clone->moveTo(p);
        refreshRegion(m_session->takeDirty());
        QPainterPath cross;   // where the source is being sampled
        const Point src{p.x - m_cloneOffset->x, p.y - m_cloneOffset->y};
        cross.moveTo(src.x - 2, src.y + 0.5); cross.lineTo(src.x + 3, src.y + 0.5);
        cross.moveTo(src.x + 0.5, src.y - 2); cross.lineTo(src.x + 0.5, src.y + 3);
        m_canvas->setOverlay(cross);
        return;
    }
    if (m_tools->tool() == Tool::Gradient) {
        m_session->revertAll();
        drawGradient(*m_session, m_anchor, p, m_strokeColor, m_gradientEnd, m_tools->gradientShape(), m_tools->gradientDither());
        refreshRegion(m_session->takeDirty());
        return;
    }
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
        m_canvas->setSelectionPreview({});
        const int W = document().width(), H = document().height();
        if (m_tools->tool() == Tool::Lasso) {
            if (m_lasso.size() < 3) {
                if (m_gestureOp == SelectionOp::Replace) deselect();
            } else {
                applySelection(polygonMask(W, H, m_lasso), m_gestureOp);
            }
            m_lasso.clear();
            return;
        }
        const IntRect r = rectFromCorners(m_anchor, toPoint(px)).intersected({0, 0, W, H});
        if (r.empty() || (r.w == 1 && r.h == 1 && m_anchor == toPoint(px))) {
            if (m_gestureOp == SelectionOp::Replace) deselect();   // a plain click clears the selection
            return;
        }
        Mask shape(W, H);
        shape.fillRect(r, true);
        applySelection(shape, m_gestureOp);
        return;
    }
    if (m_moving) { finishMove(true); return; }
    if (m_curveStage >= 0) {
        if (m_curveStage == 0 && m_curveA == m_curveB) { m_curveStage = -1; endSession(true); return; }   // a dot
        if (m_curveStage < 2) {
            if (m_curveStage == 0) { m_curveC1 = m_curveA; m_curveC2 = m_curveB; }
            ++m_curveStage;
            Q_EMIT statusMessage(m_curveStage == 1 ? tr("Curve: drag to bend (first control point); Enter to finish")
                                                   : tr("Curve: drag the second control point; Enter to finish"));
            redrawCurve();
            return;
        }
        m_curveStage = -1;
        endSession(true);
        return;
    }
    if (!m_polygon.empty()) return;   // polygons finish on close / double-click / Enter
    endSession(true);
}

void DocumentView::updateBrushOutline(int x, int y, bool inside) {
    if (!m_tools || !inside) { m_canvas->setBrushOutline({}); return; }
    const Tool t = m_tools->tool();
    if (t == Tool::Hand || t == Tool::Move || isSelectionTool(t)) { m_canvas->setBrushOutline({}); return; }
    const BrushSettings b = m_tools->brush();
    if (isBrushTool(t) && b.shape == BrushShape::Custom && b.tip) {
        const int w = b.tip->width(), h = b.tip->height();
        m_canvas->setBrushOutline(QRect(x - (w - 1) / 2, y - (h - 1) / 2, w, h));
        return;
    }
    const int n = isBrushTool(t) ? b.size : 1;
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
