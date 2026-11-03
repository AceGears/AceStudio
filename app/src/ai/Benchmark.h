#pragma once
// Repeatable benchmark of the generation and animation pipeline (AI Studio ›
// Benchmark…). Runs fixed cases with fixed prompts and seeds through the same
// code path as the Generate button, once with the original pipeline and once
// with the optimised one, and writes report.json, report.md and every image
// and animation to a folder, so before/after numbers and visual quality can
// be compared.
//
// Cases (from the performance brief): a 32x32 pixel character, a 64x64 RPG
// sprite, a 128x128 monster, 8-frame idle and walk cycles of the 64x64 sprite,
// a dragon wing flap, a campfire, and a four-item asset pack.

#include "acestudio/core/Image.h"

#include <QDialog>
#include <QElapsedTimer>

#include <nlohmann/json.hpp>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;

namespace acestudio::ui {

class GeneratorPage;

class BenchmarkRunner : public QDialog {
    Q_OBJECT
public:
    explicit BenchmarkRunner(GeneratorPage& page, QWidget* parent = nullptr);
    ~BenchmarkRunner() override;

private:
    struct Gen {
        std::string id;
        std::string description;
        int size = 64;
        long long seed = 1;
        bool hd = false;
    };
    struct Step {
        std::string mode;        // original | optimised
        std::string kind;        // restart | gen | idle | walk | flap | fire | packItem | packDone
        Gen gen;
    };

    void start();
    void stop(const QString& why);
    void next();
    void onJobFinished(const QString& id);
    void runAnimation(const Step& s);
    void finish();
    void log(const QString& line);
    QString outPath(const std::string& name) const;

    GeneratorPage& m_page;
    QLineEdit* m_folder = nullptr;
    QCheckBox* m_original = nullptr;
    QCheckBox* m_optimised = nullptr;
    QPushButton* m_start = nullptr;
    QPushButton* m_cancel = nullptr;
    QProgressBar* m_progress = nullptr;
    QPlainTextEdit* m_log = nullptr;

    std::vector<Step> m_steps;
    std::size_t m_next = 0;
    bool m_running = false;
    std::string m_job;            // the job of the current gen step
    QElapsedTimer m_stepClock;
    QElapsedTimer m_packClock;
    QString m_dir;
    nlohmann::json m_report;
    std::map<std::string, Image> m_images;   // "<mode>/<case>" -> result
    QMetaObject::Connection m_finishedConn, m_stateConn;
};

} // namespace acestudio::ui
