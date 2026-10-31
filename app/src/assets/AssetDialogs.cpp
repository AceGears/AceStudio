#include "AssetDialogs.h"

#include "AssetCommon.h"

#include "../DocumentView.h"
#include "../QtBridge.h"
#include "../ai/AiImageJob.h"

#include "acestudio/core/Autotile.h"
#include "acestudio/core/PngCodec.h"
#include "acestudio/core/Transform.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImageReader>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <map>
#include <tuple>
#include <cmath>
#include <deque>

namespace acestudio::ui {

namespace fs = std::filesystem;

namespace {

std::optional<Image> openImageFile(QWidget* parent, const QString& title, QString* name = nullptr) {
    const QString f = QFileDialog::getOpenFileName(parent, title, QString(), QObject::tr("Images (*.png *.jpg *.jpeg *.bmp *.webp *.gif)"));
    if (f.isEmpty()) return std::nullopt;
    QImageReader r(f);
    const QImage q = r.read();
    if (q.isNull()) {
        QMessageBox::warning(parent, title, QObject::tr("Could not read %1: %2").arg(f, r.errorString()));
        return std::nullopt;
    }
    if (name) *name = QFileInfo(f).completeBaseName();
    return fromQImage(q);
}

fs::path withExtension(const QString& f, const char* ext) {
    fs::path p = toPath(f);
    if (p.extension() != ext) p += ext;
    return p;
}

int colourDistance(Rgba8 a, Rgba8 b) {
    return std::max({std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b), std::abs(a.a - b.a)});
}

QIcon swatch(Rgba8 c) {
    QPixmap p(16, 16);
    p.fill(QColor(c.r, c.g, c.b, c.a));
    return QIcon(p);
}

} // namespace

// ================================================================ parallax

ParallaxDialog::ParallaxDialog(AiService& ai, DocumentView* view, QWidget* parent) : QDialog(parent), m_ai(ai), m_view(view) {
    setWindowTitle(tr("Parallax Background"));
    m_viewW = new QSpinBox(this);
    m_viewW->setRange(32, 4096);
    m_viewW->setValue(m_set.viewWidth);
    m_viewH = new QSpinBox(this);
    m_viewH->setRange(32, 4096);
    m_viewH->setValue(m_set.viewHeight);
    if (view) {   // default to the document's size, a common game resolution
        m_viewW->setValue(view->document().width());
        m_viewH->setValue(view->document().height());
    }
    m_list = new QListWidget(this);
    m_list->setToolTip(tr("Back (top of the list) to front"));
    auto* fromDoc = new QPushButton(tr("From Document"), this);
    auto* fromFile = new QPushButton(tr("Open…"), this);
    auto* remove = new QPushButton(tr("Remove"), this);
    auto* up = new QPushButton(tr("Back ↑"), this);
    auto* down = new QPushButton(tr("Front ↓"), this);
    m_scroll = new QDoubleSpinBox(this);
    m_scroll->setRange(0.0, 2.0);
    m_scroll->setSingleStep(0.05);
    m_scroll->setToolTip(tr("0 = fixed (sky), 1 = moves with the camera, between = far away"));
    m_y = new QSpinBox(this);
    m_y->setRange(-4096, 4096);
    m_repeat = new QCheckBox(tr("Repeat horizontally"), this);
    m_overlap = new QSpinBox(this);
    m_overlap->setRange(2, 512);
    m_overlap->setValue(24);
    m_overlap->setToolTip(tr("Width of the dithered blend made over the wrap"));
    auto* seamless = new QPushButton(tr("Make Seamless"), this);
    auto* sky = new QPushButton(tr("Clear Sky"), this);
    sky->setToolTip(tr("Make the flat colour at the top-left, where it touches the top edge, transparent (for layers in front)"));
    m_skyTolerance = new QSpinBox(this);
    m_skyTolerance->setRange(0, 255);
    m_skyTolerance->setValue(24);
    m_skyTolerance->setToolTip(tr("How far from the sky colour a pixel may be and still be cleared"));
    m_skyMode = new QComboBox(this);
    m_skyMode->addItem(tr("Sky against ground"), static_cast<int>(SkyClear::Split));
    m_skyMode->addItem(tr("Flat colour"), static_cast<int>(SkyClear::Flat));
    m_skyMode->addItem(tr("Follow gradient"), static_cast<int>(SkyClear::Gradient));
    m_skyMode->addItem(tr("Every matching pixel"), static_cast<int>(SkyClear::Every));
    m_skyMode->setToolTip(tr("Sky against ground: the top edge is sky, the bottom edge ground, every pixel goes to the side it "
                             "reaches more easily; handles banded skies, no tolerance. Flat: one colour touching the top. "
                             "Follow gradient: smooth skies. Every matching pixel: also gaps between trees"));
    m_undo = new QPushButton(tr("Undo"), this);
    m_undo->setToolTip(tr("Undo the last Clear Sky, Make Seamless or background removal"));
    m_undo->setEnabled(false);
    m_aiCut = new QPushButton(tr("Remove Background (AI)"), this);
    m_aiCut->setToolTip(tr("Cut the layer out with BiRefNet: for skies Clear Sky cannot handle"));
    m_prompt = new QLineEdit(tr("distant purple mountains at dusk"), this);
    m_generate = new QPushButton(tr("Generate Layer"), this);
    m_generate->setToolTip(tr("A horizontally seamless layer from Stable Diffusion XL, reduced to the view width"));
    m_genStatus = new QLabel(this);
    m_genStatus->setWordWrap(true);

    auto* layerForm = new QFormLayout;
    layerForm->addRow(tr("Scroll factor:"), m_scroll);
    layerForm->addRow(tr("Y position:"), m_y);
    layerForm->addRow(QString(), m_repeat);
    auto* seamRow = new QHBoxLayout;
    seamRow->addWidget(m_overlap);
    seamRow->addWidget(seamless);
    seamRow->addWidget(sky);
    layerForm->addRow(tr("Blend width:"), seamRow);
    auto* skyRow = new QHBoxLayout;
    skyRow->addWidget(m_skyTolerance);
    skyRow->addWidget(m_skyMode, 1);
    layerForm->addRow(tr("Sky tolerance:"), skyRow);
    auto* cutRow = new QHBoxLayout;
    cutRow->addWidget(m_aiCut, 1);
    cutRow->addWidget(m_undo);
    layerForm->addRow(QString(), cutRow);
    auto* listButtons = new QHBoxLayout;
    for (auto* b : {fromDoc, fromFile, remove}) listButtons->addWidget(b);
    auto* orderButtons = new QHBoxLayout;
    orderButtons->addWidget(up);
    orderButtons->addWidget(down);
    auto* genRow = new QHBoxLayout;
    genRow->addWidget(m_prompt, 1);
    genRow->addWidget(m_generate);
    auto* viewRow = new QHBoxLayout;
    viewRow->addWidget(new QLabel(tr("View"), this));
    viewRow->addWidget(m_viewW);
    viewRow->addWidget(new QLabel(QStringLiteral("×"), this));
    viewRow->addWidget(m_viewH);
    viewRow->addStretch();
    auto* left = new QVBoxLayout;
    left->addLayout(viewRow);
    left->addWidget(m_list, 1);
    left->addLayout(listButtons);
    left->addLayout(orderButtons);
    left->addLayout(layerForm);
    left->addLayout(genRow);
    left->addWidget(m_genStatus);

    m_preview = new PixelView(this);
    m_preview->setMinimumSize(480, 270);
    m_play = new QCheckBox(tr("Play"), this);
    m_play->setChecked(true);
    m_speed = new QSpinBox(this);
    m_speed->setRange(1, 400);
    m_speed->setValue(40);
    m_speed->setSuffix(tr(" px/s"));
    m_issues = new QLabel(this);
    m_issues->setWordWrap(true);
    auto* exportBtn = new QPushButton(tr("&Export…"), this);
    auto* close = new QPushButton(tr("Close"), this);
    auto* playRow = new QHBoxLayout;
    playRow->addWidget(m_play);
    playRow->addWidget(new QLabel(tr("Camera speed"), this));
    playRow->addWidget(m_speed);
    playRow->addStretch();
    playRow->addWidget(exportBtn);
    playRow->addWidget(close);
    auto* right = new QVBoxLayout;
    right->addWidget(m_preview, 1);
    right->addWidget(m_issues);
    right->addLayout(playRow);
    auto* root = new QHBoxLayout(this);
    root->addLayout(left);
    root->addLayout(right, 1);

    m_timer = new QTimer(this);
    m_timer->setInterval(33);
    connect(m_timer, &QTimer::timeout, this, [this] {
        m_camera += m_speed->value() * 0.033;
        redraw();
    });
    m_timer->start();
    connect(m_play, &QCheckBox::toggled, this, [this](bool on) { on ? m_timer->start() : m_timer->stop(); });
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    connect(exportBtn, &QPushButton::clicked, this, &ParallaxDialog::exportFiles);
    connect(m_viewW, &QSpinBox::valueChanged, this, [this](int v) { m_set.viewWidth = v; redraw(); });
    connect(m_viewH, &QSpinBox::valueChanged, this, [this](int v) { m_set.viewHeight = v; redraw(); });
    connect(fromDoc, &QPushButton::clicked, this, [this] {
        const Image img = documentImage(m_view);
        if (img.isNull()) { QMessageBox::information(this, windowTitle(), tr("Open a document first.")); return; }
        addLayer(img, m_view ? m_view->displayName() : tr("layer"));
    });
    connect(fromFile, &QPushButton::clicked, this, [this] {
        QString name;
        if (auto img = openImageFile(this, tr("Open Layer Image"), &name)) addLayer(*img, name);
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        const int i = current();
        if (i < 0) return;
        m_set.layers.erase(m_set.layers.begin() + i);
        m_undoStack.clear();
        m_undo->setEnabled(false);
        refreshList();
    });
    auto move = [this](int d) {
        const int i = current(), j = i + d;
        if (i < 0 || j < 0 || j >= static_cast<int>(m_set.layers.size())) return;
        std::swap(m_set.layers[static_cast<std::size_t>(i)], m_set.layers[static_cast<std::size_t>(j)]);
        m_undoStack.clear();
        m_undo->setEnabled(false);
        refreshList();
        m_list->setCurrentRow(j);
    };
    connect(up, &QPushButton::clicked, this, [move] { move(-1); });
    connect(down, &QPushButton::clicked, this, [move] { move(1); });
    connect(m_list, &QListWidget::currentRowChanged, this, &ParallaxDialog::layerSelected);
    connect(m_scroll, &QDoubleSpinBox::valueChanged, this, &ParallaxDialog::layerEdited);
    connect(m_y, &QSpinBox::valueChanged, this, &ParallaxDialog::layerEdited);
    connect(m_repeat, &QCheckBox::toggled, this, &ParallaxDialog::layerEdited);
    connect(seamless, &QPushButton::clicked, this, &ParallaxDialog::makeSeamless);
    connect(sky, &QPushButton::clicked, this, [this] {
        const int i = current();
        if (i < 0) return;
        auto& l = m_set.layers[static_cast<std::size_t>(i)];
        pushUndo(i);
        l.image = clearSky(l.image, m_skyTolerance->value(), static_cast<SkyClear>(m_skyMode->currentData().toInt()), l.repeatX);
        redraw();
    });
    connect(m_undo, &QPushButton::clicked, this, [this] {
        if (m_undoStack.empty()) return;
        auto [k, img] = std::move(m_undoStack.back());
        m_undoStack.pop_back();
        if (k >= 0 && k < static_cast<int>(m_set.layers.size())) m_set.layers[static_cast<std::size_t>(k)].image = std::move(img);
        m_undo->setEnabled(!m_undoStack.empty());
        refreshList();
    });
    // Following a gradient compares neighbouring pixels, so it needs a much
    // smaller step than matching one flat colour.
    connect(m_skyMode, &QComboBox::currentIndexChanged, this, [this] {
        const auto mode = static_cast<SkyClear>(m_skyMode->currentData().toInt());
        m_skyTolerance->setValue(mode == SkyClear::Gradient ? 6 : 24);
        m_skyTolerance->setEnabled(mode != SkyClear::Split);
    });
    m_skyTolerance->setEnabled(false);   // Split, the first mode, needs none
    connect(m_aiCut, &QPushButton::clicked, this, [this] {
        if (m_cutJob && m_cutJob->running()) { m_cutJob->cancel(); return; }
        const int i = current();
        if (i < 0) return;
        if (!m_cutJob) {
            m_cutJob = new AiImageJob(m_ai, this);
            connect(m_cutJob, &AiImageJob::progress, this, [this](const QString& msg, const QString&) { m_genStatus->setText(msg); });
            connect(m_cutJob, &AiImageJob::finished, this, [this](const Image& cut, const Image&) {
                m_aiCut->setText(tr("Remove Background (AI)"));
                const int k = m_cutJob->property("layer").toInt();
                const int pad = m_cutPad;
                if (k < 0 || k >= static_cast<int>(m_set.layers.size()) || cut.width() != m_cutSource.width() + 2 * pad) return;
                auto& layer = m_set.layers[static_cast<std::size_t>(k)];
                if (layer.image.width() != m_cutSource.width() || layer.image.height() != m_cutSource.height()) return;
                // Crop the wrapped context away, then give the pixel art hard
                // edges and recover parts the mask missed.
                const Image mid = cut.copy({pad, 0, m_cutSource.width(), m_cutSource.height()});
                pushUndo(k);
                layer.image = refineCutout(m_cutSource, mid);
                m_genStatus->setText(tr("Background removed."));
                refreshList();
            });
            connect(m_cutJob, &AiImageJob::failed, this, [this](const QString& msg) {
                m_aiCut->setText(tr("Remove Background (AI)"));
                m_genStatus->setText(msg);
            });
        }
        QString why;
        m_cutJob->setProperty("layer", i);
        // A repeating layer is cut with a quarter of its other end on each
        // side, so the mask at the left edge sees what continues it on the
        // right and the cut-out still wraps.
        const auto& src = m_set.layers[static_cast<std::size_t>(i)];
        m_cutSource = src.image;
        m_cutPad = src.repeatX ? std::max(1, src.image.width() / 4) : 0;
        Image padded(src.image.width() + 2 * m_cutPad, src.image.height());
        padded.blit(src.image, m_cutPad, 0);
        if (m_cutPad > 0) {
            padded.blit(src.image.copy({src.image.width() - m_cutPad, 0, m_cutPad, src.image.height()}), 0, 0);
            padded.blit(src.image.copy({0, 0, m_cutPad, src.image.height()}), m_cutPad + src.image.width(), 0);
        }
        if (!m_cutJob->startRemoveBackground(padded, tr("Parallax: remove background"), &why)) {
            QMessageBox::information(this, windowTitle(), why);
            return;
        }
        m_aiCut->setText(tr("Cancel"));
        m_genStatus->setText(tr("Removing the background…"));
    });
    connect(m_generate, &QPushButton::clicked, this, &ParallaxDialog::generate);
    noDefaultButtons(this);
    resize(1000, 600);
    refreshList();
}

void ParallaxDialog::pushUndo(int layer) {
    if (layer < 0 || layer >= static_cast<int>(m_set.layers.size())) return;
    m_undoStack.emplace_back(layer, m_set.layers[static_cast<std::size_t>(layer)].image);
    if (m_undoStack.size() > 20) m_undoStack.erase(m_undoStack.begin());
    m_undo->setEnabled(true);
}

int ParallaxDialog::current() const {
    const int i = m_list->currentRow();
    return i >= 0 && i < static_cast<int>(m_set.layers.size()) ? i : -1;
}

void ParallaxDialog::addLayer(Image img, const QString& name) {
    ParallaxLayer l;
    l.name = name.toStdString();
    // Fit the layer's height to the view and keep the rest; a layer that is
    // shorter than the view sits at the bottom.
    if (img.height() > m_set.viewHeight * 2) {
        const int w = std::max(1, static_cast<int>(std::lround(img.width() * double(m_set.viewHeight) / img.height())));
        if (auto s = scaleNearest(img, w, m_set.viewHeight)) img = std::move(s).value();
    }
    l.image = std::move(img);
    l.y = m_set.viewHeight - l.image.height();
    // Further layers scroll slower: guess from the position in the stack.
    const auto n = m_set.layers.size();
    l.scroll = n == 0 ? 0.0 : std::min(1.0, 0.25 * static_cast<double>(n));
    l.repeatX = true;
    m_set.layers.push_back(std::move(l));
    refreshList();
    m_list->setCurrentRow(static_cast<int>(m_set.layers.size()) - 1);
}

void ParallaxDialog::refreshList() {
    const int keep = m_list->currentRow();
    m_updating = true;
    m_list->clear();
    for (const auto& l : m_set.layers)
        m_list->addItem(tr("%1  (%2×%3, scroll %4)").arg(qs(l.name)).arg(l.image.width()).arg(l.image.height()).arg(l.scroll, 0, 'f', 2));
    m_updating = false;
    m_list->setCurrentRow(std::min(keep, m_list->count() - 1));
    layerSelected();
    redraw();
}

void ParallaxDialog::layerSelected() {
    if (m_updating) return;
    const int i = current();
    for (QWidget* w : std::initializer_list<QWidget*>{m_scroll, m_y, m_repeat}) w->setEnabled(i >= 0);
    if (i < 0) return;
    const auto& l = m_set.layers[static_cast<std::size_t>(i)];
    m_updating = true;
    m_scroll->setValue(l.scroll);
    m_y->setValue(l.y);
    m_repeat->setChecked(l.repeatX);
    m_updating = false;
}

void ParallaxDialog::layerEdited() {
    if (m_updating) return;
    const int i = current();
    if (i < 0) return;
    auto& l = m_set.layers[static_cast<std::size_t>(i)];
    l.scroll = m_scroll->value();
    l.y = m_y->value();
    l.repeatX = m_repeat->isChecked();
    if (auto* item = m_list->item(i))
        item->setText(tr("%1  (%2×%3, scroll %4)").arg(qs(l.name)).arg(l.image.width()).arg(l.image.height()).arg(l.scroll, 0, 'f', 2));
    redraw();
}

void ParallaxDialog::redraw() {
    m_preview->setImage(renderParallax(m_set, m_camera));
    const auto issues = checkParallax(m_set);
    if (m_set.layers.empty()) {
        m_issues->setText(tr("Add layers back to front: from the document, a file, or Generate Layer."));
    } else if (issues.empty()) {
        m_issues->setText(tr("✓ No problems found: every repeating layer wraps without a seam and the view is covered."));
    } else {
        QStringList t;
        for (const auto& s : issues) t << QStringLiteral("• ") + qs(s);
        m_issues->setText(t.join(QLatin1Char('\n')));
    }
}

void ParallaxDialog::makeSeamless() {
    const int i = current();
    if (i < 0) return;
    auto& l = m_set.layers[static_cast<std::size_t>(i)];
    auto r = makeSeamlessX(l.image, m_overlap->value());
    if (!r) { QMessageBox::warning(this, windowTitle(), qs(r.error().message)); return; }
    pushUndo(i);
    l.image = std::move(r).value();
    refreshList();
}

void ParallaxDialog::generate() {
    if (m_job && m_job->running()) { m_job->cancel(); return; }
    if (!m_job) {
        m_job = new AiImageJob(m_ai, this);
        connect(m_job, &AiImageJob::progress, this, [this](const QString& msg, const QString&) { m_genStatus->setText(msg); });
        connect(m_job, &AiImageJob::finished, this, [this](const Image& pixel, const Image&) {
            m_generate->setText(tr("Generate Layer"));
            m_genStatus->setText(tr("Done. Use Clear Sky on a layer in front so the layers behind show through."));
            addLayer(pixel, m_job->property("name").toString());
        });
        connect(m_job, &AiImageJob::failed, this, [this](const QString& msg) {
            m_generate->setText(tr("Generate Layer"));
            m_genStatus->setText(msg);
        });
    }
    AiImageJob::Request r;
    const bool first = m_set.layers.empty();
    r.prompt = m_prompt->text().trimmed() +
               (first ? tr(", side-scrolling game background, wide panorama, pixel art")
                      : tr(", side-scrolling game background layer, silhouette against a plain flat light sky, pixel art"));
    r.negative = tr("text, watermark, frame, border, people, characters, vignette");
    r.width = 1344;   // an SDXL training aspect close to 16:9
    r.height = 768;
    r.tileable = QStringLiteral("x");
    r.pixelHeight = m_set.viewHeight;
    r.pixelWidth = std::max(8, static_cast<int>(std::lround(m_set.viewHeight * 1344.0 / 768.0)));
    r.colors = 24;
    r.label = tr("Parallax: %1").arg(m_prompt->text().left(30));
    m_job->setProperty("name", m_prompt->text().left(32));
    QString why;
    if (!m_job->start(r, &why)) { QMessageBox::information(this, windowTitle(), why); return; }
    m_generate->setText(tr("Cancel"));
    m_genStatus->setText(tr("Generating…"));
}

void ParallaxDialog::exportFiles() {
    if (m_set.layers.empty()) return;
    const QString f = QFileDialog::getSaveFileName(this, tr("Export Parallax Set"), QStringLiteral("parallax.json"), tr("Parallax JSON (*.json)"));
    if (f.isEmpty()) return;
    const fs::path json = withExtension(f, ".json");
    std::vector<std::string> files;
    for (std::size_t i = 0; i < m_set.layers.size(); ++i) {
        const std::string name = json.stem().string() + "_" + std::to_string(i) + ".png";
        if (Status st = writePng(m_set.layers[i].image, json.parent_path() / name); !st) {
            QMessageBox::warning(this, windowTitle(), qs(st.error().message));
            return;
        }
        files.push_back(name);
    }
    if (Status st = writeTextFile(json, parallaxJson(m_set, files)); !st) {
        QMessageBox::warning(this, windowTitle(), qs(st.error().message));
        return;
    }
    QMessageBox::information(this, windowTitle(), tr("Wrote %1 and %2 layer images.").arg(fromPath(json.filename())).arg(files.size()));
}

// ================================================================ UI kit

UiKitDialog::UiKitDialog(DocumentView* view, OpenImageFn openImage, QWidget* parent)
    : QDialog(parent), m_view(view), m_openImage(std::move(openImage)) {
    setWindowTitle(tr("UI Kit Generator"));
    auto* colours = new QFormLayout;
    const std::pair<QString, Rgba8 UiTheme::*> fields[] = {
        {tr("Base"), &UiTheme::base},       {tr("Light"), &UiTheme::light}, {tr("Dark"), &UiTheme::dark},
        {tr("Outline"), &UiTheme::outline}, {tr("Accent"), &UiTheme::accent}, {tr("Text"), &UiTheme::text},
    };
    for (const auto& [label, member] : fields) {
        auto* b = new QPushButton(this);
        b->setIcon(swatch(m_theme.*member));
        auto mem = member;
        connect(b, &QPushButton::clicked, this, [this, b, mem] {
            const Rgba8 c = m_theme.*mem;
            const QColor q = QColorDialog::getColor(QColor(c.r, c.g, c.b, c.a), this, tr("Choose Colour"));
            if (!q.isValid()) return;
            m_theme.*mem = Rgba8{static_cast<std::uint8_t>(q.red()), static_cast<std::uint8_t>(q.green()), static_cast<std::uint8_t>(q.blue()), 255};
            b->setIcon(swatch(m_theme.*mem));
            rebuild();
        });
        colours->addRow(label, b);
        m_colors.push_back(b);
    }
    auto* fromPalette = new QPushButton(tr("From Document Palette"), this);
    fromPalette->setToolTip(tr("Pick six colours of the document's palette, darkest to lightest"));
    connect(fromPalette, &QPushButton::clicked, this, [this] {
        if (!m_view) return;
        std::vector<Rgba8> cols(m_view->document().palette().colors().begin(), m_view->document().palette().colors().end());
        if (cols.size() < 4) { QMessageBox::information(this, windowTitle(), tr("The document palette needs at least 4 colours.")); return; }
        auto lum = [](Rgba8 c) { return 0.299 * c.r + 0.587 * c.g + 0.114 * c.b; };
        std::sort(cols.begin(), cols.end(), [&](Rgba8 a, Rgba8 b) { return lum(a) < lum(b); });
        auto at = [&](double t) { return cols[static_cast<std::size_t>(std::lround(t * double(cols.size() - 1)))]; };
        m_theme.outline = at(0.0);
        m_theme.dark = at(0.25);
        m_theme.base = at(0.45);
        m_theme.light = at(0.65);
        m_theme.text = at(1.0);
        // The accent: the most saturated colour.
        auto sat = [](Rgba8 c) { return std::max({c.r, c.g, c.b}) - std::min({c.r, c.g, c.b}); };
        m_theme.accent = *std::max_element(cols.begin(), cols.end(), [&](Rgba8 a, Rgba8 b) { return sat(a) < sat(b); });
        const Rgba8 UiTheme::* order[] = {&UiTheme::base, &UiTheme::light, &UiTheme::dark, &UiTheme::outline, &UiTheme::accent, &UiTheme::text};
        for (std::size_t i = 0; i < m_colors.size(); ++i) m_colors[i]->setIcon(swatch(m_theme.*order[i]));
        rebuild();
    });
    m_scale = new QSpinBox(this);
    m_scale->setRange(1, 4);
    m_scale->setToolTip(tr("Pixel size of lines and bevels"));
    m_rounded = new QCheckBox(tr("Rounded corners"), this);
    m_rounded->setChecked(true);
    m_texture = new QCheckBox(tr("Panel texture from document"), this);
    m_texture->setToolTip(tr("Tile the document's current frame (or selection) inside panels"));
    m_texture->setEnabled(view != nullptr);
    colours->addRow(QString(), fromPalette);
    colours->addRow(tr("Scale:"), m_scale);
    colours->addRow(QString(), m_rounded);
    colours->addRow(QString(), m_texture);

    m_element = new QComboBox(this);
    m_stretchW = new QSpinBox(this);
    m_stretchW->setRange(8, 512);
    m_stretchW->setValue(120);
    m_stretchH = new QSpinBox(this);
    m_stretchH->setRange(8, 512);
    m_stretchH->setValue(48);
    auto* testForm = new QFormLayout;
    testForm->addRow(tr("Element:"), m_element);
    auto* sizeRow = new QHBoxLayout;
    sizeRow->addWidget(m_stretchW);
    sizeRow->addWidget(new QLabel(QStringLiteral("×"), this));
    sizeRow->addWidget(m_stretchH);
    testForm->addRow(tr("Stretched to:"), sizeRow);
    auto* left = new QVBoxLayout;
    left->addLayout(colours);
    auto* testBox = new QGroupBox(tr("9-slice test"), this);
    testBox->setLayout(testForm);
    left->addWidget(testBox);
    left->addStretch();

    m_atlasView = new PixelView(this);
    m_atlasView->setMinimumSize(320, 240);
    m_stretchView = new PixelView(this);
    m_stretchView->setMinimumSize(320, 120);
    m_info = new QLabel(this);
    m_info->setWordWrap(true);
    auto* open = new QPushButton(tr("Open as &Document"), this);
    auto* exportBtn = new QPushButton(tr("&Export…"), this);
    auto* close = new QPushButton(tr("Close"), this);
    auto* buttons = new QHBoxLayout;
    buttons->addWidget(open);
    buttons->addWidget(exportBtn);
    buttons->addStretch();
    buttons->addWidget(close);
    auto* right = new QVBoxLayout;
    right->addWidget(m_atlasView, 2);
    right->addWidget(m_stretchView, 1);
    right->addWidget(m_info);
    right->addLayout(buttons);
    auto* root = new QHBoxLayout(this);
    root->addLayout(left);
    root->addLayout(right, 1);

    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    connect(exportBtn, &QPushButton::clicked, this, &UiKitDialog::exportFiles);
    connect(open, &QPushButton::clicked, this, [this] {
        if (m_files && !m_files->atlas.pages.empty() && m_openImage) m_openImage(m_files->atlas.pages.front(), tr("UI Kit"));
    });
    connect(m_scale, &QSpinBox::valueChanged, this, &UiKitDialog::rebuild);
    connect(m_rounded, &QCheckBox::toggled, this, &UiKitDialog::rebuild);
    connect(m_texture, &QCheckBox::toggled, this, &UiKitDialog::rebuild);
    connect(m_element, &QComboBox::currentIndexChanged, this, &UiKitDialog::rebuild);
    connect(m_stretchW, &QSpinBox::valueChanged, this, &UiKitDialog::rebuild);
    connect(m_stretchH, &QSpinBox::valueChanged, this, &UiKitDialog::rebuild);
    noDefaultButtons(this);
    resize(900, 600);
    rebuild();
}

void UiKitDialog::rebuild() {
    m_theme.scale = m_scale->value();
    m_theme.rounded = m_rounded->isChecked();
    m_theme.texture = m_texture->isChecked() ? documentImage(m_view) : Image();
    if (!m_theme.texture.isNull() && (m_theme.texture.width() > 64 || m_theme.texture.height() > 64))
        m_theme.texture = m_theme.texture.copy({0, 0, std::min(64, m_theme.texture.width()), std::min(64, m_theme.texture.height())});
    m_kit = generateUiKit(m_theme);
    auto files = packUiKit(m_kit, "ui_kit.png");
    m_files.reset();
    if (!files) { m_info->setText(qs(files.error().message)); return; }
    m_files = std::move(files).value();
    m_atlasView->setImage(m_files->atlas.pages.front());

    const QString keep = m_element->currentText();
    {
        const QSignalBlocker block(m_element);
        m_element->clear();
        for (const auto& e : m_kit)
            if (e.stretchable) m_element->addItem(qs(e.name));
        const int i = m_element->findText(keep);
        m_element->setCurrentIndex(i >= 0 ? i : 0);
    }
    const auto it = std::find_if(m_kit.begin(), m_kit.end(), [&](const UiElement& e) { return qs(e.name) == m_element->currentText(); });
    if (it != m_kit.end()) {
        auto r = renderNineSlice(it->image, it->slice, m_stretchW->value(), m_stretchH->value());
        if (r) m_stretchView->setImage(r.value());
    }
    int stretchable = 0;
    for (const auto& e : m_kit) stretchable += e.stretchable;
    m_info->setText(tr("%1 elements on a %2×%3 atlas; %4 can stretch with 9-slice borders stored in the JSON.")
                        .arg(m_kit.size()).arg(m_files->atlas.pages.front().width()).arg(m_files->atlas.pages.front().height()).arg(stretchable));
}

void UiKitDialog::exportFiles() {
    if (!m_files) return;
    const QString f = QFileDialog::getSaveFileName(this, tr("Export UI Kit"), QStringLiteral("ui_kit.png"), tr("PNG image (*.png)"));
    if (f.isEmpty()) return;
    const fs::path png = withExtension(f, ".png");
    const fs::path json = fs::path(png).replace_extension(".json");
    auto files = packUiKit(m_kit, png.filename().string());
    if (!files) { QMessageBox::warning(this, windowTitle(), qs(files.error().message)); return; }
    Status st = writePng(files.value().atlas.pages.front(), png);
    if (st) st = writeTextFile(json, files.value().json);
    if (!st) { QMessageBox::warning(this, windowTitle(), qs(st.error().message)); return; }
    QMessageBox::information(this, windowTitle(), tr("Wrote %1 and %2.").arg(fromPath(png.filename()), fromPath(json.filename())));
}

// ================================================================ slicer

SliceDialog::SliceDialog(AiService& ai, DocumentView* view, OpenFramesFn openFrames, QWidget* parent)
    : QDialog(parent), m_ai(ai), m_openFrames(std::move(openFrames)) {
    setWindowTitle(tr("Smart Slicer"));
    m_sheet = documentImage(view);
    m_original = m_sheet;
    m_name = view ? view->displayName() : QString();
    m_mode = new QComboBox(this);
    m_mode->addItem(tr("Find separate sprites"));
    m_mode->addItem(tr("Grid"));
    auto spin = [this](int lo, int hi, int v, const QString& tip = {}) {
        auto* s = new QSpinBox(this);
        s->setRange(lo, hi);
        s->setValue(v);
        s->setToolTip(tip);
        return s;
    };
    m_tolerance = spin(0, 255, 8, tr("How far from the background colour a pixel may be and still count as background"));
    m_merge = spin(0, 32, 2, tr("Parts this close (pixels) are one sprite: a detached sword, a shadow"));
    m_minPixels = spin(1, 10000, 4, tr("Smaller specks are ignored as noise"));
    m_cellW = spin(1, 4096, 16);
    m_cellH = spin(1, 4096, 16);
    m_offX = spin(0, 4096, 0);
    m_offY = spin(0, 4096, 0);
    m_spaceX = spin(0, 512, 0);
    m_spaceY = spin(0, 512, 0);
    m_skipEmpty = new QCheckBox(tr("Skip empty cells"), this);
    m_skipEmpty->setChecked(true);
    auto* detect = new QPushButton(tr("Detect Grid"), this);

    m_bgLabel = new QLabel(this);
    m_bgLabel->setWordWrap(true);
    m_autoKey = new QPushButton(tr("Auto"), this);
    m_autoKey->setToolTip(tr("Find the background colour automatically again"));
    m_aiRemove = new QPushButton(tr("Remove Background (AI)"), this);
    m_aiRemove->setToolTip(tr("For sprites on a busy background (a scene, a gradient): cut them out with BiRefNet, then slice"));
    m_restore = new QPushButton(tr("Restore Original"), this);
    m_restore->setVisible(false);
    auto* bgRow = new QHBoxLayout;
    bgRow->addWidget(m_bgLabel, 1);
    bgRow->addWidget(m_autoKey);
    auto* aiRow = new QHBoxLayout;
    aiRow->addWidget(m_aiRemove);
    aiRow->addWidget(m_restore);

    auto* common = new QFormLayout;
    common->addRow(tr("Mode:"), m_mode);
    common->addRow(tr("Background:"), bgRow);
    common->addRow(QString(), aiRow);
    common->addRow(tr("Background tolerance:"), m_tolerance);
    m_autoBox = new QWidget(this);
    auto* autoForm = new QFormLayout(m_autoBox);
    autoForm->setContentsMargins(0, 0, 0, 0);
    autoForm->addRow(tr("Merge distance:"), m_merge);
    autoForm->addRow(tr("Minimum pixels:"), m_minPixels);
    m_gridBox = new QWidget(this);
    auto* gridForm = new QFormLayout(m_gridBox);
    gridForm->setContentsMargins(0, 0, 0, 0);
    auto pair = [this](QSpinBox* a, QSpinBox* b) {
        auto* h = new QHBoxLayout;
        h->addWidget(a);
        h->addWidget(new QLabel(QStringLiteral("×"), this));
        h->addWidget(b);
        return h;
    };
    gridForm->addRow(QString(), detect);
    gridForm->addRow(tr("Cell size:"), pair(m_cellW, m_cellH));
    gridForm->addRow(tr("Offset:"), pair(m_offX, m_offY));
    gridForm->addRow(tr("Spacing:"), pair(m_spaceX, m_spaceY));
    gridForm->addRow(QString(), m_skipEmpty);
    auto* left = new QVBoxLayout;
    left->addLayout(common);
    left->addWidget(m_autoBox);
    left->addWidget(m_gridBox);
    left->addStretch();

    m_preview = new PixelView(this);
    m_preview->setMinimumSize(480, 360);
    m_info = new QLabel(this);
    m_info->setWordWrap(true);
    auto* frames = new QPushButton(tr("Open as &Animation"), this);
    frames->setToolTip(tr("A new document with one frame per sprite, aligned bottom-centre"));
    auto* pngs = new QPushButton(tr("Export &PNGs…"), this);
    auto* json = new QPushButton(tr("Export &JSON…"), this);
    auto* close = new QPushButton(tr("Close"), this);
    auto* buttons = new QHBoxLayout;
    buttons->addWidget(frames);
    buttons->addWidget(pngs);
    buttons->addWidget(json);
    buttons->addStretch();
    buttons->addWidget(close);
    auto* right = new QVBoxLayout;
    right->addWidget(m_preview, 1);
    right->addWidget(m_info);
    right->addLayout(buttons);
    auto* root = new QHBoxLayout(this);
    root->addLayout(left);
    root->addLayout(right, 1);

    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    connect(frames, &QPushButton::clicked, this, &SliceDialog::openFrames);
    connect(pngs, &QPushButton::clicked, this, &SliceDialog::exportPngs);
    connect(json, &QPushButton::clicked, this, &SliceDialog::exportJson);
    connect(detect, &QPushButton::clicked, this, &SliceDialog::detectGridNow);
    connect(m_preview, &PixelView::pressed, this, [this](QPoint p, Qt::MouseButton) {
        // Click the background to use its colour.
        const int x = p.x() / std::max(1, m_zoom), y = p.y() / std::max(1, m_zoom);
        if (!m_sheet.contains(x, y)) return;
        m_key = m_sheet.pixel(x, y);
        reslice();
    });
    connect(m_autoKey, &QPushButton::clicked, this, [this] { m_key.reset(); reslice(); });
    connect(m_restore, &QPushButton::clicked, this, [this] {
        m_sheet = m_original;
        m_restore->setVisible(false);
        reslice();
    });
    connect(m_aiRemove, &QPushButton::clicked, this, [this] {
        if (m_job && m_job->running()) { m_job->cancel(); return; }
        if (!m_job) {
            m_job = new AiImageJob(m_ai, this);
            connect(m_job, &AiImageJob::progress, this, [this](const QString& msg, const QString&) { m_info->setText(msg); });
            connect(m_job, &AiImageJob::finished, this, [this](const Image& cut, const Image&) {
                m_aiRemove->setText(tr("Remove Background (AI)"));
                if (cut.width() != m_original.width() || cut.height() != m_original.height()) return;
                // Hard pixel-art edges, and sprite parts the mask missed back.
                m_sheet = refineCutout(m_original, cut);
                m_key.reset();
                m_restore->setVisible(true);
                reslice();
            });
            connect(m_job, &AiImageJob::failed, this, [this](const QString& msg) {
                m_aiRemove->setText(tr("Remove Background (AI)"));
                m_info->setText(msg);
            });
        }
        QString why;
        if (!m_job->startRemoveBackground(m_original, tr("Slicer: remove background"), &why)) {
            QMessageBox::information(this, windowTitle(), why);
            return;
        }
        m_aiRemove->setText(tr("Cancel"));
        m_info->setText(tr("Removing the background…"));
    });
    connect(m_mode, &QComboBox::currentIndexChanged, this, &SliceDialog::reslice);
    connect(m_skipEmpty, &QCheckBox::toggled, this, &SliceDialog::reslice);
    for (QSpinBox* s : {m_tolerance, m_merge, m_minPixels, m_cellW, m_cellH, m_offX, m_offY, m_spaceX, m_spaceY})
        connect(s, &QSpinBox::valueChanged, this, &SliceDialog::reslice);
    noDefaultButtons(this);
    resize(960, 600);
    if (!m_sheet.isNull()) {
        // Start in grid mode when the sheet looks like a grid.
        const GridGuess g = detectGrid(m_sheet, options());
        if (g.found && g.columns * g.rows > 1) {
            m_updating = true;
            m_mode->setCurrentIndex(1);
            m_updating = false;
            detectGridNow();
        }
    }
    reslice();
}

SliceOptions SliceDialog::options() const {
    SliceOptions o;
    o.tolerance = m_tolerance->value();
    o.mergeDistance = m_merge->value();
    o.minPixels = m_minPixels->value();
    o.background = m_key;
    return o;
}

void SliceDialog::updateBackgroundLabel() {
    const auto bg = m_sheet.isNull() ? std::nullopt : sliceBackground(m_sheet, options());
    QString t;
    // No colour means the sheet has transparency, which is the background.
    if (!bg || bg->a == 0) t = tr("transparent");
    else t = tr("#%1%2%3").arg(bg->r, 2, 16, QLatin1Char('0')).arg(bg->g, 2, 16, QLatin1Char('0')).arg(bg->b, 2, 16, QLatin1Char('0'));
    if (m_key) t += tr(" (picked)");
    else if (bg) t += tr(" (automatic; click the picture to pick another, or use Remove Background (AI) on a busy background)");
    m_bgLabel->setText(t);
    m_autoKey->setEnabled(m_key.has_value());
}

GridGuess SliceDialog::grid() const {
    GridGuess g;
    g.cellWidth = m_cellW->value();
    g.cellHeight = m_cellH->value();
    g.offsetX = m_offX->value();
    g.offsetY = m_offY->value();
    g.spacingX = m_spaceX->value();
    g.spacingY = m_spaceY->value();
    g.columns = std::max(0, (m_sheet.width() - g.offsetX + g.spacingX) / std::max(1, g.cellWidth + g.spacingX));
    g.rows = std::max(0, (m_sheet.height() - g.offsetY + g.spacingY) / std::max(1, g.cellHeight + g.spacingY));
    g.found = g.columns > 0 && g.rows > 0;
    return g;
}

void SliceDialog::detectGridNow() {
    const GridGuess g = detectGrid(m_sheet, options());
    if (!g.found) {
        m_info->setText(tr("No regular grid found: the sprites are probably packed irregularly. Try Find separate sprites."));
        return;
    }
    m_updating = true;
    m_cellW->setValue(g.cellWidth);
    m_cellH->setValue(g.cellHeight);
    m_offX->setValue(g.offsetX);
    m_offY->setValue(g.offsetY);
    m_spaceX->setValue(g.spacingX);
    m_spaceY->setValue(g.spacingY);
    m_updating = false;
    reslice();
}

void SliceDialog::reslice() {
    if (m_updating) return;
    const bool gridMode = m_mode->currentIndex() == 1;
    m_gridBox->setVisible(gridMode);
    m_autoBox->setVisible(!gridMode);
    if (m_sheet.isNull()) {
        m_info->setText(tr("Open a sprite sheet document first."));
        return;
    }
    m_slices = gridMode ? sliceGrid(m_sheet, grid(), m_skipEmpty->isChecked(), options()) : detectSprites(m_sheet, options());
    // The overlay: the sheet scaled up with each slice outlined.
    const int z = std::clamp(720 / std::max(m_sheet.width(), m_sheet.height()), 1, 8);
    m_zoom = z;
    QImage q = toQImage(m_sheet).scaled(m_sheet.width() * z, m_sheet.height() * z);
    QPainter p(&q);
    p.setPen(QPen(QColor(255, 64, 160), 1));
    for (const auto& s : m_slices) p.drawRect(s.rect.x * z, s.rect.y * z, s.rect.w * z - 1, s.rect.h * z - 1);
    p.end();
    m_preview->setImage(q);
    updateBackgroundLabel();
    m_info->setText(tr("%1 sprite(s) in %2×%3.").arg(m_slices.size()).arg(m_sheet.width()).arg(m_sheet.height()));
}

void SliceDialog::exportJson() {
    if (m_slices.empty()) return;
    const QString f = QFileDialog::getSaveFileName(this, tr("Export Slices"), QStringLiteral("sheet.json"), tr("Slice JSON (*.json)"));
    if (f.isEmpty()) return;
    const fs::path json = withExtension(f, ".json");
    const fs::path png = fs::path(json).replace_extension(".png");
    Status st = writePng(m_sheet, png);
    if (st) st = writeTextFile(json, slicesJson(m_slices, png.filename().string(), m_sheet.width(), m_sheet.height()));
    if (!st) { QMessageBox::warning(this, windowTitle(), qs(st.error().message)); return; }
    QMessageBox::information(this, windowTitle(), tr("Wrote %1 and %2.").arg(fromPath(json.filename()), fromPath(png.filename())));
}

void SliceDialog::exportPngs() {
    if (m_slices.empty()) return;
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Export Sprites to Folder"));
    if (dir.isEmpty()) return;
    const fs::path d = toPath(dir);
    int existing = 0;
    for (const auto& s : m_slices) existing += fs::exists(d / (s.name + ".png"));
    if (existing && QMessageBox::question(this, windowTitle(), tr("%1 file(s) with the same names already exist in that folder. Replace them?").arg(existing)) != QMessageBox::Yes)
        return;
    for (const auto& s : m_slices)
        if (Status st = writePng(m_sheet.copy(s.rect), d / (s.name + ".png")); !st) {
            QMessageBox::warning(this, windowTitle(), qs(st.error().message));
            return;
        }
    QMessageBox::information(this, windowTitle(), tr("Wrote %1 PNG files.").arg(m_slices.size()));
}

void SliceDialog::openFrames() {
    if (m_slices.empty() || !m_openFrames) return;
    int w = 0, h = 0;
    for (const auto& s : m_slices) w = std::max(w, s.rect.w), h = std::max(h, s.rect.h);
    std::vector<Image> frames;
    for (const auto& s : m_slices) {
        Image f(w, h);
        f.blit(m_sheet.copy(s.rect), (w - s.rect.w) / 2, h - s.rect.h);
        if (const auto bg = sliceBackground(m_sheet, options()); bg && bg->a != 0) {
            // A coloured background becomes transparent in the frames.
            for (auto& px : f.pixels())
                if (colourDistance(px, *bg) <= m_tolerance->value()) px = Rgba8::transparent();
        }
        frames.push_back(std::move(f));
    }
    m_openFrames(frames, std::vector<int>(frames.size(), 100), m_name.isEmpty() ? tr("Sliced") : tr("%1 frames").arg(m_name));
    accept();
}

// ================================================================ atlas

AtlasDialog::AtlasDialog(DocumentView* view, QWidget* parent) : QDialog(parent), m_view(view) {
    setWindowTitle(tr("Pack Sprite Atlas"));
    m_list = new QListWidget(this);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    auto* addFiles = new QPushButton(tr("Add Files…"), this);
    auto* addFrames = new QPushButton(tr("Add Document Frames"), this);
    addFrames->setEnabled(view != nullptr);
    auto* remove = new QPushButton(tr("Remove"), this);
    m_maxSize = new QComboBox(this);
    for (int s : {256, 512, 1024, 2048, 4096, 8192}) m_maxSize->addItem(QStringLiteral("%1 × %1").arg(s), s);
    m_maxSize->setCurrentIndex(3);
    m_padding = new QSpinBox(this);
    m_padding->setRange(0, 64);
    m_padding->setValue(1);
    m_border = new QSpinBox(this);
    m_border->setRange(0, 64);
    m_extrude = new QSpinBox(this);
    m_extrude->setRange(0, 16);
    m_extrude->setToolTip(tr("Repeat edge pixels outward so filtered or scaled sprites don't bleed"));
    m_trim = new QCheckBox(tr("Trim transparent borders"), this);
    m_merge = new QCheckBox(tr("Merge identical sprites"), this);
    m_merge->setChecked(true);
    m_pot = new QCheckBox(tr("Power-of-two pages"), this);
    m_rotate = new QCheckBox(tr("Allow rotation"), this);
    m_rotate->setToolTip(tr("Turn sprites 90° clockwise where that packs tighter (\"rotated\": true in the JSON; the engine turns them back)"));
    auto* form = new QFormLayout;
    form->addRow(tr("Max page size:"), m_maxSize);
    form->addRow(tr("Padding:"), m_padding);
    form->addRow(tr("Border:"), m_border);
    form->addRow(tr("Extrude:"), m_extrude);
    form->addRow(QString(), m_trim);
    form->addRow(QString(), m_merge);
    form->addRow(QString(), m_pot);
    form->addRow(QString(), m_rotate);
    auto* listButtons = new QHBoxLayout;
    listButtons->addWidget(addFiles);
    listButtons->addWidget(addFrames);
    listButtons->addWidget(remove);
    auto* left = new QVBoxLayout;
    left->addWidget(m_list, 1);
    left->addLayout(listButtons);
    left->addLayout(form);

    m_preview = new PixelView(this);
    m_preview->setMinimumSize(420, 420);
    m_page = new QSpinBox(this);
    m_page->setPrefix(tr("Page "));
    m_page->setRange(1, 1);
    m_info = new QLabel(this);
    m_info->setWordWrap(true);
    m_export = new QPushButton(tr("&Export…"), this);
    auto* close = new QPushButton(tr("Close"), this);
    auto* buttons = new QHBoxLayout;
    buttons->addWidget(m_page);
    buttons->addStretch();
    buttons->addWidget(m_export);
    buttons->addWidget(close);
    auto* right = new QVBoxLayout;
    right->addWidget(m_preview, 1);
    right->addWidget(m_info);
    right->addLayout(buttons);
    auto* root = new QHBoxLayout(this);
    root->addLayout(left);
    root->addLayout(right, 1);

    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_export, &QPushButton::clicked, this, &AtlasDialog::exportFiles);
    connect(addFiles, &QPushButton::clicked, this, [this] {
        const QStringList files = QFileDialog::getOpenFileNames(this, tr("Add Sprites"), QString(), tr("Images (*.png *.bmp *.gif *.webp)"));
        QStringList failed;
        for (const QString& f : files) {
            QImageReader r(f);
            const QImage q = r.read();
            if (q.isNull()) { failed << QFileInfo(f).fileName(); continue; }
            m_items.push_back({QFileInfo(f).completeBaseName().toStdString(), fromQImage(q)});
        }
        if (!failed.isEmpty()) QMessageBox::warning(this, windowTitle(), tr("Could not read: %1").arg(failed.join(QStringLiteral(", "))));
        refreshList();
    });
    connect(addFrames, &QPushButton::clicked, this, [this] {
        if (!m_view) return;
        const auto& doc = m_view->document();
        QString base = m_view->displayName();
        base.remove(QLatin1Char('*'));
        base = QFileInfo(base).completeBaseName().trimmed().replace(QLatin1Char(' '), QLatin1Char('_'));
        if (base.isEmpty()) base = QStringLiteral("frame");
        for (int i = 0; i < doc.frameCount(); ++i)
            m_items.push_back({QStringLiteral("%1_%2").arg(base).arg(i, 3, 10, QLatin1Char('0')).toStdString(), doc.composite(i)});
        refreshList();
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        std::vector<int> rows;
        for (auto* it : m_list->selectedItems()) rows.push_back(m_list->row(it));
        std::sort(rows.rbegin(), rows.rend());
        for (int r : rows) m_items.erase(m_items.begin() + r);
        refreshList();
    });
    connect(m_maxSize, &QComboBox::currentIndexChanged, this, &AtlasDialog::repack);
    for (QSpinBox* s : {m_padding, m_border, m_extrude}) connect(s, &QSpinBox::valueChanged, this, &AtlasDialog::repack);
    for (QCheckBox* c : {m_trim, m_merge, m_pot, m_rotate}) connect(c, &QCheckBox::toggled, this, &AtlasDialog::repack);
    connect(m_page, &QSpinBox::valueChanged, this, [this](int p) {
        if (m_atlas && p >= 1 && p <= static_cast<int>(m_atlas->pages.size())) m_preview->setImage(m_atlas->pages[static_cast<std::size_t>(p - 1)]);
    });
    noDefaultButtons(this);
    resize(960, 620);
    refreshList();
}

void AtlasDialog::refreshList() {
    // Names must be unique in the JSON: number repeats.
    std::map<std::string, int> seen;
    for (auto& it : m_items) {
        const int n = seen[it.name]++;
        if (n) it.name += "_" + std::to_string(n);
    }
    m_list->clear();
    for (const auto& it : m_items) m_list->addItem(tr("%1  (%2×%3)").arg(qs(it.name)).arg(it.image.width()).arg(it.image.height()));
    repack();
}

void AtlasDialog::repack() {
    m_atlas.reset();
    m_export->setEnabled(false);
    if (m_items.empty()) {
        m_preview->setImage(Image());
        m_info->setText(tr("Add PNG files or the frames of the current document."));
        return;
    }
    AtlasOptions o;
    o.maxWidth = o.maxHeight = m_maxSize->currentData().toInt();
    o.padding = m_padding->value();
    o.border = m_border->value();
    o.extrude = m_extrude->value();
    o.trim = m_trim->isChecked();
    o.mergeDuplicates = m_merge->isChecked();
    o.powerOfTwo = m_pot->isChecked();
    o.allowRotation = m_rotate->isChecked();
    auto a = packAtlas(m_items, o);
    if (!a) {
        m_preview->setImage(Image());
        m_info->setText(qs(a.error().message));
        return;
    }
    m_atlas = std::move(a).value();
    long long used = 0, total = 0;
    int dups = 0, turned = 0;
    for (const auto& s : m_atlas->sprites) {
        turned += s.rotated && s.duplicateOf < 0;
        if (s.duplicateOf >= 0) { ++dups; continue; }
        used += static_cast<long long>(s.rect.w) * s.rect.h;
    }
    for (const auto& p : m_atlas->pages) total += static_cast<long long>(p.width()) * p.height();
    m_page->setRange(1, static_cast<int>(m_atlas->pages.size()));
    m_page->setValue(1);
    m_preview->setImage(m_atlas->pages.front());
    QString sizes;
    for (const auto& p : m_atlas->pages) sizes += QStringLiteral(" %1×%2").arg(p.width()).arg(p.height());
    m_info->setText(tr("%1 sprite(s) on %2 page(s):%3 · %4% filled · %5 duplicate(s) merged%6. The same input always packs the same way.")
                        .arg(m_atlas->sprites.size()).arg(m_atlas->pages.size()).arg(sizes)
                        .arg(total ? qRound(100.0 * double(used) / double(total)) : 0).arg(dups)
                        .arg(turned ? tr(" · %1 rotated").arg(turned) : QString()));
    m_export->setEnabled(true);
}

void AtlasDialog::exportFiles() {
    if (!m_atlas) return;
    const QString f = QFileDialog::getSaveFileName(this, tr("Export Atlas"), QStringLiteral("atlas.png"), tr("PNG image (*.png)"));
    if (f.isEmpty()) return;
    const fs::path png = withExtension(f, ".png");
    const fs::path json = fs::path(png).replace_extension(".json");
    std::vector<std::string> files;
    const bool multi = m_atlas->pages.size() > 1;
    for (std::size_t i = 0; i < m_atlas->pages.size(); ++i) {
        const std::string name = multi ? png.stem().string() + "_" + std::to_string(i) + ".png" : png.filename().string();
        if (Status st = writePng(m_atlas->pages[i], png.parent_path() / name); !st) {
            QMessageBox::warning(this, windowTitle(), qs(st.error().message));
            return;
        }
        files.push_back(name);
    }
    if (Status st = writeTextFile(json, atlasJson(*m_atlas, files)); !st) {
        QMessageBox::warning(this, windowTitle(), qs(st.error().message));
        return;
    }
    QMessageBox::information(this, windowTitle(), tr("Wrote %1 page image(s) and %2.").arg(files.size()).arg(fromPath(json.filename())));
}

// ================================================================ template

void newTilesetTemplate(OpenImageFn openImage, QWidget* parent) {
    const QStringList items{QObject::tr("8-neighbour blob (47 tiles)"), QObject::tr("Wang corners / dual grid (16 tiles)"),
                            QObject::tr("4-neighbour (16 tiles)")};
    bool ok = false;
    const QString pick = QInputDialog::getItem(parent, QObject::tr("New Tileset Template"), QObject::tr("Layout:"), items, 0, false, &ok);
    if (!ok) return;
    const int size = QInputDialog::getInt(parent, QObject::tr("New Tileset Template"), QObject::tr("Tile size (px):"), 16, 8, 64, 1, &ok);
    if (!ok) return;
    const AutotileScheme s = std::array{AutotileScheme::Blob47, AutotileScheme::Wang16, AutotileScheme::Cardinal16}[static_cast<std::size_t>(items.indexOf(pick))];
    const Image img = tilesetTemplate(s, size, Rgba8{96, 168, 72, 255}, Rgba8{150, 108, 70, 255});
    if (openImage) openImage(img, QObject::tr("Tileset template (%1)").arg(pick));
}

} // namespace acestudio::ui
