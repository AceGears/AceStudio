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
    rebuildChecker();
}

void CanvasWidget::setImage(const QImage& image) {
    const bool sizeChanged = image.size() != m_image.size();
    m_image = image;
    if (sizeChanged) m_fitPending = true;
    if (m_fitPending && isVisible()) zoomToFit();
    update();
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
    if (e->button() == Qt::MiddleButton || (e->button() == Qt::LeftButton && m_spaceHeld)) {
        m_panning = true;
        m_autoFit = false;
        m_lastMouse = e->position();
        setCursor(Qt::ClosedHandCursor);
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
    const QPointF ip = widgetToImage(e->position());
    const int x = static_cast<int>(std::floor(ip.x()));
    const int y = static_cast<int>(std::floor(ip.y()));
    Q_EMIT cursorMoved(x, y, x >= 0 && y >= 0 && x < m_image.width() && y < m_image.height());
}

void CanvasWidget::mouseReleaseEvent(QMouseEvent* e) {
    if (m_panning && (e->button() == Qt::MiddleButton || e->button() == Qt::LeftButton)) {
        m_panning = false;
        setCursor(m_spaceHeld ? Qt::OpenHandCursor : Qt::ArrowCursor);
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
        if (!m_panning) setCursor(Qt::ArrowCursor);
        return;
    }
    QWidget::keyReleaseEvent(e);
}

void CanvasWidget::leaveEvent(QEvent*) { Q_EMIT cursorMoved(-1, -1, false); }

} // namespace acestudio::ui
