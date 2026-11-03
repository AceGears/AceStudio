#pragma once
// Compact performance monitor for the status bar: the active model and job
// with its running time, CPU, GPU, VRAM and RAM of the PC, and Pause All.
// It samples only while the AI worker is running.

#include <QWidget>

class QLabel;
class QToolButton;

namespace acestudio::ui {

class AiService;

class PerformanceStrip : public QWidget {
    Q_OBJECT
public:
    explicit PerformanceStrip(AiService& ai, QWidget* parent = nullptr);
    ~PerformanceStrip() override;

private:
    void refresh();
    void setSampling(bool on);

    AiService& m_ai;
    QLabel* m_job = nullptr;
    QLabel* m_load = nullptr;
    QToolButton* m_pause = nullptr;
    bool m_sampling = false;
};

} // namespace acestudio::ui
