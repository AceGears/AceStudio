#include "AssetCommon.h"

#include "../DocumentView.h"
#include "../QtBridge.h"

#include "acestudio/core/FileUtil.h"
#include "acestudio/core/Transform.h"

#include <QMouseEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace acestudio::ui {

Image documentImage(const DocumentView* view) {
    if (!view) return {};
    Image img = view->document().composite(view->currentFrame());
    if (const Mask* sel = view->selection()) {
        const IntRect b = sel->bounds();
        if (!b.empty()) img = img.copy(b);
    }
    return img;
}

Image fitSquare(const Image& img, int size) {
    if (img.isNull() || size <= 0) return {};
    const int side = std::min(img.width(), img.height());
    Image sq = img.copy({(img.width() - side) / 2, (img.height() - side) / 2, side, side});
    if (side == size) return sq;
    auto s = scaleNearest(sq, size, size);
    return s ? std::move(s).value() : Image();
}

Status writeTextFile(const std::filesystem::path& path, const std::string& text) {
    return writeFileAtomic(path, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

PixelView::PixelView(QWidget* parent) : QWidget(parent) {
    setMinimumSize(96, 96);
    setMouseTracking(false);
}

void PixelView::setImage(const Image& img) { setImage(toQImage(img)); }

void PixelView::setImage(const QImage& img) {
    m_img = img;
    update();
}

void PixelView::setRepeat(int n) {
    m_repeat = std::max(1, n);
    update();
}

void PixelView::setFixedZoom(int z) {
    m_fixedZoom = std::max(0, z);
    update();
}

int PixelView::zoom() const { return std::max(1, static_cast<int>(scale())); }

// Integer zoom while the image fits; scaled down (still without smoothing)
// when it is larger than the widget, so it is never cut off.
double PixelView::scale() const {
    if (m_img.isNull()) return 1;
    if (m_fixedZoom > 0) return m_fixedZoom;
    const double sw = double(width()) / (m_img.width() * m_repeat), sh = double(height()) / (m_img.height() * m_repeat);
    const double s = std::min(sw, sh);
    return s >= 1 ? std::floor(s) : std::max(s, 0.01);
}

QPoint PixelView::toImage(QPointF p) const {
    const double z = scale();
    const double w = m_img.width() * m_repeat * z, h = m_img.height() * m_repeat * z;
    const double ox = std::floor((width() - w) / 2), oy = std::floor((height() - h) / 2);
    return {static_cast<int>(std::floor((p.x() - ox) / z)), static_cast<int>(std::floor((p.y() - oy) / z))};
}

void PixelView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), palette().window());
    if (m_img.isNull()) return;
    const double z = scale();
    const int w = static_cast<int>(std::lround(m_img.width() * m_repeat * z)), h = static_cast<int>(std::lround(m_img.height() * m_repeat * z));
    const int ox = static_cast<int>(std::floor((width() - m_img.width() * m_repeat * z) / 2)),
              oy = static_cast<int>(std::floor((height() - m_img.height() * m_repeat * z) / 2));
    const int cs = 8;
    for (int y = 0; y < h; y += cs)
        for (int x = 0; x < w; x += cs)
            p.fillRect(ox + x, oy + y, std::min(cs, w - x), std::min(cs, h - y), ((x / cs + y / cs) % 2) ? QColor(60, 60, 64) : QColor(80, 80, 86));
    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    const double tw = m_img.width() * z, th = m_img.height() * z;
    for (int j = 0; j < m_repeat; ++j)
        for (int i = 0; i < m_repeat; ++i)
            p.drawImage(QRectF(ox + i * tw, oy + j * th, tw, th), m_img);
}

void PixelView::mousePressEvent(QMouseEvent* e) { Q_EMIT pressed(toImage(e->position()), e->button()); }

void PixelView::mouseMoveEvent(QMouseEvent* e) {
    if (e->buttons() != Qt::NoButton) Q_EMIT dragged(toImage(e->position()), e->buttons());
}

} // namespace acestudio::ui
