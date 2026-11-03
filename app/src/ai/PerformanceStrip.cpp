#include "PerformanceStrip.h"

#include "AiService.h"
#include "SystemMonitor.h"
#include "../QtBridge.h"

#include "acestudio/core/AiWorker.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QTimer>
#include <QToolButton>

#include <chrono>

namespace acestudio::ui {

namespace {

QString modelOf(const ai::Job& j) {
    const std::string& t = j.task;
    if (t.rfind("klein", 0) == 0) return QStringLiteral("FLUX.2 klein");
    if (t.rfind("generate", 0) == 0 || t.rfind("sdxl", 0) == 0) return QStringLiteral("SDXL");
    if (t.find("cutout") != std::string::npos || t.find("background") != std::string::npos) return QStringLiteral("BiRefNet");
    if (t.find("upscale") != std::string::npos) return QStringLiteral("Real-ESRGAN");
    return qs(t);
}

QString gb(double v) { return QString::number(v, 'f', 1); }

} // namespace

PerformanceStrip::PerformanceStrip(AiService& ai, QWidget* parent) : QWidget(parent), m_ai(ai) {
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 6, 0);
    row->setSpacing(8);
    m_job = new QLabel(this);
    m_load = new QLabel(this);
    m_load->setToolTip(tr("Whole-PC load while the AI worker runs: CPU, GPU, video memory and RAM"));
    m_pause = new QToolButton(this);
    m_pause->setCheckable(true);
    m_pause->setAutoRaise(true);
    m_pause->setText(tr("Pause AI"));
    m_pause->setToolTip(tr("Pause All AI Jobs: the running job stops between steps and queued jobs wait"));
    m_pause->setAccessibleName(tr("Pause all AI jobs"));
    row->addWidget(m_job);
    row->addWidget(m_load);
    row->addWidget(m_pause);
    connect(m_pause, &QToolButton::toggled, this, [this](bool on) { m_ai.setPaused(on); });
    connect(&m_ai, &AiService::stateChanged, this, &PerformanceStrip::refresh);
    connect(&m_ai, &AiService::jobChanged, this, &PerformanceStrip::refresh);
    connect(&systemMonitor(), &SystemMonitor::sampled, this, &PerformanceStrip::refresh);
    auto* clock = new QTimer(this);   // the job's running time
    clock->setInterval(1000);
    connect(clock, &QTimer::timeout, this, &PerformanceStrip::refresh);
    clock->start();
    refresh();
}

PerformanceStrip::~PerformanceStrip() { setSampling(false); }

void PerformanceStrip::setSampling(bool on) {
    if (on == m_sampling) return;
    m_sampling = on;
    if (on) systemMonitor().acquire();
    else systemMonitor().release();
}

void PerformanceStrip::refresh() {
    const ai::WorkerState st = m_ai.state();
    const bool running = st == ai::WorkerState::Ready || st == ai::WorkerState::Starting || st == ai::WorkerState::Restarting;
    setSampling(running);
    const ai::Job* active = nullptr;
    int queued = 0;
    if (auto* sup = m_ai.supervisor())
        for (const ai::Job* j : sup->jobs()) {
            if (j->state == ai::JobState::Running) active = j;
            else if (j->state == ai::JobState::Queued) ++queued;
        }
    if (m_pause->isChecked() != m_ai.paused()) {
        const QSignalBlocker b(m_pause);
        m_pause->setChecked(m_ai.paused());
    }
    m_pause->setText(m_ai.paused() ? tr("Resume AI") : tr("Pause AI"));
    m_pause->setVisible(running || m_ai.paused());
    QString job;
    if (active) {
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(ai::Clock::now() - active->started).count();
        job = tr("%1 · %2 · %3:%4").arg(modelOf(*active), qs(active->label.empty() ? active->task : active->label).left(28))
                  .arg(secs / 60).arg(secs % 60, 2, 10, QLatin1Char('0'));
        if (m_ai.paused()) job += tr(" · paused");
        if (queued > 0) job += tr(" · %n queued", nullptr, queued);
    } else if (running) {
        job = st == ai::WorkerState::Ready ? tr("AI ready · %1").arg(m_ai.profileName()) : tr("AI starting…");
    }
    m_job->setText(job);
    m_job->setVisible(!job.isEmpty());
    if (!running) {
        m_load->setVisible(false);
        return;
    }
    const SystemSample& s = systemMonitor().latest();
    QStringList parts;
    if (s.cpuPercent >= 0) parts << tr("CPU %1%").arg(qRound(s.cpuPercent));
    if (s.gpuPercent >= 0) parts << tr("GPU %1%").arg(qRound(s.gpuPercent));
    if (s.vramTotalGb > 0) parts << tr("VRAM %1/%2 GB").arg(gb(s.vramUsedGb), gb(s.vramTotalGb));
    if (s.ramTotalGb > 0) parts << tr("RAM %1/%2 GB").arg(gb(s.ramUsedGb), gb(s.ramTotalGb));
    m_load->setText(parts.join(QStringLiteral("  ")));
    m_load->setVisible(!parts.isEmpty());
}

} // namespace acestudio::ui
