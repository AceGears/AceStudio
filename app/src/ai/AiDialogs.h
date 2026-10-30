#pragma once

#include "acestudio/core/ModelCatalog.h"

#include <QDialog>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QTableWidget;

namespace acestudio::ui {

class AiService;
class AppContext;

// Explains what will be installed and where, then starts AiService setup.
class AiSetupDialog : public QDialog {
    Q_OBJECT
public:
    AiSetupDialog(AiService& ai, AppContext& ctx, QWidget* parent = nullptr);
    QStringList profiles() const;

private:
    void detectPython();
    AiService& m_ai;
    AppContext& m_ctx;
    QLabel* m_python = nullptr;
    QLineEdit* m_pythonPath = nullptr;
    QCheckBox* m_onnx = nullptr;
    QCheckBox* m_diffusion = nullptr;
    QRadioButton* m_cuda = nullptr;
    QRadioButton* m_cpu = nullptr;
    QPushButton* m_install = nullptr;
};

// Shows a model's licence; the download button stays disabled until the
// user ticks the acceptance box.
class LicenseDialog : public QDialog {
    Q_OBJECT
public:
    LicenseDialog(const ai::ModelEntry& m, QWidget* parent = nullptr);
};

// Catalogue browser: install state, licence, size; download/remove.
class ModelsDialog : public QDialog {
    Q_OBJECT
public:
    ModelsDialog(AiService& ai, QWidget* parent = nullptr);

private:
    void rebuild();
    const ai::ModelEntry* selected() const;
    void download();
    void remove();
    AiService& m_ai;
    QTableWidget* m_table = nullptr;
    QLabel* m_details = nullptr;
    QPushButton* m_download = nullptr;
    QPushButton* m_remove = nullptr;
};

QString formatBytes(std::uint64_t bytes);

} // namespace acestudio::ui
