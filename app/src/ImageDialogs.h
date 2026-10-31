#pragma once

#include "acestudio/core/Transform.h"

#include <QDialog>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QSpinBox;

namespace acestudio::ui {

// Canvas size with a 3×3 anchor grid deciding where existing content goes.
class CanvasSizeDialog : public QDialog {
    Q_OBJECT
public:
    CanvasSizeDialog(int width, int height, QWidget* parent = nullptr);
    int newWidth() const;
    int newHeight() const;
    // Offset applied to existing content for the chosen anchor.
    int offsetX() const;
    int offsetY() const;

private:
    int m_oldW, m_oldH;
    QSpinBox* m_w = nullptr;
    QSpinBox* m_h = nullptr;
    QButtonGroup* m_anchor = nullptr;
};

// Rotate (RotSprite) by an arbitrary angle.
class RotateDialog : public QDialog {
    Q_OBJECT
public:
    explicit RotateDialog(QWidget* parent = nullptr);
    double degrees() const;
    bool allFrames() const;
    bool expand() const;

private:
    QDoubleSpinBox* m_angle = nullptr;
    QCheckBox* m_allFrames = nullptr;
    QCheckBox* m_expand = nullptr;
};

// Nearest-neighbour scale by percentage.
class ScaleDialog : public QDialog {
    Q_OBJECT
public:
    explicit ScaleDialog(QWidget* parent = nullptr);
    double percentX() const;
    double percentY() const;
    bool allFrames() const;

private:
    QDoubleSpinBox* m_x = nullptr;
    QDoubleSpinBox* m_y = nullptr;
    QCheckBox* m_lock = nullptr;
    QCheckBox* m_allFrames = nullptr;
};

class OutlineDialog : public QDialog {
    Q_OBJECT
public:
    explicit OutlineDialog(QWidget* parent = nullptr);
    OutlineMode mode() const;
    bool diagonal() const;
    bool allFrames() const;

private:
    QComboBox* m_mode = nullptr;
    QCheckBox* m_diagonal = nullptr;
    QCheckBox* m_allFrames = nullptr;
};

} // namespace acestudio::ui
