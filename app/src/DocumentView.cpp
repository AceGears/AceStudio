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
    m_canvas->setUnderlay(m_onion ? toQImage(renderOnion()) : QImage());
    m_canvas->setTint(m_editMask ? maskTint({0, 0, document().width(), document().height()}) : QImage());
}

QImage DocumentView::maskTint(const IntRect& r) const {
    // Red where the mask hides the layer, strength following how much it hides.
    QImage out(r.w, r.h, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    for (int y = 0; y < r.h; ++y) {
        auto* row = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < r.w; ++x) {
            const int hidden = 255 - document().maskValue(m_layer, m_frame, r.x + x, r.y + y);
            if (hidden == 0) continue;
            const int a = hidden * 110 / 255;   // up to ~43% opacity
            row[x] = qRgba(a * 230 / 255, a * 40 / 255, a * 60 / 255, a);   // premultiplied
        }
    }
    return out;
}

Image DocumentView::renderOnion() {
    // Ghost composites are cached: painting only touches the current frame,
    // which is never a ghost; any other change clears the cache.
    const std::vector<int> want = onionSkinFrames(document(), m_frame, *m_onion);
    for (auto it = m_frameCache.begin(); it != m_frameCache.end();)
        it = std::find(want.begin(), want.end(), it->first) == want.end() ? m_frameCache.erase(it) : std::next(it);
    return renderOnionSkin(document(), m_frame, *m_onion, [this](int f) -> const Image& {
        auto it = m_frameCache.find(f);
        if (it == m_frameCache.end()) it = m_frameCache.emplace(f, document().composite(f)).first;
        return it->second;
    });
}

void DocumentView::setOnionSkin(std::optional<OnionSkin> opts) {
    m_onion = opts;
    m_frameCache.clear();
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
    m_frameCache.erase(m_frame);
    m_canvas->updateImageRegion(toQImage(m_composite.copy(r)), QPoint(r.x, r.y));
    if (m_editMask) m_canvas->updateTintRegion(maskTint(r), QPoint(r.x, r.y));
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
        m_frameCache.clear();
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
    if (m_onion) m_frameCache[m_frame] = m_composite;   // up to date: it may become a ghost
    m_frame = frame;
    refreshCanvas();
    Q_EMIT currentFrameChanged(m_frame);
}

void DocumentView::setCurrentLayer(LayerId id) {
    if (id == m_layer || !document().layer(id)) return;
    finishGesture(true);
    m_layer = id;
    if (m_editMask && !document().layer(id)->hasMask) setEditingMask(false);
    else if (m_editMask) m_canvas->setTint(maskTint({0, 0, document().width(), document().height()}));
    Q_EMIT currentLayerChanged(m_layer);
}

void DocumentView::setEditingMask(bool on) {
    if (on && (!document().layer(m_layer) || !document().layer(m_layer)->hasMask)) on = false;
    if (on == m_editMask) return;
    finishGesture(true);
    m_editMask = on;
    m_canvas->setTint(on ? maskTint({0, 0, document().width(), document().height()}) : QImage());
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
    if (m_xf.active) finishFreeTransform(commit);
}

// ------------------------------------------------------------------ free transform

void DocumentView::beginFreeTransform() {
    if (m_xf.active) return;
    finishGesture(true);
    const Layer* l = document().layer(m_layer);
    if (!l || l->kind != LayerKind::Raster || m_editMask) {
        Q_EMIT statusMessage(tr("Free Transform works on a raster layer's pixels"));
        return;
    }
    IntRect area;
    if (hasSelection()) {
        area = document().selection().bounds();
    } else if (const Cel* c = document().cel(m_layer, m_frame)) {
        const IntRect b = c->image.opaqueBounds();
        if (!b.empty()) area = {c->x + b.x, c->y + b.y, b.w, b.h};
    }
    if (area.empty()) {
        Q_EMIT statusMessage(tr("There are no pixels to transform on this layer and frame"));
        return;
    }
    if (!beginSession(tr("Free Transform"), Qt::LeftButton, /*useSelection=*/false)) return;
    FreeXform x;
    x.active = true;
    x.area = area;
    x.selection = hasSelection() ? document().selection() : Mask();
    x.block = Image(area.w, area.h);
    if (const Cel* c = m_session->originalCel())
        x.block.blit(c->image, c->x - area.x, c->y - area.y);
    if (!x.selection.isNull())
        for (int y = 0; y < area.h; ++y)
            for (int xx = 0; xx < area.w; ++xx)
                if (!x.selection.get(area.x + xx, area.y + y)) x.block.setPixel(xx, y, Rgba8::transparent());
    m_xf = std::move(x);
    xfRedraw();
    Q_EMIT statusMessage(tr("Free Transform: drag inside to move, handles to scale (Shift keeps proportions), "
                            "the top knob to rotate (Shift: 15° steps). Enter applies, Esc cancels."));
}

QPointF DocumentView::xfCentre() const {
    return {m_xf.area.x + m_xf.area.w / 2.0 + m_xf.dx, m_xf.area.y + m_xf.area.h / 2.0 + m_xf.dy};
}

QPointF DocumentView::xfMap(double ux, double uy) const {
    const double hw = m_xf.area.w * m_xf.sx / 2.0, hh = m_xf.area.h * m_xf.sy / 2.0;
    const double rad = m_xf.deg * 3.14159265358979323846 / 180.0, c = std::cos(rad), sn = std::sin(rad);
    const double lx = ux * hw, ly = uy * hh;
    const QPointF ctr = xfCentre();
    return {ctr.x() + c * lx - sn * ly, ctr.y() + sn * lx + c * ly};
}

DocumentView::XGrab DocumentView::xfHit(QPointF p) const {
    const double tol = std::max(0.75, 6.0 / m_canvas->zoom());
    auto near = [&](QPointF q) { return QLineF(p, q).length() <= tol; };
    // Rotation knob: 20 screen px beyond the top edge, along the box's own up direction.
    const QPointF top = xfMap(0, m_xf.sy >= 0 ? -1 : 1), ctr = xfCentre();
    const QPointF up = top - ctr;
    const double len = std::hypot(up.x(), up.y());
    const QPointF knob = len > 1e-9 ? top + up / len * (20.0 / m_canvas->zoom()) : top;
    if (near(knob)) return XGrab::Rotate;
    const std::pair<XGrab, QPointF> handles[] = {
        {XGrab::NW, xfMap(-1, -1)}, {XGrab::NE, xfMap(1, -1)}, {XGrab::SW, xfMap(-1, 1)}, {XGrab::SE, xfMap(1, 1)},
        {XGrab::N, xfMap(0, -1)},   {XGrab::S, xfMap(0, 1)},   {XGrab::W, xfMap(-1, 0)},  {XGrab::E, xfMap(1, 0)}};
    for (const auto& [g, q] : handles)
        if (near(q)) return g;
    // Inside the box (or anywhere else) moves it.
    return XGrab::Move;
}

void DocumentView::xfPress(QPointF p) {
    m_xf.grab = xfHit(p);
    m_xf.grabStart = p;
    m_xf.startSx = m_xf.sx;
    m_xf.startSy = m_xf.sy;
    m_xf.startDeg = m_xf.deg;
    m_xf.startDx = m_xf.dx;
    m_xf.startDy = m_xf.dy;
}

void DocumentView::xfDrag(QPointF p, Qt::KeyboardModifiers mods) {
    using G = XGrab;
    const G g = m_xf.grab;
    if (g == G::None) return;
    const QPointF ctr = xfCentre();
    if (g == G::Move) {
        m_xf.dx = m_xf.startDx + static_cast<int>(std::lround(p.x() - m_xf.grabStart.x()));
        m_xf.dy = m_xf.startDy + static_cast<int>(std::lround(p.y() - m_xf.grabStart.y()));
    } else if (g == G::Rotate) {
        const double a0 = std::atan2(m_xf.grabStart.y() - ctr.y(), m_xf.grabStart.x() - ctr.x());
        const double a1 = std::atan2(p.y() - ctr.y(), p.x() - ctr.x());
        double deg = m_xf.startDeg + (a1 - a0) * 180.0 / 3.14159265358979323846;
        if (mods & Qt::ShiftModifier) deg = std::round(deg / 15.0) * 15.0;
        deg = std::fmod(deg, 360.0);
        m_xf.deg = deg < 0 ? deg + 360.0 : deg;
    } else {
        // Scale about the centre: the handle follows the mouse in the box's axes.
        const double rad = m_xf.deg * 3.14159265358979323846 / 180.0, c = std::cos(rad), sn = std::sin(rad);
        const double vx = p.x() - ctr.x(), vy = p.y() - ctr.y();
        const double lx = c * vx + sn * vy, ly = -sn * vx + c * vy;   // into box axes
        const int ux = (g == G::E || g == G::NE || g == G::SE) ? 1 : (g == G::W || g == G::NW || g == G::SW) ? -1 : 0;
        const int uy = (g == G::S || g == G::SE || g == G::SW) ? 1 : (g == G::N || g == G::NE || g == G::NW) ? -1 : 0;
        double sx = m_xf.sx, sy = m_xf.sy;
        if (ux) sx = ux * lx / (m_xf.area.w / 2.0);
        if (uy) sy = uy * ly / (m_xf.area.h / 2.0);
        auto snap = [&](double v, int size) {   // whole output pixels, never zero
            double px = std::round(v * size);
            if (std::abs(px) < 1) px = v < 0 ? -1 : 1;
            return px / size;
        };
        if (ux && uy && (mods & Qt::ShiftModifier)) {
            const double m = std::max(std::abs(sx), std::abs(sy));
            sx = std::copysign(m, sx);
            sy = std::copysign(m, sy);
        }
        m_xf.sx = snap(sx, m_xf.area.w);
        m_xf.sy = snap(sy, m_xf.area.h);
    }
    xfRedraw();
}

void DocumentView::xfRedraw() {
    if (!m_xf.active || !m_session) return;
    PaintSession& s = *m_session;
    s.revertAll();
    const IntRect a = m_xf.area;
    for (int y = a.y; y < a.y + a.h; ++y)
        for (int x = a.x; x < a.x + a.w; ++x)
            if (m_xf.selection.isNull() || m_xf.selection.get(x, y)) s.writeExact(x, y, Rgba8::transparent());
    const Image r = freeTransform(m_xf.block, m_xf.sx, m_xf.sy, m_xf.deg, /*quality=*/false);
    const int px = a.x + (a.w - r.width()) / 2 + m_xf.dx, py = a.y + (a.h - r.height()) / 2 + m_xf.dy;
    for (int y = 0; y < r.height(); ++y)
        for (int x = 0; x < r.width(); ++x)
            if (r.pixel(x, y).a) s.writeExact(px + x, py + y, r.pixel(x, y));
    refreshRegion(s.takeDirty());
    // Box, handles and rotation knob (image coordinates; cosmetic pens).
    QPainterPath guide;
    const QPointF c[4] = {xfMap(-1, -1), xfMap(1, -1), xfMap(1, 1), xfMap(-1, 1)};
    guide.moveTo(c[0]);
    for (int i = 1; i < 4; ++i) guide.lineTo(c[i]);
    guide.closeSubpath();
    const double hs = std::max(0.4, 4.0 / m_canvas->zoom());
    for (QPointF q : {c[0], c[1], c[2], c[3], xfMap(0, -1), xfMap(0, 1), xfMap(-1, 0), xfMap(1, 0)})
        guide.addRect(QRectF(q.x() - hs, q.y() - hs, 2 * hs, 2 * hs));
    const QPointF top = xfMap(0, m_xf.sy >= 0 ? -1 : 1), ctr = xfCentre(), up = top - ctr;
    const double len = std::hypot(up.x(), up.y());
    if (len > 1e-9) {
        const QPointF knob = top + up / len * (20.0 / m_canvas->zoom());
        guide.moveTo(top);
        guide.lineTo(knob);
        guide.addEllipse(knob, hs * 1.2, hs * 1.2);
    }
    m_canvas->setOverlay(guide);
    Q_EMIT statusMessage(tr("Free Transform: %1 × %2 px, %3°, offset %4, %5")
                             .arg(r.width()).arg(r.height()).arg(m_xf.deg, 0, 'f', 1).arg(m_xf.dx).arg(m_xf.dy));
}

void DocumentView::finishFreeTransform(bool commit) {
    if (!m_xf.active) return;
    FreeXform x = std::move(m_xf);
    m_xf = FreeXform{};
    m_canvas->setOverlay({});
    if (m_session) { m_session->cancel(); m_session.reset(); }   // drop the preview; the real edit follows
    const bool changed = x.sx != 1 || x.sy != 1 || std::abs(x.deg) > 1e-9 || x.dx != 0 || x.dy != 0;
    if (!commit || !changed) { refreshCanvas(); return; }
    const std::size_t mark = m_editor.history().mark();
    const double sx = x.sx, sy = x.sy, deg = x.deg;
    const Status st = m_editor.transformLayer(m_layer, m_frame, false, x.selection.isNull() ? nullptr : &x.selection,
                                              "Free Transform",
                                              [sx, sy, deg](const Image& i) { return freeTransform(i, sx, sy, deg, true); },
                                              Point{x.dx, x.dy}, x.area);
    if (!st) { Q_EMIT statusMessage(qs(st.error().message)); return; }
    if (!x.selection.isNull()) {
        // The selection becomes the transformed pixels' footprint.
        const Image r = freeTransform(x.block, sx, sy, deg, true);
        const int px = x.area.x + (x.area.w - r.width()) / 2 + x.dx, py = x.area.y + (x.area.h - r.height()) / 2 + x.dy;
        Mask sel(document().width(), document().height());
        for (int y = 0; y < r.height(); ++y)
            for (int xx = 0; xx < r.width(); ++xx)
                if (r.pixel(xx, y).a) sel.set(px + xx, py + y, true);
        (void)m_editor.setSelection(std::move(sel), "Transform Selection");
        (void)m_editor.history().squash(mark, "Free Transform");
        Q_EMIT documentChanged();
    }
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
    const std::size_t mark = m_editor.history().mark();
    endSession(moved);
    if (moved && hasSelection()) {
        // The selection travels with the pixels, in the same undo step.
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
        (void)m_editor.history().squash(mark, m_moveCopy ? "Duplicate Selection" : "Move Selection");
        Q_EMIT documentChanged();   // refreshes the undo label
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
    if (m_xf.active) {
        if (button == Qt::LeftButton) xfPress(QPointF(px.x() + 0.5, px.y() + 0.5));
        return;
    }
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

void DocumentView::onToolMoved(QPoint px, Qt::MouseButtons buttons, Qt::KeyboardModifiers mods) {
    if (m_xf.active) {
        if (buttons & Qt::LeftButton) xfDrag(QPointF(px.x() + 0.5, px.y() + 0.5), mods);
        return;
    }
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
    if (m_xf.active) { m_xf.grab = XGrab::None; return; }
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
