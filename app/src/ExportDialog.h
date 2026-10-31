#pragma once

#include "acestudio/core/Document.h"

#include <nlohmann/json.hpp>

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;

namespace acestudio::ui {

enum class ExportKind { SpriteSheet, Gif, Apng, Webp, PngSequence };

// One dialog for every animation export; only the options that apply to the
// chosen kind are shown. Accepting writes the files (asking before replacing
// any existing file) and reports errors without closing the dialog.
class ExportDialog : public QDialog {
    Q_OBJECT
public:
    // `preset` holds the options last used for this kind (see preset()).
    ExportDialog(ExportKind kind, const Document& doc, int currentFrame, QString suggestedPath,
                 const nlohmann::json& preset = {}, QWidget* parent = nullptr);
    // The chosen options (not frames or file name), to remember for next time.
    nlohmann::json preset() const;
    static QString kindKey(ExportKind kind);
    // Human-readable summary of what was written (valid after accept).
    QString resultMessage() const { return m_result; }
    QString outputPath() const;

private:
    void browse();
    void updateRangeControls();
    void updateSummary();
    bool runExport();   // false keeps the dialog open
    QString kindTitle() const;
    void applyPreset(const nlohmann::json& p);

    ExportKind m_kind;
    const Document& m_doc;
    QString m_result;
    QComboBox* m_range = nullptr;
    QSpinBox* m_from = nullptr;
    QSpinBox* m_to = nullptr;
    QSpinBox* m_scale = nullptr;
    QSpinBox* m_loop = nullptr;
    QComboBox* m_layout = nullptr;
    QSpinBox* m_columns = nullptr;
    QSpinBox* m_border = nullptr;
    QSpinBox* m_spacing = nullptr;
    QSpinBox* m_inner = nullptr;
    QCheckBox* m_trim = nullptr;
    QCheckBox* m_merge = nullptr;
    QCheckBox* m_pot = nullptr;
    QComboBox* m_json = nullptr;
    QLineEdit* m_path = nullptr;
    QLabel* m_summary = nullptr;
};

} // namespace acestudio::ui
