#pragma once

#include "acestudio/core/Document.h"

#include <QDialog>

class QComboBox;
class QSpinBox;

namespace acestudio::ui {

class NewProjectDialog : public QDialog {
    Q_OBJECT
public:
    explicit NewProjectDialog(QWidget* parent = nullptr);

    int canvasWidth() const;
    int canvasHeight() const;
    int frameCount() const;
    ColorMode colorMode() const;

private:
    QComboBox* m_preset = nullptr;
    QSpinBox* m_width = nullptr;
    QSpinBox* m_height = nullptr;
    QSpinBox* m_frames = nullptr;
    QComboBox* m_mode = nullptr;
};

} // namespace acestudio::ui
