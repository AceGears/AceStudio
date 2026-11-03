#pragma once
// System load as the user sees it: CPU and RAM of the whole PC, GPU use and
// VRAM (from nvidia-smi, when the NVIDIA driver provides it), and how late
// the UI thread runs its timers (a direct measure of responsiveness).
//
// Sampling is cheap (one nvidia-smi query per second, run asynchronously)
// and only runs while something is watching (the performance monitor is
// open, or a benchmark is recording).

#include <QElapsedTimer>
#include <QObject>

#include <optional>

class QProcess;
class QTimer;

namespace acestudio::ui {

struct SystemSample {
    double cpuPercent = -1;        // whole PC, 0..100; -1 = unknown
    double ramUsedGb = -1, ramTotalGb = -1;
    double gpuPercent = -1;        // GPU 0 utilisation; -1 = unknown
    double vramUsedGb = -1, vramTotalGb = -1;
    double uiLagMs = 0;            // worst timer lateness in the last second
};

struct SystemStats {               // over a recording
    int samples = 0;
    double cpuAvg = 0, cpuPeak = 0;
    double ramPeakGb = 0;
    double gpuAvg = 0, gpuPeak = 0;
    double vramPeakGb = 0;
    double uiLagAvgMs = 0, uiLagPeakMs = 0;
    bool gpuKnown = false;
};

class SystemMonitor : public QObject {
    Q_OBJECT
public:
    explicit SystemMonitor(QObject* parent = nullptr);
    ~SystemMonitor() override;

    // Reference-counted: sampling runs while at least one user is active.
    void acquire();
    void release();

    const SystemSample& latest() const { return m_latest; }

    void startRecording();
    SystemStats stopRecording();
    bool recording() const { return m_recording; }

Q_SIGNALS:
    void sampled();

private:
    void tick();
    void lagTick();
    void readCpuRam();
    void startGpuQuery();

    QTimer* m_timer = nullptr;
    QTimer* m_lagTimer = nullptr;
    QElapsedTimer m_lagClock;
    double m_lagWorst = 0;
    QProcess* m_smi = nullptr;
    bool m_smiMissing = false;
    int m_users = 0;
    SystemSample m_latest;
    // CPU time counters from the previous sample.
    unsigned long long m_prevIdle = 0, m_prevTotal = 0;
    bool m_recording = false;
    SystemStats m_rec;
    double m_cpuSum = 0, m_gpuSum = 0, m_lagSum = 0;
    int m_gpuSamples = 0;
};

// The app-wide monitor (created on first use, owned by the application).
SystemMonitor& systemMonitor();

} // namespace acestudio::ui
