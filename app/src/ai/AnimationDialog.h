#pragma once

#include "acestudio/core/AnimationQA.h"
#include "acestudio/core/Character.h"
#include "acestudio/core/Pose.h"

#include <QDialog>
#include <QPointer>

#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;

namespace acestudio::ui {

class AiService;
class AppContext;
class DocumentView;

// Phase 5 animation pipeline: generate -> evaluate -> correct -> commit.
// Every frame is one pose-conditioned SDXL job sharing the character's seed,
// prompt, palette and reference images, pixelized through one fixed framing
// box. Results are scored (ground line, height, palette, colour mix,
// silhouette); fixable problems are corrected, the rest regenerated, and the
// result is committed as a new layer + tag in one undo step.
class AnimationDialog : public QDialog {
    Q_OBJECT
public:
    using NewDocumentFn = std::function<void(const std::vector<Image>&, const std::vector<int>&, const QString&)>;
    AnimationDialog(AppContext& ctx, AiService& ai, DocumentView* view, NewDocumentFn newDocument, QWidget* parent = nullptr);
    ~AnimationDialog() override;

private:
    void reloadCharacters(const QString& selectId = {});
    void resetPoses();
    void refreshStrip();
    void updateButtons();
    bool checkModels(QString* missing) const;
    void generate(const std::vector<int>& frames, bool newSeed);
    void onJobFinished(const QString& id);
    void evaluate();
    QAOptions qaOptions() const;
    void autoCorrect();
    void commit();
    void editPose(int frame);

    AppContext& m_ctx;
    AiService& m_ai;
    QPointer<DocumentView> m_view;
    NewDocumentFn m_newDocument;
    std::filesystem::path m_session;          // pose and reference PNGs for this dialog's jobs

    std::optional<CharacterProfile> m_profile;
    std::vector<Pose> m_poses;
    std::vector<std::optional<Image>> m_results;
    std::vector<QString> m_errors;
    std::map<std::string, int> m_jobs;        // job id -> frame
    std::map<int, int> m_dupOf;               // frame -> frame whose job it shares (same pose and attempt)
    std::vector<int> m_attempts;
    std::optional<QAReport> m_report;
    std::int64_t m_seed = -1;
    std::optional<std::array<double, 4>> m_box;   // framing box of the last generation
    int m_previewSerial = 0;
    bool m_corrected = false;

    QComboBox* m_character = nullptr;
    QComboBox* m_action = nullptr;
    QSpinBox* m_frames = nullptr;
    QSpinBox* m_duration = nullptr;
    QLineEdit* m_actionWords = nullptr;
    QSpinBox* m_steps = nullptr;
    QSpinBox* m_poseStrength = nullptr;
    QComboBox* m_precision = nullptr;
    QCheckBox* m_useRefs = nullptr;
    QCheckBox* m_removeBg = nullptr;
    QListWidget* m_strip = nullptr;
    QLabel* m_status = nullptr;
    QPushButton* m_generate = nullptr;
    QPushButton* m_regen = nullptr;
    QPushButton* m_correct = nullptr;
    QPushButton* m_commit = nullptr;
    QPushButton* m_cancel = nullptr;
};

} // namespace acestudio::ui
