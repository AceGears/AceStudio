#pragma once

#include "acestudio/core/Document.h"

#include <QDialog>
#include <QWidget>

#include <vector>

class QComboBox;
class QLabel;
class QSlider;

namespace acestudio { class Editor; }

namespace acestudio::ui {

// Curve editor: drag points, click empty space to add one, double-click or
// right-click a point to remove it (the end points stay).
class CurveWidget : public QWidget {
    Q_OBJECT
public:
    explicit CurveWidget(QWidget* parent = nullptr);
    void setPoints(std::vector<CurvePoint> pts);
    const std::vector<CurvePoint>& points() const { return m_points; }
    QSize sizeHint() const override { return {256, 256}; }

Q_SIGNALS:
    void changed();

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;

private:
    QRectF plot() const;
    QPointF toWidget(CurvePoint p) const;
    CurvePoint fromWidget(QPointF w) const;
    int hit(QPointF w) const;
    void removeAt(int i);

    std::vector<CurvePoint> m_points{{0, 0}, {255, 255}};
    int m_drag = -1;
};

// Edits an adjustment layer's type and parameters with live preview. While
// open, changes are applied (merged into one undo step); Cancel restores the
// history to where it was when the dialog opened.
class AdjustmentDialog : public QDialog {
    Q_OBJECT
public:
    AdjustmentDialog(Editor& editor, LayerId layer, QWidget* parent = nullptr);
    void reject() override;

private:
    void typeChanged();
    void apply();
    Adjustment current() const;

    Editor& m_editor;
    LayerId m_layer;
    std::size_t m_historyIndex;
    QComboBox* m_type = nullptr;
    QSlider* m_sliders[3] = {};
    QLabel* m_labels[3] = {};
    QLabel* m_values[3] = {};
    CurveWidget* m_curve = nullptr;
    bool m_syncing = false;
};

} // namespace acestudio::ui
