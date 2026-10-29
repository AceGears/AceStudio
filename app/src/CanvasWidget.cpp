#include "CanvasWidget.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace acestudio::ui {

CanvasWidget::CanvasWidget(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMinimumSize(200, 150);
    setCursor(Qt::CrossCursor);
    rebuildChecker();
}

void CanvasWidget::setImage(const QImage& image) {
    const bool sizeChanged = image.size() != m_image.size();
    m_image = image;
    if (sizeChanged) m_fitPending = true;
    if (m_fitPending && isVisible()) zoomToFit();
    update();
}

void CanvasWidget::updateImageRegion(const QImage& patch, const QPoint& at) {
    if (m_image.isNull() || patch.isNull()) return;
    {
        QPainter p(&m_image);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.drawImage(at, patch);
    }
    update(imageToWidget(QRect(at, patch.size())).toAlignedRect().adjusted(-2, -2, 2, 2));
}

void CanvasWidget::setAppearance(const CanvasAppearance& a) {
    m_look = a;
    rebuildChecker();
    update();
}

void CanvasWidget::rebuildChecker() {
    const int s = std::max(2, m_look.checkerSize);
    m_checkerTile = QPixmap(s * 2, s * 2);
    QPainter p(&m_checkerTile);
    p.fillRect(0, 0, s * 2, s * 2, m_look.checker1);
    p.fillRect(s, 0, s, s, m_look.checker2);
    p.fillRect(0, s, s, s, m_look.checker2);
}

void CanvasWidget::setGridVisible(bool visible) {
    m_look.gridVisible = visible;
    update();
}

void CanvasWidget::zoomIn() { m_autoFit = false; setZoomAt(m_zoom * m_look.zoomStep, rect().center()); }
void CanvasWidget::zoomOut() { m_autoFit = false; setZoomAt(m_zoom / m_look.zoomStep, rect().center()); }

void CanvasWidget::zoomActualSize() {
    m_autoFit = false;
    m_zoom = 1.0;
    m_offset = QPointF((width() - m_image.width()) / 2.0, (height() - m_image.height()) / 2.0);
    Q_EMIT zoomChanged(m_zoom);
    update();
}

void CanvasWidget::zoomToFit() {
    m_fitPending = false;
    m_autoFit = true;
    if (m_image.isNull() || width() <= 0 || height() <= 0) return;
    const double margin = 32.0;
    const double zx = (width() - margin) / m_image.width();
    const double zy = (height() - margin) / m_image.height();
    double z = std::clamp(std::min(zx, zy), kMinZoom, kMaxZoom);
    // Prefer integer zoom above 1x so pixels stay uniformly sized.
    if (z >= 1.0) z = std::floor(z);
    m_zoom = z;
    m_offset = QPointF(std::round((width() - m_image.width() * z) / 2.0),
                       std::round((height() - m_image.height() * z) / 2.0));
    Q_EMIT zoomChanged(m_zoom);
    update();
}

void CanvasWidget::setZoomAt(double zoom, QPointF anchor) {
    zoom = std::clamp(zoom, kMinZoom, kMaxZoom);
    if (zoom >= 1.0) zoom = std::round(zoom);                 // integer steps keep pixels crisp
    if (std::abs(zoom - m_zoom) < 1e-9) return;
    const QPointF imgPt = widgetToImage(anchor);
    m_zoom = zoom;
    m_offset = anchor - imgPt * m_zoom;
    m_offset = QPointF(std::round(m_offset.x()), std::round(m_offset.y()));
    Q_EMIT zoomChanged(m_zoom);
    update();
}

QPointF CanvasWidget::widgetToImage(QPointF p) const { return (p - m_offset) / m_zoom; }

QPoint CanvasWidget::pixelAt(QPointF w) const {
    const QPointF ip = widgetToImage(w);
    return {static_cast<int>(std::floor(ip.x())), static_cast<int>(std::floor(ip.y()))};
}

QRectF CanvasWidget::imageToWidget(const QRect& r) const {
    return {m_offset.x() + r.x() * m_zoom, m_offset.y() + r.y() * m_zoom, r.width() * m_zoom, r.height() * m_zoom};
}

void CanvasWidget::setHandMode(bool on) {
    m_handMode = on;
    setCursor(on ? Qt::OpenHandCursor : Qt::CrossCursor);
}

void CanvasWidget::setBrushOutline(const QRect& r) {
    if (r == m_brushOutline) return;
    m_brushOutline = r;
    update();
}

void CanvasWidget::setSelectionRect(const QRect& r) {
    if (r == m_selection) return;
    m_selection = r;
    update();
}

void CanvasWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), m_look.background);
    if (m_image.isNull()) return;

    const QRectF target(m_offset, QSizeF(m_image.width() * m_zoom, m_image.height() * m_zoom));
    p.save();
    p.setClipRect(target);
    p.setBrushOrigin(target.topLeft());
    p.fillRect(target, QBrush(m_checkerTile));
    p.restore();

    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.drawImage(target, m_image);

    if (m_look.gridVisible && m_zoom >= m_look.gridMinZoom) {
        p.setPen(QPen(m_look.gridColor, 1));
        const QRectF vis = target.intersected(QRectF(rect()));
        const int x0 = std::max(0, static_cast<int>(std::floor((vis.left() - m_offset.x()) / m_zoom)));
        const int x1 = std::min(m_image.width(), static_cast<int>(std::ceil((vis.right() - m_offset.x()) / m_zoom)));
        const int y0 = std::max(0, static_cast<int>(std::floor((vis.top() - m_offset.y()) / m_zoom)));
        const int y1 = std::min(m_image.height(), static_cast<int>(std::ceil((vis.bottom() - m_offset.y()) / m_zoom)));
        for (int x = x0; x <= x1; ++x) {
            const double wx = m_offset.x() + x * m_zoom;
            p.drawLine(QPointF(wx, vis.top()), QPointF(wx, vis.bottom()));
        }
        for (int y = y0; y <= y1; ++y) {
            const double wy = m_offset.y() + y * m_zoom;
            p.drawLine(QPointF(vis.left(), wy), QPointF(vis.right(), wy));
        }
    }

    p.setPen(QPen(palette().color(QPalette::Mid), 1));
    p.drawRect(target.adjusted(-1, -1, 0, 0));

    // Selection: two-tone dashed outline readable on any colour.
    if (!m_selection.isEmpty()) {
        const QRectF r = imageToWidget(m_selection).adjusted(0, 0, -1, -1);
        p.setPen(QPen(Qt::black, 1));
        p.drawRect(r);
        QPen dash(Qt::white, 1, Qt::DashLine);
        dash.setDashPattern({4, 4});
        p.setPen(dash);
        p.drawRect(r);
    }
    // Brush footprint preview (hidden while panning).
    if (!m_brushOutline.isEmpty() && !m_panning && !m_handMode && !m_spaceHeld) {
        const QRectF r = imageToWidget(m_brushOutline).adjusted(0, 0, -1, -1);
        p.setPen(QPen(QColor(0, 0, 0, 160), 1));
        p.drawRect(r.adjusted(-1, -1, 1, 1));
        p.setPen(QPen(QColor(255, 255, 255, 200), 1));
        p.drawRect(r);
    }
}

void CanvasWidget::resizeEvent(QResizeEvent*) {
    if (m_fitPending || m_autoFit) zoomToFit();
}

void CanvasWidget::showEvent(QShowEvent*) {
    if (m_fitPending) zoomToFit();
}

void CanvasWidget::wheelEvent(QWheelEvent* e) {
    const QPoint delta = e->angleDelta();
    if (e->modifiers() & (Qt::ShiftModifier | Qt::AltModifier)) {
        // Pan: Shift scrolls horizontally, Alt vertically-with-trackpad.
        const QPoint px = e->pixelDelta().isNull() ? delta / 2 : e->pixelDelta();
        m_offset += (e->modifiers() & Qt::ShiftModifier) ? QPointF(px.y(), px.x()) : QPointF(px);
        m_autoFit = false;
        update();
    } else if (delta.y() != 0) {
        const double steps = delta.y() / 120.0;
        double factor = std::pow(m_look.zoomStep, steps);
        double z = m_zoom * factor;
        // Guarantee progress at integer zoom levels.
        if (m_zoom >= 1.0 && std::round(z) == m_zoom) z = m_zoom + (steps > 0 ? 1 : -1);
        m_autoFit = false;
        setZoomAt(z, e->position());
    }
    e->accept();
}

void CanvasWidget::mousePressEvent(QMouseEvent* e) {
    const bool panGesture = e->button() == Qt::MiddleButton ||
                            (e->button() == Qt::LeftButton && (m_spaceHeld || m_handMode));
    if (panGesture && !m_toolActive) {
        m_panning = true;
        m_autoFit = false;
        m_lastMouse = e->position();
        setCursor(Qt::ClosedHandCursor);
        e->accept();
        return;
    }
    if ((e->button() == Qt::LeftButton || e->button() == Qt::RightButton) && !m_panning && !m_toolActive) {
        m_toolActive = true;
        Q_EMIT toolPressed(pixelAt(e->position()), e->button(), e->modifiers());
        e->accept();
        return;
    }
    QWidget::mousePressEvent(e);
}

void CanvasWidget::mouseMoveEvent(QMouseEvent* e) {
    if (m_panning) {
        m_offset += e->position() - m_lastMouse;
        m_lastMouse = e->position();
        update();
    }
    const QPoint px = pixelAt(e->position());
    if (m_toolActive) Q_EMIT toolMoved(px, e->buttons(), e->modifiers());
    Q_EMIT cursorMoved(px.x(), px.y(), px.x() >= 0 && px.y() >= 0 && px.x() < m_image.width() && px.y() < m_image.height());
}

void CanvasWidget::mouseReleaseEvent(QMouseEvent* e) {
    if (m_panning && (e->button() == Qt::MiddleButton || e->button() == Qt::LeftButton)) {
        m_panning = false;
        setCursor(m_spaceHeld || m_handMode ? Qt::OpenHandCursor : Qt::CrossCursor);
        return;
    }
    if (m_toolActive && (e->button() == Qt::LeftButton || e->button() == Qt::RightButton)) {
        m_toolActive = false;
        Q_EMIT toolReleased(pixelAt(e->position()), e->button(), e->modifiers());
        return;
    }
    QWidget::mouseReleaseEvent(e);
}

void CanvasWidget::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Space && !e->isAutoRepeat()) {
        m_spaceHeld = true;
        setCursor(Qt::OpenHandCursor);
        return;
    }
    QWidget::keyPressEvent(e);
}

void CanvasWidget::keyReleaseEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Space && !e->isAutoRepeat()) {
        m_spaceHeld = false;
        if (!m_panning) setCursor(m_handMode ? Qt::OpenHandCursor : Qt::CrossCursor);
        return;
    }
    QWidget::keyReleaseEvent(e);
}

void CanvasWidget::leaveEvent(QEvent*) {
    m_brushOutline = {};
    update();
    Q_EMIT cursorMoved(-1, -1, false);
}

} // namespace acestudio::ui
