#pragma once

#include "acestudio/core/AssetFiles.h"
#include "acestudio/core/Image.h"
#include "acestudio/core/Result.h"

#include <QImage>
#include <QWidget>

#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

class QCheckBox;

namespace acestudio::ui {

class DocumentView;

// The current frame of a document, flattened, cropped to the selection if
// there is one. Null if there is no document.
Image documentImage(const DocumentView* view);

// Square crop from the centre, scaled (nearest) to size x size.
Image fitSquare(const Image& img, int size);

Status writeTextFile(const std::filesystem::path& path, const std::string& text);

// "Also write files for: [ ] Godot [ ] Unity [ ] Tiled", with the engines an
// export supports. The choice is remembered for the session.
class EngineChoice : public QWidget {
    Q_OBJECT
public:
    EngineChoice(unsigned available, QWidget* parent = nullptr);
    unsigned engines() const;

private:
    std::vector<std::pair<unsigned, QCheckBox*>> m_boxes;
};

// Writes asset files through `write` (an AssetFiles writer): a dry run first,
// one question before replacing existing files other than `confirmedFile`
// (which the save dialog already asked about), then the write and a report
// that includes the writer's notes. Returns false if nothing was written.
bool writeAssetFiles(QWidget* parent, AssetWriteOptions opts, const std::filesystem::path& confirmedFile,
                     const std::function<Result<WrittenFiles>(const AssetWriteOptions&)>& write);

// Return in a spin box or combo box must not press a dialog button (it used
// to add a terrain level): turns off auto-default on every button inside.
void noDefaultButtons(QWidget* dialog);

// Shows an image with integer nearest-neighbour zoom on a checkerboard; the
// zoom fits the widget unless fixed.
class PixelView : public QWidget {
    Q_OBJECT
public:
    explicit PixelView(QWidget* parent = nullptr);
    void setImage(const Image& img);
    void setImage(const QImage& img);
    void setRepeat(int n);           // draw the image n x n times (seamless preview)
    void setFixedZoom(int z);        // 0 = fit
    const QImage& image() const { return m_img; }
    QSize sizeHint() const override { return {256, 256}; }
    int zoom() const;
    double scale() const;            // display scale: integer when the image fits, below 1 when it does not

Q_SIGNALS:
    void pressed(QPoint imagePos, Qt::MouseButton button);
    void dragged(QPoint imagePos, Qt::MouseButtons buttons);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;

private:
    QPoint toImage(QPointF p) const;
    QImage m_img;
    int m_repeat = 1;
    int m_fixedZoom = 0;
};

} // namespace acestudio::ui
