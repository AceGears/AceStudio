#pragma once

#include <QColor>
#include <QImage>
#include <QPointF>
#include <QWidget>

namespace acestudio::ui {

struct CanvasAppearance {
    QColor background{"#1B1B20"};
    QColor checker1{"#2A2A31"};
    QColor checker2{"#34343C"};
    int checkerSize = 8;
    bool gridVisible = true;
    QColor gridColor{255, 255, 255, 31};
    double gridMinZoom = 8.0;
    double zoomStep = 1.25;
};

// Pixel-exact viewer for a flattened document frame.
// Rendering never interpolates: every zoom level uses nearest-neighbour.
// Zoom: mouse wheel (anchored at the cursor). Pan: middle-drag, or
// Space + left-drag, or Shift/Alt + wheel.
class CanvasWidget : public QWidget {
    Q_OBJECT
public:
    static constexpr double kMinZoom = 0.125;
    static constexpr double kMaxZoom = 128.0;

    explicit CanvasWidget(QWidget* parent = nullptr);

    void setImage(const QImage& image);
    void setAppearance(const CanvasAppearance& a);
    double zoom() const { return m_zoom; }

public Q_SLOTS:
    void zoomIn();
    void zoomOut();
    void zoomToFit();
    void zoomActualSize();
    void setGridVisible(bool visible);

Q_SIGNALS:
    void cursorMoved(int x, int y, bool inside);
    void zoomChanged(double zoom);

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void showEvent(QShowEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void keyReleaseEvent(QKeyEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    void setZoomAt(double zoom, QPointF anchorWidget);
    QPointF widgetToImage(QPointF p) const;
    void rebuildChecker();

    QImage m_image;
    CanvasAppearance m_look;
    QPixmap m_checkerTile;
    double m_zoom = 1.0;
    QPointF m_offset;              // widget position of image origin
    bool m_fitPending = true;
    bool m_autoFit = true;         // refit on resize until the user zooms or pans
    bool m_panning = false;
    bool m_spaceHeld = false;
    QPointF m_lastMouse;
};

} // namespace acestudio::ui
