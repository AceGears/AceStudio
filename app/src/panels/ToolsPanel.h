#pragma once

#include "ToolState.h"

#include <QWidget>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLabel;
class QSpinBox;
class QWidget;

namespace acestudio::ui {

class ColorPairWidget;

// Tool palette, colour pair and the options of the active tool.
class ToolsPanel : public QWidget {
    Q_OBJECT
public:
    ToolsPanel(ToolState* state, QWidget* parent = nullptr);

private:
    void syncFromState();

    ToolState* m_state;
    QButtonGroup* m_group = nullptr;
    ColorPairWidget* m_colors = nullptr;
    QLabel* m_toolLabel = nullptr;
    QWidget* m_brushBox = nullptr;
    QSpinBox* m_size = nullptr;
    QComboBox* m_shape = nullptr;
    QComboBox* m_mirror = nullptr;
    QCheckBox* m_pixelPerfect = nullptr;
    QCheckBox* m_filled = nullptr;
    QWidget* m_fillBox = nullptr;
    QCheckBox* m_contiguous = nullptr;
    QSpinBox* m_tolerance = nullptr;
    QCheckBox* m_sampleMerged = nullptr;
    QSpinBox* m_dither = nullptr;
    QWidget* m_selBox = nullptr;
    QComboBox* m_selOp = nullptr;
    QWidget* m_gradBox = nullptr;
    QComboBox* m_gradShape = nullptr;
    QComboBox* m_gradDither = nullptr;
    bool m_syncing = false;
};

} // namespace acestudio::ui
