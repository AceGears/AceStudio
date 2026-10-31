#include "TilesetDialog.h"

#include "AssetCommon.h"

#include "../DocumentView.h"
#include "../QtBridge.h"
#include "../ai/AiImageJob.h"

#include "acestudio/core/PngCodec.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImageReader>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>
#include <set>

namespace acestudio::ui {

namespace fs = std::filesystem;

namespace {

QString schemeLabel(AutotileScheme s) {
    switch (s) {
    case AutotileScheme::Blob47: return QObject::tr("8-neighbour blob (47 tiles)");
    case AutotileScheme::Wang16: return QObject::tr("Wang corners / dual grid (16 tiles)");
    case AutotileScheme::Cardinal16: return QObject::tr("4-neighbour (16 tiles)");
    }
    return {};
}

// A small map with an island, a lake inside it, a thin bridge and loose
// cells, so every kind of tile shows up; higher levels sit inside it.
LevelMap sampleMap(int levels) {
    LevelMap m(16, 10);
    for (int y = 0; y < 10; ++y)
        for (int x = 0; x < 16; ++x) {
            const double dx = (x - 6.0) / 4.6, dy = (y - 4.5) / 3.6;
            if (dx * dx + dy * dy < 1.0) m.set(x, y, 1);
        }
    m.set(6, 4, 0);
    m.set(7, 4, 0);
    m.set(6, 5, 0);
    for (int x = 10; x < 14; ++x) m.set(x, 5, 1);
    for (int y = 2; y < 8; ++y) m.set(13, y, 1);
    m.set(15, 1, 1);
    m.set(15, 8, 1);
    m.set(14, 8, 1);
    if (levels >= 2) {
        for (int y = 2; y < 8; ++y)
            for (int x = 3; x < 6; ++x) m.set(x, y, 2);
        m.set(8, 3, 2);
        m.set(8, 2, 2);
        for (int y = 3; y < 7; ++y) m.set(13, y, 2);
    }
    if (levels >= 3) {
        for (int y = 3; y < 7; ++y) m.set(4, y, 3);
        m.set(3, 4, 3);
    }
    return m;
}

QString wrapText(const Image& tex) {
    if (tex.isNull()) return QObject::tr("No texture yet");
    const WrapCheck w = checkWrap(tex);
    if (w.horizontal && w.vertical) return QObject::tr("✓ Tiles seamlessly");
    QString t = QObject::tr("✗ Visible seam");
    if (!w.horizontal) t += QObject::tr(" left↔right");
    if (!w.vertical) t += QObject::tr(" top↔bottom");
    return t;
}

const char* const kDefaultPrompts[] = {"brown dirt ground", "lush green grass", "grey mountain rock", "white snow"};

} // namespace

TilesetDialog::TilesetDialog(AiService& ai, DocumentView* view, OpenImageFn openImage, QWidget* parent)
    : QDialog(parent), m_ai(ai), m_view(view), m_openImage(std::move(openImage)) {
    setWindowTitle(tr("Tileset Generator"));
    m_scheme = new QComboBox(this);
    for (auto s : {AutotileScheme::Blob47, AutotileScheme::Wang16, AutotileScheme::Cardinal16})
        m_scheme->addItem(schemeLabel(s), QString::fromLatin1(toString(s).data()));
    m_tileSize = new QSpinBox(this);
    m_tileSize->setRange(8, 64);
    m_tileSize->setValue(16);
    m_tileSize->setSuffix(tr(" px"));
    m_phases = new QComboBox(this);
    for (int p = 1; p <= 4; ++p) m_phases->addItem(p == 1 ? tr("1 tile") : tr("%1 × %1 tiles").arg(p), p);
    m_phases->setToolTip(tr("Texture size. Larger textures keep more detail: each tile gets one variant per position in the texture"));
    m_roundness = new QSlider(Qt::Horizontal, this);
    m_roundness->setRange(0, 100);
    m_roundness->setValue(100);
    m_roundness->setToolTip(tr("Straight / diagonal (left) to rounded (right) transitions"));
    m_noise = new QSlider(Qt::Horizontal, this);
    m_noise->setRange(0, 90);
    m_noise->setValue(30);
    m_noise->setToolTip(tr("How ragged the terrain edge is"));
    m_noiseScale = new QSpinBox(this);
    m_noiseScale->setRange(1, 16);
    m_noiseScale->setValue(4);
    m_noiseScale->setToolTip(tr("Edge noise features per tile"));
    m_outline = new QCheckBox(tr("Outline the terrain edge"), this);
    m_outline->setChecked(true);
    m_seed = new QSpinBox(this);
    m_seed->setRange(1, 999999);
    m_seed->setValue(1);

    auto* settings = new QFormLayout;
    settings->addRow(tr("Layout:"), m_scheme);
    settings->addRow(tr("Tile size:"), m_tileSize);
    settings->addRow(tr("Texture size:"), m_phases);
    settings->addRow(tr("Roundness:"), m_roundness);
    settings->addRow(tr("Edge noise:"), m_noise);
    settings->addRow(tr("Noise detail:"), m_noiseScale);
    settings->addRow(tr("Seed:"), m_seed);
    settings->addRow(QString(), m_outline);

    auto* sourcesWidget = new QWidget(this);
    m_sourceLayout = new QVBoxLayout(sourcesWidget);
    m_sourceLayout->setContentsMargins(0, 0, 0, 0);
    m_sourceLayout->addStretch();
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setWidget(sourcesWidget);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setMinimumWidth(420);
    m_addLevel = new QPushButton(tr("Add Terrain Level"), this);
    m_addLevel->setToolTip(tr("Another terrain drawn over the one below it, e.g. grass over sand over water"));
    m_removeLevel = new QPushButton(tr("Remove Top Level"), this);
    auto* levelButtons = new QHBoxLayout;
    levelButtons->addWidget(m_addLevel);
    levelButtons->addWidget(m_removeLevel);
    levelButtons->addStretch();

    auto* left = new QVBoxLayout;
    left->addWidget(scroll, 1);
    left->addLayout(levelButtons);
    left->addLayout(settings);

    m_preview = new PixelView(this);
    m_preview->setMinimumSize(384, 288);
    m_previewLevel = new QComboBox(this);
    m_mapPreview = new PixelView(this);
    m_mapPreview->setMinimumSize(384, 240);
    auto* tilesetPage = new QWidget(this);
    auto* tilesetLay = new QVBoxLayout(tilesetPage);
    tilesetLay->setContentsMargins(0, 0, 0, 0);
    tilesetLay->addWidget(m_previewLevel);
    tilesetLay->addWidget(m_preview, 1);
    auto* tabs = new QTabWidget(this);
    tabs->addTab(m_mapPreview, tr("Preview map"));
    tabs->addTab(tilesetPage, tr("Tileset"));
    m_seams = new QLabel(this);
    m_seams->setWordWrap(true);
    auto* right = new QVBoxLayout;
    right->addWidget(tabs, 1);
    right->addWidget(m_seams);

    m_sandbox = new QPushButton(tr("Terrain &Sandbox…"), this);
    m_open = new QPushButton(tr("Open as &Document"), this);
    m_export = new QPushButton(tr("&Export…"), this);
    auto* close = new QPushButton(tr("Close"), this);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_sandbox, &QPushButton::clicked, this, &TilesetDialog::openSandbox);
    connect(m_export, &QPushButton::clicked, this, &TilesetDialog::exportFiles);
    connect(m_open, &QPushButton::clicked, this, [this] {
        const int i = std::max(0, m_previewLevel->currentIndex());
        if (i < static_cast<int>(m_levels.size()) && m_openImage)
            m_openImage(m_levels[static_cast<std::size_t>(i)].image,
                        tr("Tileset %1 level %2").arg(schemeLabel(m_levels[0].scheme)).arg(i + 1));
    });
    connect(m_previewLevel, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (i >= 0 && i < static_cast<int>(m_levels.size())) m_preview->setImage(m_levels[static_cast<std::size_t>(i)].image);
    });
    auto* buttons = new QHBoxLayout;
    buttons->addWidget(m_sandbox);
    buttons->addWidget(m_open);
    buttons->addWidget(m_export);
    buttons->addStretch();
    buttons->addWidget(close);

    auto* body = new QHBoxLayout;
    body->addLayout(left);
    body->addLayout(right, 1);
    auto* root = new QVBoxLayout(this);
    root->addLayout(body, 1);
    root->addLayout(buttons);

    for (QObject* o : std::initializer_list<QObject*>{m_scheme, m_roundness, m_noise, m_noiseScale, m_seed, m_outline}) {
        if (auto* c = qobject_cast<QComboBox*>(o)) connect(c, &QComboBox::currentIndexChanged, this, &TilesetDialog::rebuild);
        if (auto* sp = qobject_cast<QSpinBox*>(o)) connect(sp, &QSpinBox::valueChanged, this, &TilesetDialog::rebuild);
        if (auto* sl = qobject_cast<QSlider*>(o)) connect(sl, &QSlider::valueChanged, this, &TilesetDialog::rebuild);
        if (auto* cb = qobject_cast<QCheckBox*>(o)) connect(cb, &QCheckBox::toggled, this, &TilesetDialog::rebuild);
    }
    // When the tile or texture size changes, textures are re-fitted from the
    // images they were made from, not from the reduced copy.
    auto refit = [this] {
        for (auto& s : m_sources)
            if (!s->original.isNull()) {
                const int want = tileSize() * phases();
                s->texture = fitSquare(s->original, want);
                s->view->setImage(s->texture);
                QString text = wrapText(s->texture);
                if (std::min(s->original.width(), s->original.height()) < want)
                    text += tr("\nEnlarged from %1 px: open or generate it again for full detail.").arg(std::min(s->original.width(), s->original.height()));
                s->status->setText(text);
            }
        rebuild();
    };
    connect(m_tileSize, &QSpinBox::valueChanged, this, refit);
    connect(m_phases, &QComboBox::currentIndexChanged, this, refit);
    connect(m_addLevel, &QPushButton::clicked, this, [this] {
        if (m_sources.size() >= 4) return;
        addSource(QString::fromLatin1(kDefaultPrompts[m_sources.size()]));
        retitleSources();
        rebuild();
    });
    connect(m_removeLevel, &QPushButton::clicked, this, [this] {
        if (m_sources.size() <= 2) return;
        Source& s = *m_sources.back();
        if (s.job && s.job->running()) s.job->cancel();
        s.box->deleteLater();
        m_sources.pop_back();
        retitleSources();
        rebuild();
    });

    // Base and one terrain to start with: dirt under grass.
    addSource(tr("brown dirt ground"));
    addSource(tr("lush green grass"));
    retitleSources();
    noDefaultButtons(this);
    resize(1180, 760);
    rebuild();
}

TilesetDialog::~TilesetDialog() = default;

TilesetDialog::Source& TilesetDialog::addSource(const QString& defaultPrompt) {
    auto owned = std::make_unique<Source>();
    Source& s = *owned;
    m_sources.push_back(std::move(owned));
    s.box = new QGroupBox(this);
    s.view = new PixelView(s.box);
    s.view->setRepeat(3);
    s.view->setFixedSize(120, 120);
    s.view->setToolTip(tr("Shown 3×3 times to reveal seams"));
    s.status = new QLabel(s.box);
    s.status->setWordWrap(true);
    s.prompt = new QLineEdit(defaultPrompt, s.box);
    s.prompt->setToolTip(tr("What the texture shows, for AI generation"));
    auto* fromLayer = new QPushButton(tr("From Document"), s.box);
    fromLayer->setToolTip(tr("The current frame (or the selection), cropped square and scaled to the texture size"));
    auto* open = new QPushButton(tr("Open…"), s.box);
    s.generate = new QPushButton(tr("Generate"), s.box);
    s.generate->setToolTip(tr("A seamless texture from Stable Diffusion XL, reduced to the texture size"));
    Source* sp = &s;
    connect(fromLayer, &QPushButton::clicked, this, [this, sp] {
        const Image img = documentImage(m_view);
        if (img.isNull()) { QMessageBox::information(this, windowTitle(), tr("Open a document first.")); return; }
        setTexture(*sp, img);
    });
    connect(open, &QPushButton::clicked, this, [this, sp] {
        const QString f = QFileDialog::getOpenFileName(this, tr("Open Texture"), QString(), tr("Images (*.png *.jpg *.jpeg *.bmp *.webp *.gif)"));
        if (f.isEmpty()) return;
        QImageReader r(f);
        const QImage q = r.read();
        if (q.isNull()) { QMessageBox::warning(this, windowTitle(), tr("Could not read %1: %2").arg(f, r.errorString())); return; }
        setTexture(*sp, fromQImage(q));
    });
    connect(s.generate, &QPushButton::clicked, this, [this, sp] { generateTexture(*sp); });
    auto* row = new QHBoxLayout;
    row->addWidget(fromLayer);
    row->addWidget(open);
    auto* gen = new QHBoxLayout;
    gen->addWidget(s.prompt, 1);
    gen->addWidget(s.generate);
    auto* side = new QVBoxLayout;
    side->addLayout(row);
    side->addWidget(s.status);
    side->addStretch();
    side->addLayout(gen);
    auto* top = new QHBoxLayout(s.box);
    top->addWidget(s.view);
    top->addLayout(side, 1);
    s.status->setText(wrapText({}));
    m_sourceLayout->insertWidget(m_sourceLayout->count() - 1, s.box);
    noDefaultButtons(s.box);
    return s;
}

void TilesetDialog::retitleSources() {
    for (std::size_t i = 0; i < m_sources.size(); ++i)
        m_sources[i]->box->setTitle(i == 0 ? tr("Base (level 0, underneath)")
                                           : tr("Terrain level %1 (drawn over level %2)").arg(i).arg(i - 1));
    m_addLevel->setEnabled(m_sources.size() < 4);
    m_removeLevel->setEnabled(m_sources.size() > 2);
}

void TilesetDialog::setTexture(Source& s, const Image& img) {
    s.original = img;
    s.texture = fitSquare(img, tileSize() * phases());
    s.view->setImage(s.texture);
    s.status->setText(wrapText(s.texture));
    rebuild();
}

void TilesetDialog::generateTexture(Source& s) {
    if (s.job && s.job->running()) {
        s.job->cancel();
        return;
    }
    if (!s.job) {
        s.job = new AiImageJob(m_ai, this);
        Source* sp = &s;
        connect(s.job, &AiImageJob::progress, this, [sp](const QString& msg, const QString& preview) {
            sp->status->setText(msg);
            if (!preview.isEmpty()) {
                QImage q(preview);
                if (!q.isNull()) sp->view->setImage(q.scaled(96, 96));
            }
        });
        connect(s.job, &AiImageJob::finished, this, [this, sp](const Image& pixel, const Image&) {
            sp->generate->setText(tr("Generate"));
            setTexture(*sp, pixel);
        });
        connect(s.job, &AiImageJob::failed, this, [this, sp](const QString& msg) {
            sp->generate->setText(tr("Generate"));
            sp->status->setText(msg);
            sp->view->setImage(sp->texture);
        });
    }
    AiImageJob::Request r;
    r.prompt = s.prompt->text().trimmed() + tr(", seamless repeating texture filling the whole image, top-down view, flat even lighting, pixel art");
    r.negative = tr("objects, border, frame, perspective, horizon, shadow, vignette, text, watermark, tiles, paving, grid of squares");
    r.tileable = QStringLiteral("xy");
    r.pixelWidth = r.pixelHeight = tileSize() * phases();
    r.colors = 16;
    r.seed = m_seed->value();
    r.label = tr("Texture: %1").arg(s.prompt->text().left(30));
    QString why;
    if (!s.job->start(r, &why)) { QMessageBox::information(this, windowTitle(), why); return; }
    s.generate->setText(tr("Cancel"));
    s.status->setText(tr("Generating…"));
}

AutotileScheme TilesetDialog::scheme() const {
    return autotileSchemeFromString(m_scheme->currentData().toString().toStdString()).value_or(AutotileScheme::Blob47);
}

int TilesetDialog::tileSize() const { return m_tileSize->value(); }

int TilesetDialog::phases() const { return std::max(1, m_phases->currentData().toInt()); }

void TilesetDialog::rebuild() {
    m_levels.clear();
    m_export->setEnabled(false);
    m_sandbox->setEnabled(false);
    m_open->setEnabled(false);
    const bool ready = std::all_of(m_sources.begin(), m_sources.end(), [](const auto& s) { return !s->texture.isNull(); });
    if (!ready || m_sources.size() < 2) {
        m_seams->setText(tr("Choose a texture for every level: from the document, a file, or Generate."));
        m_preview->setImage(Image());
        m_mapPreview->setImage(Image());
        const QSignalBlocker block(m_previewLevel);
        m_previewLevel->clear();
        return;
    }
    TilesetStyle st;
    st.roundness = m_roundness->value() / 100.0;
    st.edgeNoise = m_noise->value() / 100.0;
    st.noiseScale = m_noiseScale->value();
    st.outline = m_outline->isChecked();
    st.seed = static_cast<std::uint32_t>(m_seed->value());
    for (std::size_t i = 1; i < m_sources.size(); ++i) {
        // Level 1 is drawn over the base texture; higher levels over a
        // transparent base, so the level below shows through.
        auto t = generateTileset(scheme(), i == 1 ? m_sources[0]->texture : Image(), m_sources[i]->texture, st, tileSize());
        if (!t) { m_seams->setText(qs(t.error().message)); m_levels.clear(); return; }
        m_levels.push_back(std::move(t).value());
    }
    {
        const int keep = std::max(0, m_previewLevel->currentIndex());
        const QSignalBlocker block(m_previewLevel);
        m_previewLevel->clear();
        for (std::size_t i = 0; i < m_levels.size(); ++i)
            m_previewLevel->addItem(i == 0 ? tr("Level 1 over the base") : tr("Level %1 (transparent base)").arg(i + 1));
        m_previewLevel->setCurrentIndex(std::min(keep, m_previewLevel->count() - 1));
    }
    m_preview->setImage(m_levels[static_cast<std::size_t>(std::max(0, m_previewLevel->currentIndex()))].image);
    if (auto img = renderLevels(sampleMap(static_cast<int>(m_levels.size())), m_levels)) m_mapPreview->setImage(img.value());
    int pairs = 0, issues = 0;
    QString worst;
    for (std::size_t i = 0; i < m_levels.size(); ++i) {
        const auto rep = checkSeams(m_levels[i]);
        if (!rep) continue;
        pairs += rep.value().pairsChecked;
        issues += static_cast<int>(rep.value().issues.size());
        if (worst.isEmpty() && !rep.value().ok())
            worst = tr("level %1, tile %2 → %3").arg(i + 1).arg(rep.value().issues.front().a).arg(rep.value().issues.front().b);
    }
    QString text = issues == 0 ? tr("✓ Seam check: %1 tile pairs that can touch, no seams.").arg(pairs)
                               : tr("✗ Seam check: %1 of %2 pairs show a seam (largest: %3). The textures probably do not tile seamlessly.")
                                     .arg(issues).arg(pairs).arg(worst);
    if (std::any_of(m_sources.begin(), m_sources.end(), [](const auto& s) {
            const WrapCheck w = checkWrap(s->texture);
            return !(w.horizontal && w.vertical);
        }))
        text += tr("\nA texture does not tile seamlessly, so repeated tiles will show its edges.");
    if (phases() > 1) text += tr("\nEach tile has %1 × %1 variants: draw cell (x, y) with variant (x mod %1, y mod %1).").arg(phases());
    m_seams->setText(text);
    m_export->setEnabled(true);
    m_sandbox->setEnabled(true);
    m_open->setEnabled(true);
}

void TilesetDialog::exportFiles() {
    if (m_levels.empty()) return;
    const QString f = QFileDialog::getSaveFileName(this, tr("Export Tileset"), QStringLiteral("tileset.png"), tr("PNG image (*.png)"));
    if (f.isEmpty()) return;
    fs::path png = toPath(f);
    if (png.extension() != ".png") png += ".png";
    const fs::path dir = png.parent_path();
    const std::string stem = png.stem().string();
    const Tileset& first = m_levels.front();
    const bool withBase = !first.base.isNull();
    const fs::path base = dir / (stem + "_base.png");
    std::vector<std::string> images, jsons;
    QStringList written;
    auto fail = [this](const Status& st) { QMessageBox::warning(this, windowTitle(), qs(st.error().message)); };
    if (withBase) {
        if (Status st = writePng(first.base, base); !st) return fail(st);
        written << fromPath(base.filename());
    }
    const bool single = m_levels.size() == 1;
    for (std::size_t i = 0; i < m_levels.size(); ++i) {
        const std::string name = single ? stem : stem + "_level" + std::to_string(i + 1);
        const fs::path img = dir / (name + ".png"), js = dir / (name + ".json");
        Status st = writePng(m_levels[i].image, img);
        if (st) st = writeTextFile(js, tilesetJson(m_levels[i], img.filename().string(), i == 0 && withBase ? base.filename().string() : std::string()));
        if (!st) return fail(st);
        images.push_back(img.filename().string());
        jsons.push_back(js.filename().string());
        written << fromPath(img.filename()) << fromPath(js.filename());
    }
    if (!single) {
        const fs::path set = dir / (stem + "_terrain.json");
        if (Status st = writeTextFile(set, terrainSetJson(m_levels, images, jsons, withBase ? base.filename().string() : std::string())); !st) return fail(st);
        written << fromPath(set.filename());
    }
    QMessageBox::information(this, windowTitle(), tr("Wrote %1.").arg(written.join(QStringLiteral(", "))));
}

void TilesetDialog::openSandbox() {
    if (m_levels.empty()) return;
    TerrainSandbox box(m_levels, this);
    box.exec();
}

// ---------------------------------------------------------------- sandbox

TerrainSandbox::TerrainSandbox(std::vector<Tileset> levels, QWidget* parent) : QDialog(parent), m_levels(std::move(levels)) {
    setWindowTitle(tr("Terrain Sandbox — %1").arg(schemeLabel(m_levels.front().scheme)));
    m_map = sampleMap(static_cast<int>(m_levels.size()));
    m_view = new PixelView(this);
    m_view->setMinimumSize(512, 360);
    m_w = new QSpinBox(this);
    m_w->setRange(2, 128);
    m_w->setValue(m_map.width());
    m_h = new QSpinBox(this);
    m_h->setRange(2, 128);
    m_h->setValue(m_map.height());
    m_brush = new QSpinBox(this);
    m_brush->setRange(1, 8);
    m_brush->setValue(1);
    m_level = new QComboBox(this);
    for (std::size_t i = 1; i <= m_levels.size(); ++i) m_level->addItem(tr("Level %1").arg(i), static_cast<int>(i));
    m_level->setCurrentIndex(0);
    m_level->setVisible(m_levels.size() > 1);
    m_status = new QLabel(this);
    auto* clear = new QPushButton(tr("Clear"), this);
    auto* fill = new QPushButton(tr("Fill"), this);
    auto* noise = new QPushButton(tr("Random"), this);
    auto* exportBtn = new QPushButton(tr("Export Map…"), this);
    auto* close = new QPushButton(tr("Close"), this);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    connect(clear, &QPushButton::clicked, this, [this] { m_map.fill(0); redraw(); });
    connect(fill, &QPushButton::clicked, this, [this] { m_map.fill(m_level->currentData().toInt()); redraw(); });
    connect(noise, &QPushButton::clicked, this, [this] {
        // Smoothed noise so it reads as terrain, not static; higher levels
        // are thresholds of the same field, so they nest inside lower ones.
        static std::uint32_t s = 12345;
        const int w = m_map.width(), h = m_map.height(), n = static_cast<int>(m_levels.size());
        std::vector<double> f(static_cast<std::size_t>(w * h));
        for (auto& v : f) { s = s * 1664525u + 1013904223u; v = (s >> 8) / double(1u << 24); }
        for (int pass = 0; pass < 3; ++pass) {
            std::vector<double> g(f.size());
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    double sum = 0;
                    int c = 0;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int xx = x + dx, yy = y + dy;
                            if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                            sum += f[static_cast<std::size_t>(yy * w + xx)];
                            ++c;
                        }
                    g[static_cast<std::size_t>(y * w + x)] = sum / c;
                }
            f = std::move(g);
        }
        std::vector<double> sorted = f;
        std::sort(sorted.begin(), sorted.end());
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const double v = f[static_cast<std::size_t>(y * w + x)];
                int level = 0;
                for (int k = 1; k <= n; ++k) {
                    // Each level covers a smaller share of the cells than the one below.
                    const double share = 0.55 * (1.0 - double(k - 1) / (n + 0.5));
                    const double thr = sorted[static_cast<std::size_t>(std::clamp((1.0 - share) * double(sorted.size() - 1), 0.0, double(sorted.size() - 1)))];
                    if (v >= thr) level = k;
                }
                m_map.set(x, y, level);
            }
        redraw();
    });
    connect(exportBtn, &QPushButton::clicked, this, &TerrainSandbox::exportMap);
    auto resize = [this] {
        LevelMap n(m_w->value(), m_h->value());
        for (int y = 0; y < n.height(); ++y)
            for (int x = 0; x < n.width(); ++x) n.set(x, y, m_map.at(x, y));
        m_map = n;
        redraw();
    };
    connect(m_w, &QSpinBox::valueChanged, this, resize);
    connect(m_h, &QSpinBox::valueChanged, this, resize);
    connect(m_view, &PixelView::pressed, this, [this](QPoint p, Qt::MouseButton b) { paintAt(p, b == Qt::RightButton); });
    connect(m_view, &PixelView::dragged, this, [this](QPoint p, Qt::MouseButtons b) { paintAt(p, (b & Qt::RightButton) != 0); });
    auto* bar = new QHBoxLayout;
    bar->addWidget(new QLabel(tr("Map"), this));
    bar->addWidget(m_w);
    bar->addWidget(new QLabel(QStringLiteral("×"), this));
    bar->addWidget(m_h);
    bar->addWidget(new QLabel(tr("Brush"), this));
    bar->addWidget(m_brush);
    bar->addWidget(m_level);
    bar->addWidget(clear);
    bar->addWidget(fill);
    bar->addWidget(noise);
    bar->addStretch();
    bar->addWidget(exportBtn);
    bar->addWidget(close);
    auto* root = new QVBoxLayout(this);
    root->addWidget(new QLabel(m_levels.size() > 1 ? tr("Left button paints the chosen level, right button paints the base.")
                                                   : tr("Left button paints terrain, right button erases."),
                               this));
    root->addWidget(m_view, 1);
    root->addWidget(m_status);
    root->addLayout(bar);
    noDefaultButtons(this);
    QDialog::resize(900, 640);
    redraw();
}

void TerrainSandbox::paintAt(QPoint p, bool erase) {
    const int T = m_levels.front().tileSize;
    if (p.x() < 0 || p.y() < 0) return;
    const int cx = p.x() / T, cy = p.y() / T, r = m_brush->value() - 1;
    const int level = erase ? 0 : m_level->currentData().toInt();
    for (int y = cy - r; y <= cy + r; ++y)
        for (int x = cx - r; x <= cx + r; ++x) m_map.set(x, y, level);
    redraw();
}

void TerrainSandbox::redraw() {
    auto img = renderLevels(m_map, m_levels);
    if (!img) { m_status->setText(qs(img.error().message)); return; }
    m_view->setImage(img.value());
    std::set<int> used;
    for (std::size_t k = 0; k < m_levels.size(); ++k) {
        const TilePlacement p = autotile(m_map.atLeast(static_cast<int>(k) + 1), m_levels[k].scheme);
        for (int t : p.tiles) if (t >= 0) used.insert(static_cast<int>(k) * 100 + t);
    }
    m_status->setText(tr("%1 × %2 cells · %3 of %4 tile variants in use").arg(m_map.width()).arg(m_map.height())
                          .arg(used.size()).arg(tileCount(m_levels.front().scheme) * static_cast<int>(m_levels.size())));
}

void TerrainSandbox::exportMap() {
    const QString f = QFileDialog::getSaveFileName(this, tr("Export Map Image"), QStringLiteral("map.png"), tr("PNG image (*.png)"));
    if (f.isEmpty()) return;
    auto img = renderLevels(m_map, m_levels);
    Status st = img ? writePng(img.value(), toPath(f)) : Status(img.error());
    if (!st) QMessageBox::warning(this, windowTitle(), qs(st.error().message));
}

// ---------------------------------------------------------------- seams

void checkDocumentSeams(DocumentView* view, QWidget* parent) {
    const Image img = documentImage(view);
    if (img.isNull()) { QMessageBox::information(parent, QObject::tr("Check Tileset Seams"), QObject::tr("Open a tileset document first.")); return; }
    // Guess the layout from the image proportions.
    std::optional<AutotileScheme> guess;
    for (auto s : {AutotileScheme::Blob47, AutotileScheme::Wang16, AutotileScheme::Cardinal16}) {
        const auto [c, r] = tilesetGrid(s);
        if (img.width() % c == 0 && img.height() % r == 0 && img.width() / c == img.height() / r) { guess = s; break; }
    }
    QStringList items;
    for (auto s : {AutotileScheme::Blob47, AutotileScheme::Wang16, AutotileScheme::Cardinal16}) items << schemeLabel(s);
    bool ok = false;
    const int def = guess == AutotileScheme::Wang16 ? 1 : guess == AutotileScheme::Cardinal16 ? 2 : 0;
    const QString pick = QInputDialog::getItem(parent, QObject::tr("Check Tileset Seams"),
                                               QObject::tr("The document (%1×%2) is a tileset laid out as:").arg(img.width()).arg(img.height()),
                                               items, def, false, &ok);
    if (!ok) return;
    const AutotileScheme s = std::array{AutotileScheme::Blob47, AutotileScheme::Wang16, AutotileScheme::Cardinal16}[static_cast<std::size_t>(items.indexOf(pick))];
    Tileset t;
    t.scheme = s;
    t.image = img;
    // Phase variants make the image a multiple of the plain grid.
    const QStringList phaseItems{QObject::tr("1 (one copy of the grid)"), QObject::tr("2 × 2 copies"), QObject::tr("3 × 3 copies"), QObject::tr("4 × 4 copies")};
    int defPhase = 0;
    const int phase = static_cast<int>(phaseItems.indexOf(QInputDialog::getItem(parent, QObject::tr("Check Tileset Seams"),
                                                               QObject::tr("Phase variants in the image (from a texture larger than one tile):"),
                                                               phaseItems, defPhase, false, &ok))) + 1;
    if (!ok || phase < 1) return;
    t.phases = phase;
    t.tileSize = img.width() / (tilesetGrid(s).first * phase);
    auto rep = checkSeams(t);
    if (!rep) { QMessageBox::warning(parent, QObject::tr("Check Tileset Seams"), qs(rep.error().message)); return; }
    QDialog dlg(parent);
    dlg.setWindowTitle(QObject::tr("Seam Check"));
    auto* list = new QListWidget(&dlg);
    const auto [cols, rows] = tilesetGrid(s);
    (void)rows;
    for (const auto& i : rep.value().issues)
        list->addItem((phase > 1 ? QObject::tr("[variant %1,%2] ").arg(i.phaseX).arg(i.phaseY) : QString()) +
                      QObject::tr("Tile %1 (column %2, row %3) %4 tile %5: jump %6% where the tiles vary %7%")
                          .arg(i.a).arg(i.a % cols + 1).arg(i.a / cols + 1)
                          .arg(i.horizontal ? QObject::tr("→ right edge into") : QObject::tr("↓ bottom edge into"))
                          .arg(i.b).arg(qRound(i.jump * 100)).arg(qRound(i.typical * 100)));
    auto* summary = new QLabel(rep.value().ok()
                                   ? QObject::tr("✓ No seams: %1 pairs of tiles that can touch were compared.").arg(rep.value().pairsChecked)
                                   : QObject::tr("✗ %1 seam(s) in %2 pairs that can touch:").arg(rep.value().issues.size()).arg(rep.value().pairsChecked),
                               &dlg);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto* lay = new QVBoxLayout(&dlg);
    lay->addWidget(summary);
    lay->addWidget(list, 1);
    lay->addWidget(buttons);
    list->setVisible(!rep.value().ok());
    dlg.resize(620, rep.value().ok() ? 120 : 420);
    dlg.exec();
}

} // namespace acestudio::ui
