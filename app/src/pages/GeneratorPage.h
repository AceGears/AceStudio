#pragma once
// AI Studio workspace: the unified generator. One form for every asset type
// (describe → configure → generate → refine → send on), backed by FLUX.2
// [klein] when installed (references, region edits) or SDXL + Pixel Art XL.
// Every result is checked automatically (gen::checkAsset) and the problems
// found are listed next to it.

#include "acestudio/core/AssetGen.h"
#include "acestudio/core/Document.h"
#include "acestudio/core/Image.h"
#include "acestudio/core/SpriteMotion.h"
#include "acestudio/core/StyleProfile.h"

#include <QPointer>
#include <QWidget>

#include <map>
#include <optional>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;

namespace acestudio::ui {

class AiService;
class AppContext;
class DocumentView;

class GeneratorPage : public QWidget {
    Q_OBJECT
public:
    GeneratorPage(AiService& ai, AppContext& ctx, QWidget* parent = nullptr);
    void setView(DocumentView* view);
    void setPrompt(const QString& prompt, const QString& assetTypeKey = {});
    // Adds an image to the references (up to four).
    void addReference(const Image& img, const QString& name, style::RefRole role = style::RefRole::Style);

Q_SIGNALS:
    void openAsDocument(const acestudio::Image& img, const QString& name);
    void addAsLayer(const acestudio::Image& img, const QString& name);
    void animateRequested(const acestudio::Image& img, const QString& name);
    void openAnimation(const std::vector<acestudio::Image>& frames, const std::vector<int>& durations,
                       const std::vector<acestudio::AnimationTag>& tags, const QString& name);
    void showModelsRequested();
    void showSetupRequested();
    void statusMessage(const QString& text);

private:
    struct Result {
        Image image;
        Image raw;               // full-size generation (before reduction), when there is one
        QString name;
        long long seed = 0;
        gen::QualityReport report;
        std::optional<double> styleMatch;   // similarity to the locked style, 0..1
    };
    struct Reference {
        Image image;
        QString name;
        style::RefRole role = style::RefRole::Style;
    };

    QWidget* buildForm();
    QWidget* buildResults();
    QWidget* buildStyleBox(QWidget* parent);
    void refreshStyles();
    std::optional<style::StyleProfile> lockedStyle() const;
    void refreshModels();
    void refreshReferences();
    void generate(const std::vector<Image>& extraRefs = {});
    // Queues one generation; returns the job id, or "" when it could not start.
    // A pack view (viewWords set) runs alongside, at a fixed seed and size.
    std::string submitJob(const std::vector<Image>& extraRefs, const std::string& viewWords = {},
                          std::optional<long long> seed = {}, std::optional<std::pair<int, int>> size = {});
    void startCharacterPack(const Result& base);
    void onPackJobFinished(const std::string& id);
    void finishPack();
    void onJobChanged(const QString& id);
    void onJobFinished(const QString& id);
    void showResult(int index);
    gen::Spec spec() const;
    gen::CheckOptions checkOptions() const;
    std::pair<int, int> outputSize() const;
    bool pixelStyle() const;
    Result* selected();

    AiService& m_ai;
    AppContext& m_ctx;
    QPointer<DocumentView> m_view;

    QPlainTextEdit* m_prompt = nullptr;
    QComboBox* m_type = nullptr;
    QComboBox* m_style = nullptr;
    QComboBox* m_perspective = nullptr;
    QComboBox* m_size = nullptr;
    QSpinBox* m_customW = nullptr;
    QSpinBox* m_customH = nullptr;
    QComboBox* m_palette = nullptr;
    QCheckBox* m_transparent = nullptr;
    QComboBox* m_model = nullptr;
    QComboBox* m_preset = nullptr;
    QLabel* m_presetInfo = nullptr;
    QSpinBox* m_count = nullptr;
    QSpinBox* m_seed = nullptr;
    QListWidget* m_refs = nullptr;
    QComboBox* m_role = nullptr;
    QComboBox* m_styleCombo = nullptr;
    QCheckBox* m_lockStyle = nullptr;
    QComboBox* m_styleStrength = nullptr;
    QLabel* m_styleInfo = nullptr;
    style::StyleLibrary m_styles;
    QPushButton* m_generate = nullptr;
    QPushButton* m_cancel = nullptr;
    QProgressBar* m_progress = nullptr;
    QLabel* m_status = nullptr;
    QListWidget* m_results = nullptr;
    QLabel* m_preview = nullptr;
    QLabel* m_report = nullptr;
    QList<QPushButton*> m_resultActions;

    // Character pack: a side view plus generated views for the other
    // directions, each animated with the chosen actions.
    struct Pack {
        QString name;
        std::map<std::string, Image> views;          // direction key ("e", "s", ...) -> sprite
        std::map<std::string, std::string> jobs;     // job id -> direction key
        std::vector<std::string> directions;         // all directions in output order
        std::vector<::acestudio::motion::Action> actions;
        QStringList problems;
    };
    std::optional<Pack> m_pack;

    std::vector<Reference> m_references;
    std::vector<Result> m_resultList;
    std::string m_job;
    bool m_jobPixel = false;
};

} // namespace acestudio::ui
