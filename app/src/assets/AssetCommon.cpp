#include "AssetCommon.h"
#include "../licensing/Licensing.h"

#include "../DocumentView.h"
#include "../QtBridge.h"

#include "acestudio/core/FileUtil.h"
#include "acestudio/core/Transform.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>

#include <algorithm>
#include <cmath>

namespace acestudio::ui {

Image documentImage(const DocumentView* view) {
    if (!view) return {};
    Image img = view->document().composite(view->currentFrame());
    if (const Mask* sel = view->selection()) {
        const IntRect b = sel->bounds();
        if (!b.empty()) img = img.copy(b);
    }
    return img;
}

Image fitSquare(const Image& img, int size) {
    if (img.isNull() || size <= 0) return {};
    const int side = std::min(img.width(), img.height());
    Image sq = img.copy({(img.width() - side) / 2, (img.height() - side) / 2, side, side});
    if (side == size) return sq;
    auto s = scaleNearest(sq, size, size);
    return s ? std::move(s).value() : Image();
}

Status writeTextFile(const std::filesystem::path& path, const std::string& text) {
    return writeFileAtomic(path, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

namespace {
unsigned s_lastEngines = kEngineNone;
}

EngineChoice::EngineChoice(unsigned available, QWidget* parent) : QWidget(parent) {
    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(new QLabel(tr("Also for:"), this));
    const std::pair<unsigned, QString> all[] = {{kEngineGodot, tr("Godot")},
                                                {kEngineUnity, tr("Unity")},
                                                {kEngineUnityRuleTiles, tr("+ Rule Tiles")},
                                                {kEngineTiled, tr("Tiled")}};
    for (const auto& [bit, label] : all) {
        if (!(available & bit)) continue;
        auto* box = new QCheckBox(label, this);
        box->setChecked(s_lastEngines & bit);
        connect(box, &QCheckBox::toggled, this, [bit = bit](bool on) {
            if (on) s_lastEngines |= bit;
            else s_lastEngines &= ~bit;
        });
        if (bit == kEngineUnityRuleTiles)
            box->setToolTip(tr("Also Unity Rule Tiles (_ruletile.asset) for Blob47 and 4-neighbour sets. They need Unity's 2D "
                               "Tilemap Extras package; AceStudio's own Unity tiles work without it."));
        m_boxes.push_back({bit, box});
        lay->addWidget(box);
    }
    setToolTip(tr("Godot: .tres resources (SpriteFrames, TileSet terrains, StyleBoxTexture). Unity: .meta files with "
                  "sprites, borders and pivots, .anim clips, and tiles that paint like AceStudio (no package needed). "
                  "Tiled: .tsx tilesets with Wang sets and tile animations."));
}

unsigned EngineChoice::engines() const {
    unsigned e = kEngineNone;
    for (const auto& [bit, box] : m_boxes)
        if (box->isChecked()) e |= bit;
    if (e & kEngineUnityRuleTiles) e |= kEngineUnity;
    return e;
}

bool writeAssetFiles(QWidget* parent, AssetWriteOptions opts, const std::filesystem::path& confirmedFile,
                     const std::function<Result<WrittenFiles>(const AssetWriteOptions&)>& write) {
    if (!licenceAllows(parent, QObject::tr("Exporting"))) return false;
    const QString title = parent ? parent->windowTitle() : QString();
    AssetWriteOptions dry = opts;
    dry.dryRun = true;
    auto plan = write(dry);
    if (!plan) {
        QMessageBox::warning(parent, title, qs(plan.error().message));
        return false;
    }
    QStringList others;
    for (const auto& p : plan.value().existing)
        if (p != confirmedFile) others << fromPath(p.filename());
    if (!others.isEmpty()) {
        const QString list = others.size() > 8 ? QObject::tr("%n file(s)", nullptr, static_cast<int>(others.size())) : others.join('\n');
        if (QMessageBox::question(parent, title, QObject::tr("These files already exist. Replace them?\n\n%1").arg(list),
                                  QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
            return false;
    }
    opts.overwrite = true;   // everything that exists was confirmed
    opts.dryRun = false;
    auto done = write(opts);
    if (!done) {
        QMessageBox::warning(parent, title, qs(done.error().message));
        return false;
    }
    QStringList names;
    for (const auto& p : done.value().files) names << fromPath(p.filename());
    QString text = QObject::tr("Wrote %1.").arg(names.join(QStringLiteral(", ")));
    for (const auto& n : done.value().notes) text += "\n\n" + qs(n);
    QMessageBox::information(parent, title, text);
    return true;
}

void noDefaultButtons(QWidget* dialog) {
    for (QPushButton* b : dialog->findChildren<QPushButton*>()) {
        b->setAutoDefault(false);
        b->setDefault(false);
    }
}

PixelView::PixelView(QWidget* parent) : QWidget(parent) {
    setMinimumSize(96, 96);
    setMouseTracking(false);
}

void PixelView::setImage(const Image& img) { setImage(toQImage(img)); }

void PixelView::setImage(const QImage& img) {
    m_img = img;
    update();
}

void PixelView::setRepeat(int n) {
    m_repeat = std::max(1, n);
    update();
}

void PixelView::setFixedZoom(int z) {
    m_fixedZoom = std::max(0, z);
    update();
}

int PixelView::zoom() const { return std::max(1, static_cast<int>(scale())); }

// Integer zoom while the image fits; scaled down (still without smoothing)
// when it is larger than the widget, so it is never cut off.
double PixelView::scale() const {
    if (m_img.isNull()) return 1;
    if (m_fixedZoom > 0) return m_fixedZoom;
    const double sw = double(width()) / (m_img.width() * m_repeat), sh = double(height()) / (m_img.height() * m_repeat);
    const double s = std::min(sw, sh);
    return s >= 1 ? std::floor(s) : std::max(s, 0.01);
}

QPoint PixelView::toImage(QPointF p) const {
    const double z = scale();
    const double w = m_img.width() * m_repeat * z, h = m_img.height() * m_repeat * z;
    const double ox = std::floor((width() - w) / 2), oy = std::floor((height() - h) / 2);
    return {static_cast<int>(std::floor((p.x() - ox) / z)), static_cast<int>(std::floor((p.y() - oy) / z))};
}

void PixelView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), palette().window());
    if (m_img.isNull()) return;
    const double z = scale();
    const int w = static_cast<int>(std::lround(m_img.width() * m_repeat * z)), h = static_cast<int>(std::lround(m_img.height() * m_repeat * z));
    const int ox = static_cast<int>(std::floor((width() - m_img.width() * m_repeat * z) / 2)),
              oy = static_cast<int>(std::floor((height() - m_img.height() * m_repeat * z) / 2));
    const int cs = 8;
    for (int y = 0; y < h; y += cs)
        for (int x = 0; x < w; x += cs)
            p.fillRect(ox + x, oy + y, std::min(cs, w - x), std::min(cs, h - y), ((x / cs + y / cs) % 2) ? QColor(60, 60, 64) : QColor(80, 80, 86));
    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    const double tw = m_img.width() * z, th = m_img.height() * z;
    for (int j = 0; j < m_repeat; ++j)
        for (int i = 0; i < m_repeat; ++i)
            p.drawImage(QRectF(ox + i * tw, oy + j * th, tw, th), m_img);
}

void PixelView::mousePressEvent(QMouseEvent* e) { Q_EMIT pressed(toImage(e->position()), e->button()); }

void PixelView::mouseMoveEvent(QMouseEvent* e) {
    if (e->buttons() != Qt::NoButton) Q_EMIT dragged(toImage(e->position()), e->buttons());
}

} // namespace acestudio::ui
