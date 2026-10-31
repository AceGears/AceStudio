#pragma once

#include "acestudio/core/Pose.h"

#include <QDialog>
#include <QImage>
#include <QWidget>

class QComboBox;
class QSlider;

namespace acestudio::ui {

// Shows a skeleton over an optional underlay; drag joints to move them,
// right-click a joint to hide or show it.
class PoseCanvas : public QWidget {
    Q_OBJECT
public:
    explicit PoseCanvas(QWidget* parent = nullptr);
    void setPose(const Pose& p);
    const Pose& pose() const { return m_pose; }
    void setUnderlay(const QImage& img);   // e.g. the current frame, drawn faded
    QSize sizeHint() const override { return {320, 320}; }

Q_SIGNALS:
    void poseEdited();

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;

private:
    QRectF area() const;
    QPointF toWidget(const Keypoint& k) const;
    int hit(QPointF p) const;

    Pose m_pose;
    QImage m_underlay;
    int m_drag = -1;
    int m_hover = -1;
};

// Edits one pose: presets (standing or a phase of a cycle), mirror, and
// direct joint dragging.
class PoseDialog : public QDialog {
    Q_OBJECT
public:
    PoseDialog(const Pose& initial, const QImage& underlay, QWidget* parent = nullptr);
    Pose pose() const;

private:
    void applyPreset();
    PoseCanvas* m_canvas = nullptr;
    QComboBox* m_preset = nullptr;
    QSlider* m_phase = nullptr;
};

} // namespace acestudio::ui
