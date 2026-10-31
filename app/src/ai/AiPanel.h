#pragma once

#include "acestudio/core/AiWorker.h"
#include "acestudio/core/Document.h"

#include <QPointer>
#include <QWidget>

#include <functional>
#include <map>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QTreeWidget;
class QTreeWidgetItem;

namespace acestudio::ui {

class AiService;
class AppContext;
class DocumentView;

// AI Studio dock: runtime status, Generate / Remove Background / Upscale
// forms, and the job queue. Results never overwrite existing pixels: they
// arrive as new layers (same size) or new documents (different size).
class AiPanel : public QWidget {
    Q_OBJECT
public:
    AiPanel(AiService& ai, AppContext& ctx, QWidget* parent = nullptr);
    void setView(DocumentView* view);

public Q_SLOTS:
    void showSetup();
    void showModels();
    void focusTab(int index);   // 0 generate, 1 remove background, 2 upscale

Q_SIGNALS:
    // `frame` is the frame the job was started from (the result belongs there
    // even if the playhead moved while it ran).
    void resultAsLayer(DocumentView* view, const acestudio::Image& image, const QString& name, acestudio::LayerId above, int frame);
    void resultAsDocument(const acestudio::Image& image, const QString& name);
    // One image per frame, with each frame's duration in ms.
    void resultAsAnimation(const std::vector<acestudio::Image>& frames, const std::vector<int>& durations, const QString& name);
    void statusMessage(const QString& text);

private:
    struct Pending {
        enum class Kind { Generate, RemoveBackground, Upscale, Other } kind = Kind::Other;
        QPointer<DocumentView> view;
        LayerId layer = kNoLayer;
        int frame = 0;
        bool pixel = false;
        QString name;
        std::vector<int> durations;   // upscaling every frame: one entry per frame
    };

    QWidget* buildGenerateTab();
    QWidget* buildRemoveTab();
    QWidget* buildUpscaleTab();
    void refreshStatus();
    void refreshModels();
    void refreshJob(const QString& id);
    void onJobFinished(const QString& id);
    void runGenerate();
    void runRemoveBackground();
    void runUpscale();
    bool submit(const std::string& task, nlohmann::json params, const QString& label, Pending pending,
                const std::vector<std::pair<std::string, const Image*>>& images);
    QString modelPath(QComboBox* combo) const;
    void fillModelCombo(QComboBox* combo, const std::string& task);

    AiService& m_ai;
    AppContext& m_ctx;
    QPointer<DocumentView> m_view;
    std::map<std::string, Pending> m_pending;

    QLabel* m_status = nullptr;
    QPushButton* m_setup = nullptr;
    QPushButton* m_restart = nullptr;
    QTabWidget* m_tabs = nullptr;
    // Generate
    QComboBox* m_genModel = nullptr;
    QCheckBox* m_genLora = nullptr;
    QSpinBox* m_genLoraStrength = nullptr;
    QPlainTextEdit* m_prompt = nullptr;
    QLineEdit* m_negative = nullptr;
    QCheckBox* m_pixelMode = nullptr;
    QCheckBox* m_usePalette = nullptr;
    QCheckBox* m_cropSubject = nullptr;
    QLabel* m_genPreview = nullptr;
    std::map<std::string, int> m_previewSerial;   // job id -> preview last shown
    QSpinBox* m_width = nullptr;
    QSpinBox* m_height = nullptr;
    QSpinBox* m_steps = nullptr;
    QDoubleSpinBox* m_guidance = nullptr;
    QSpinBox* m_seed = nullptr;
    QSpinBox* m_count = nullptr;
    QCheckBox* m_img2img = nullptr;
    QDoubleSpinBox* m_strength = nullptr;
    QPushButton* m_genButton = nullptr;
    // Remove background
    QComboBox* m_bgModel = nullptr;
    QCheckBox* m_bgHard = nullptr;
    QCheckBox* m_bgMerged = nullptr;
    QPushButton* m_bgButton = nullptr;
    // Upscale
    QComboBox* m_upModel = nullptr;
    QCheckBox* m_upAllFrames = nullptr;
    QPushButton* m_upButton = nullptr;
    // Queue
    QTreeWidget* m_queue = nullptr;
    std::map<std::string, QTreeWidgetItem*> m_rows;
};

} // namespace acestudio::ui
