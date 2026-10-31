#pragma once

// Conversions between core types and Qt types. Header-only, app-side only:
// the core library never sees Qt.

#include "acestudio/core/Color.h"
#include "acestudio/core/Image.h"

#include <QColor>
#include <QImage>
#include <QString>

#include <filesystem>

namespace acestudio::ui {

inline QColor toQColor(Rgba8 c) { return QColor(c.r, c.g, c.b, c.a); }

// Wraps (copies) a core image into a QImage in non-premultiplied RGBA8.
inline QImage toQImage(const Image& img) {
    if (img.isNull()) return {};
    QImage q(img.rawBytes(), img.width(), img.height(), img.width() * 4, QImage::Format_RGBA8888);
    return q.copy();   // detach from core buffer
}

// Copies a QImage (any format) into a core image.
inline Image fromQImage(const QImage& src) {
    if (src.isNull()) return {};
    const QImage q = src.convertToFormat(QImage::Format_RGBA8888);
    Image img(q.width(), q.height());
    for (int y = 0; y < q.height(); ++y) {
        const uchar* row = q.constScanLine(y);
        for (int x = 0; x < q.width(); ++x)
            img.setPixel(x, y, {row[x * 4], row[x * 4 + 1], row[x * 4 + 2], row[x * 4 + 3]});
    }
    return img;
}

inline std::filesystem::path toPath(const QString& s) {
#ifdef _WIN32
    return std::filesystem::path(s.toStdWString());
#else
    return std::filesystem::path(s.toStdString());
#endif
}

inline QString fromPath(const std::filesystem::path& p) {
#ifdef _WIN32
    return QString::fromStdWString(p.wstring());
#else
    return QString::fromStdString(p.string());
#endif
}

inline QString qs(const std::string& s) { return QString::fromStdString(s); }

} // namespace acestudio::ui
