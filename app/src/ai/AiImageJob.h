#pragma once

#include "acestudio/core/Image.h"

#include <QObject>
#include <QString>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace acestudio::ui {

class AiService;

// One SDXL image for a Phase 6 tool (terrain texture, parallax layer): the
// request, its live preview and its result, with Pixel Art XL and the live
// preview decoder used when installed.
class AiImageJob : public QObject {
    Q_OBJECT
public:
    struct Request {
        QString prompt, negative;
        int width = 1024, height = 1024;     // generation size (multiples of 8)
        QString tileable = QStringLiteral("none");   // none | x | xy
        int pixelWidth = 0, pixelHeight = 0;  // 0 = keep the full-size image
        std::vector<Rgba8> palette;           // empty = `colors` median-cut colours
        int colors = 24;
        int steps = 28;
        std::int64_t seed = -1;
        QString label;
    };

    explicit AiImageJob(AiService& ai, QObject* parent = nullptr);
    ~AiImageJob() override;

    // False (with a reason) when SDXL is not installed or the worker cannot start.
    bool start(const Request& r, QString* why);
    void cancel();
    bool running() const { return !m_job.empty(); }

Q_SIGNALS:
    void progress(const QString& message, const QString& previewPng);
    void finished(const acestudio::Image& pixel, const acestudio::Image& raw);
    void failed(const QString& message);

private:
    AiService& m_ai;
    std::string m_job;
    int m_previewSerial = 0;
};

} // namespace acestudio::ui
