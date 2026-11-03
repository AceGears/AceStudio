#include "SystemMonitor.h"

#include <QCoreApplication>
#include <QFile>
#include <QProcess>
#include <QStringList>
#include <QTimer>

#include <algorithm>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace acestudio::ui {

namespace {
constexpr int kIntervalMs = 1000;
constexpr int kLagIntervalMs = 20;
} // namespace

SystemMonitor::SystemMonitor(QObject* parent) : QObject(parent) {
    m_timer = new QTimer(this);
    m_timer->setInterval(kIntervalMs);
    connect(m_timer, &QTimer::timeout, this, &SystemMonitor::tick);
    m_lagTimer = new QTimer(this);
    m_lagTimer->setTimerType(Qt::PreciseTimer);
    m_lagTimer->setInterval(kLagIntervalMs);
    connect(m_lagTimer, &QTimer::timeout, this, &SystemMonitor::lagTick);
}

SystemMonitor::~SystemMonitor() {
    if (m_smi && m_smi->state() != QProcess::NotRunning) {
        m_smi->kill();
        m_smi->waitForFinished(500);
    }
}

void SystemMonitor::acquire() {
    if (m_users++ == 0) {
        readCpuRam();   // prime the CPU counters
        m_lagWorst = 0;
        m_lagClock.start();
        m_timer->start();
        m_lagTimer->start();
    }
}

void SystemMonitor::release() {
    if (m_users > 0 && --m_users == 0) {
        m_timer->stop();
        m_lagTimer->stop();
    }
}

void SystemMonitor::lagTick() {
    const double elapsed = static_cast<double>(m_lagClock.restart());
    m_lagWorst = std::max(m_lagWorst, elapsed - kLagIntervalMs);
}

void SystemMonitor::readCpuRam() {
#ifdef _WIN32
    FILETIME idle, kernel, user;
    if (GetSystemTimes(&idle, &kernel, &user)) {
        auto v = [](const FILETIME& f) { return (static_cast<unsigned long long>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
        const unsigned long long i = v(idle), total = v(kernel) + v(user);   // kernel time includes idle
        if (m_prevTotal && total > m_prevTotal)
            m_latest.cpuPercent = 100.0 * (1.0 - static_cast<double>(i - m_prevIdle) / static_cast<double>(total - m_prevTotal));
        m_prevIdle = i;
        m_prevTotal = total;
    }
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        m_latest.ramTotalGb = static_cast<double>(ms.ullTotalPhys) / (1ull << 30);
        m_latest.ramUsedGb = static_cast<double>(ms.ullTotalPhys - ms.ullAvailPhys) / (1ull << 30);
    }
#else
    QFile stat(QStringLiteral("/proc/stat"));
    if (stat.open(QIODevice::ReadOnly)) {
        const QStringList f = QString::fromLatin1(stat.readLine()).simplified().split(QLatin1Char(' '));
        if (f.size() >= 8) {
            unsigned long long total = 0;
            for (int k = 1; k < f.size(); ++k) total += f[k].toULongLong();
            const unsigned long long i = f[4].toULongLong() + f[5].toULongLong();   // idle + iowait
            if (m_prevTotal && total > m_prevTotal)
                m_latest.cpuPercent = 100.0 * (1.0 - static_cast<double>(i - m_prevIdle) / static_cast<double>(total - m_prevTotal));
            m_prevIdle = i;
            m_prevTotal = total;
        }
    }
    QFile mem(QStringLiteral("/proc/meminfo"));
    if (mem.open(QIODevice::ReadOnly)) {
        double total = -1, avail = -1;
        while (!mem.atEnd()) {
            const QString line = QString::fromLatin1(mem.readLine());
            if (line.startsWith(QLatin1String("MemTotal:"))) total = line.section(QLatin1Char(' '), -2, -2).toDouble();
            if (line.startsWith(QLatin1String("MemAvailable:"))) avail = line.section(QLatin1Char(' '), -2, -2).toDouble();
        }
        if (total > 0 && avail >= 0) {
            m_latest.ramTotalGb = total / (1 << 20);
            m_latest.ramUsedGb = (total - avail) / (1 << 20);
        }
    }
#endif
}

void SystemMonitor::startGpuQuery() {
    if (m_smiMissing || (m_smi && m_smi->state() != QProcess::NotRunning)) return;
    if (!m_smi) {
        m_smi = new QProcess(this);
        connect(m_smi, &QProcess::finished, this, [this](int code, QProcess::ExitStatus st) {
            if (st != QProcess::NormalExit || code != 0) return;
            // "utilization.gpu, memory.used, memory.total" in % and MiB.
            const QStringList f = QString::fromLatin1(m_smi->readAllStandardOutput()).section(QLatin1Char('\n'), 0, 0).split(QLatin1Char(','));
            if (f.size() < 3) return;
            m_latest.gpuPercent = f[0].trimmed().toDouble();
            m_latest.vramUsedGb = f[1].trimmed().toDouble() / 1024.0;
            m_latest.vramTotalGb = f[2].trimmed().toDouble() / 1024.0;
        });
        connect(m_smi, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
            if (e == QProcess::FailedToStart) m_smiMissing = true;   // no NVIDIA driver tools
        });
    }
    m_smi->start(QStringLiteral("nvidia-smi"),
                 {QStringLiteral("--query-gpu=utilization.gpu,memory.used,memory.total"), QStringLiteral("--format=csv,noheader,nounits")});
}

void SystemMonitor::tick() {
    readCpuRam();
    startGpuQuery();   // its answer lands in m_latest for the next tick
    m_latest.uiLagMs = m_lagWorst;
    m_lagWorst = 0;
    if (m_recording) {
        ++m_rec.samples;
        if (m_latest.cpuPercent >= 0) {
            m_cpuSum += m_latest.cpuPercent;
            m_rec.cpuPeak = std::max(m_rec.cpuPeak, m_latest.cpuPercent);
        }
        m_rec.ramPeakGb = std::max(m_rec.ramPeakGb, m_latest.ramUsedGb);
        if (m_latest.gpuPercent >= 0) {
            m_rec.gpuKnown = true;
            ++m_gpuSamples;
            m_gpuSum += m_latest.gpuPercent;
            m_rec.gpuPeak = std::max(m_rec.gpuPeak, m_latest.gpuPercent);
            m_rec.vramPeakGb = std::max(m_rec.vramPeakGb, m_latest.vramUsedGb);
        }
        m_lagSum += m_latest.uiLagMs;
        m_rec.uiLagPeakMs = std::max(m_rec.uiLagPeakMs, m_latest.uiLagMs);
    }
    Q_EMIT sampled();
}

void SystemMonitor::startRecording() {
    acquire();
    m_recording = true;
    m_rec = {};
    m_cpuSum = m_gpuSum = m_lagSum = 0;
    m_gpuSamples = 0;
}

SystemStats SystemMonitor::stopRecording() {
    if (!m_recording) return {};
    m_recording = false;
    SystemStats s = m_rec;
    if (s.samples > 0) {
        s.cpuAvg = m_cpuSum / s.samples;
        s.uiLagAvgMs = m_lagSum / s.samples;
    }
    if (m_gpuSamples > 0) s.gpuAvg = m_gpuSum / m_gpuSamples;
    release();
    return s;
}

SystemMonitor& systemMonitor() {
    static SystemMonitor* monitor = new SystemMonitor(QCoreApplication::instance());
    return *monitor;
}

} // namespace acestudio::ui
