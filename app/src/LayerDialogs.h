#pragma once

#include "acestudio/core/Document.h"

#include <QDialog>

class QComboBox;
class QLabel;
class QSlider;

namespace acestudio { class Editor; }

namespace acestudio::ui {

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
    bool m_syncing = false;
};

} // namespace acestudio::ui
