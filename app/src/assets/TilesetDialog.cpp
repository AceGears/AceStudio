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
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImageReader>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

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
// cells, so every kind of tile shows up.
TerrainMap sampleMap() {
    TerrainMap m(16, 10);
    for (int y = 0; y < 10; ++y)
        for (int x = 0; x < 16; ++x) {
            const double dx = (x - 6.0) / 4.6, dy = (y - 4.5) / 3.6;
            if (dx * dx + dy * dy < 1.0) m.set(x, y, true);
        }
    m.set(6, 4, false);
    m.set(7, 4, false);
    m.set(6, 5, false);
    for (int x = 10; x < 14; ++x) m.set(x, 5, true);
    for (int y = 2; y < 8; ++y) m.set(13, y, true);
    m.set(15, 1, true);
    m.set(15, 8, true);
    m.set(14, 8, true);
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
    settings->addRow(tr("Roundness:"), m_roundness);
    settings->addRow(tr("Edge noise:"), m_noise);
    settings->addRow(tr("Noise detail:"), m_noiseScale);
    settings->addRow(tr("Seed:"), m_seed);
    settings->addRow(QString(), m_outline);

    auto* left = new QVBoxLayout;
    left->addWidget(buildSource(m_terrain, tr("Terrain texture (drawn on top)"), tr("lush green grass")));
    left->addWidget(buildSource(m_base, tr("Base texture (underneath)"), tr("brown dirt ground")));
    left->addLayout(settings);
    left->addStretch();

    m_preview = new PixelView(this);
    m_preview->setMinimumSize(384, 288);
    m_mapPreview = new PixelView(this);
    m_mapPreview->setMinimumSize(384, 240);
    auto* tabs = new QTabWidget(this);
    tabs->addTab(m_mapPreview, tr("Preview map"));
    tabs->addTab(m_preview, tr("Tileset"));
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
        if (m_tileset && m_openImage) m_openImage(m_tileset->image, tr("Tileset %1").arg(schemeLabel(m_tileset->scheme)));
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

    for (QObject* o : std::initializer_list<QObject*>{m_scheme, m_tileSize, m_roundness, m_noise, m_noiseScale, m_seed, m_outline}) {
        if (auto* c = qobject_cast<QComboBox*>(o)) connect(c, &QComboBox::currentIndexChanged, this, &TilesetDialog::rebuild);
        if (auto* sp = qobject_cast<QSpinBox*>(o)) connect(sp, &QSpinBox::valueChanged, this, &TilesetDialog::rebuild);
        if (auto* sl = qobject_cast<QSlider*>(o)) connect(sl, &QSlider::valueChanged, this, &TilesetDialog::rebuild);
        if (auto* cb = qobject_cast<QCheckBox*>(o)) connect(cb, &QCheckBox::toggled, this, &TilesetDialog::rebuild);
    }
    // When the tile size changes, textures are re-fitted from what they were.
    connect(m_tileSize, &QSpinBox::valueChanged, this, [this](int t) {
        for (Source* s : {&m_terrain, &m_base})
            if (!s->texture.isNull() && s->texture.width() != t) setTexture(*s, s->texture);
    });
    resize(980, 640);
    rebuild();
}

QWidget* TilesetDialog::buildSource(Source& s, const QString& title, const QString& defaultPrompt) {
    auto* box = new QGroupBox(title, this);
    s.view = new PixelView(box);
    s.view->setRepeat(3);
    s.view->setMinimumSize(144, 144);
    s.view->setToolTip(tr("Shown 3×3 times to reveal seams"));
    s.status = new QLabel(box);
    s.prompt = new QLineEdit(defaultPrompt, box);
    s.prompt->setToolTip(tr("What the texture shows, for AI generation"));
    auto* fromLayer = new QPushButton(tr("From Document"), box);
    fromLayer->setToolTip(tr("The current frame (or the selection), cropped square and scaled to one tile"));
    auto* open = new QPushButton(tr("Open…"), box);
    s.generate = new QPushButton(tr("Generate"), box);
    s.generate->setToolTip(tr("A seamless texture from Stable Diffusion XL, reduced to one tile"));
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
    auto* v = new QVBoxLayout(box);
    auto* top = new QHBoxLayout;
    top->addWidget(s.view);
    auto* side = new QVBoxLayout;
    side->addLayout(row);
    side->addWidget(s.status);
    side->addStretch();
    top->addLayout(side, 1);
    v->addLayout(top);
    v->addLayout(gen);
    s.status->setText(wrapText({}));
    return box;
}

void TilesetDialog::setTexture(Source& s, const Image& img) {
    s.texture = fitSquare(img, tileSize());
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
    r.prompt = s.prompt->text().trimmed() + tr(", seamless tileable texture, top-down view, flat even lighting, pixel art game tile");
    r.negative = tr("objects, border, frame, perspective, horizon, shadow, vignette, text, watermark");
    r.tileable = QStringLiteral("xy");
    r.pixelWidth = r.pixelHeight = tileSize();
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

void TilesetDialog::rebuild() {
    m_tileset.reset();
    const bool ready = !m_terrain.texture.isNull() && !m_base.texture.isNull();
    m_export->setEnabled(false);
    m_sandbox->setEnabled(false);
    m_open->setEnabled(false);
    if (!ready) {
        m_seams->setText(tr("Choose a terrain and a base texture: from the document, a file, or Generate."));
        m_preview->setImage(Image());
        m_mapPreview->setImage(Image());
        return;
    }
    TilesetStyle st;
    st.roundness = m_roundness->value() / 100.0;
    st.edgeNoise = m_noise->value() / 100.0;
    st.noiseScale = m_noiseScale->value();
    st.outline = m_outline->isChecked();
    st.seed = static_cast<std::uint32_t>(m_seed->value());
    auto t = generateTileset(scheme(), m_base.texture, m_terrain.texture, st);
    if (!t) { m_seams->setText(qs(t.error().message)); return; }
    m_tileset = std::move(t).value();
    m_preview->setImage(m_tileset->image);
    if (auto img = renderMap(sampleMap(), *m_tileset)) m_mapPreview->setImage(img.value());
    const auto rep = checkSeams(*m_tileset);
    QString text;
    if (rep && rep.value().ok()) {
        text = tr("✓ Seam check: %1 tile pairs that can touch, no seams.").arg(rep.value().pairsChecked);
    } else if (rep) {
        text = tr("✗ Seam check: %1 of %2 pairs show a seam (largest: tile %3 → %4). The textures probably do not tile seamlessly.")
                   .arg(rep.value().issues.size()).arg(rep.value().pairsChecked).arg(rep.value().issues.front().a).arg(rep.value().issues.front().b);
    }
    const WrapCheck a = checkWrap(m_terrain.texture), b = checkWrap(m_base.texture);
    if (!(a.horizontal && a.vertical && b.horizontal && b.vertical))
        text += tr("\nA texture does not tile seamlessly, so repeated tiles will show its edges.");
    m_seams->setText(text);
    m_export->setEnabled(true);
    m_sandbox->setEnabled(true);
    m_open->setEnabled(true);
}

void TilesetDialog::exportFiles() {
    if (!m_tileset) return;
    const QString f = QFileDialog::getSaveFileName(this, tr("Export Tileset"), QStringLiteral("tileset.png"), tr("PNG image (*.png)"));
    if (f.isEmpty()) return;
    fs::path png = toPath(f);
    if (png.extension() != ".png") png += ".png";
    const fs::path json = fs::path(png).replace_extension(".json");
    const fs::path base = png.parent_path() / (png.stem().string() + "_base.png");
    const bool withBase = !m_tileset->base.isNull();
    Status st = writePng(m_tileset->image, png);
    if (st && withBase) st = writePng(m_tileset->base, base);
    if (st) st = writeTextFile(json, tilesetJson(*m_tileset, png.filename().string(), withBase ? base.filename().string() : std::string()));
    if (!st) { QMessageBox::warning(this, windowTitle(), qs(st.error().message)); return; }
    QMessageBox::information(this, windowTitle(), tr("Wrote %1 and %2%3.").arg(fromPath(png.filename()), fromPath(json.filename()),
                                                                                  withBase ? tr(" and %1").arg(fromPath(base.filename())) : QString()));
}

void TilesetDialog::openSandbox() {
    if (!m_tileset) return;
    TerrainSandbox box(*m_tileset, this);
    box.exec();
}

// ---------------------------------------------------------------- sandbox

TerrainSandbox::TerrainSandbox(Tileset tileset, QWidget* parent) : QDialog(parent), m_tileset(std::move(tileset)) {
    setWindowTitle(tr("Terrain Sandbox — %1").arg(schemeLabel(m_tileset.scheme)));
    m_map = sampleMap();
    m_view = new PixelView(this);
    m_view->setMinimumSize(512, 360);
    m_view->setMouseTracking(false);
    m_w = new QSpinBox(this);
    m_w->setRange(2, 128);
    m_w->setValue(m_map.width());
    m_h = new QSpinBox(this);
    m_h->setRange(2, 128);
    m_h->setValue(m_map.height());
    m_brush = new QSpinBox(this);
    m_brush->setRange(1, 8);
    m_brush->setValue(1);
    m_status = new QLabel(this);
    auto* clear = new QPushButton(tr("Clear"), this);
    auto* fill = new QPushButton(tr("Fill"), this);
    auto* noise = new QPushButton(tr("Random"), this);
    auto* exportBtn = new QPushButton(tr("Export Map…"), this);
    auto* close = new QPushButton(tr("Close"), this);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    connect(clear, &QPushButton::clicked, this, [this] { m_map.fill(false); redraw(); });
    connect(fill, &QPushButton::clicked, this, [this] { m_map.fill(true); redraw(); });
    connect(noise, &QPushButton::clicked, this, [this] {
        // Smoothed noise so it reads as terrain, not static.
        std::uint32_t s = static_cast<std::uint32_t>(m_map.width() * 7919 + m_map.height() * 104729 + m_status->text().size());
        TerrainMap r(m_map.width(), m_map.height());
        for (int y = 0; y < r.height(); ++y)
            for (int x = 0; x < r.width(); ++x) {
                s = s * 1664525u + 1013904223u;
                r.set(x, y, (s >> 24) < 120);
            }
        for (int pass = 0; pass < 2; ++pass) {
            TerrainMap n = r;
            for (int y = 0; y < r.height(); ++y)
                for (int x = 0; x < r.width(); ++x) {
                    int c = 0;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) c += r.at(x + dx, y + dy);
                    n.set(x, y, c >= 5);
                }
            r = n;
        }
        m_map = r;
        redraw();
    });
    connect(exportBtn, &QPushButton::clicked, this, &TerrainSandbox::exportMap);
    auto resize = [this] {
        TerrainMap n(m_w->value(), m_h->value());
        for (int y = 0; y < n.height(); ++y)
            for (int x = 0; x < n.width(); ++x) n.set(x, y, m_map.at(x, y));
        m_map = n;
        redraw();
    };
    connect(m_w, &QSpinBox::valueChanged, this, resize);
    connect(m_h, &QSpinBox::valueChanged, this, resize);
    connect(m_view, &PixelView::pressed, this, [this](QPoint p, Qt::MouseButton b) { paintAt(p, b != Qt::RightButton); });
    connect(m_view, &PixelView::dragged, this, [this](QPoint p, Qt::MouseButtons b) { paintAt(p, !(b & Qt::RightButton)); });
    auto* bar = new QHBoxLayout;
    bar->addWidget(new QLabel(tr("Map"), this));
    bar->addWidget(m_w);
    bar->addWidget(new QLabel(QStringLiteral("×"), this));
    bar->addWidget(m_h);
    bar->addWidget(new QLabel(tr("Brush"), this));
    bar->addWidget(m_brush);
    bar->addWidget(clear);
    bar->addWidget(fill);
    bar->addWidget(noise);
    bar->addStretch();
    bar->addWidget(exportBtn);
    bar->addWidget(close);
    auto* root = new QVBoxLayout(this);
    root->addWidget(new QLabel(tr("Left button paints terrain, right button erases."), this));
    root->addWidget(m_view, 1);
    root->addWidget(m_status);
    root->addLayout(bar);
    QDialog::resize(900, 640);
    redraw();
}

void TerrainSandbox::paintAt(QPoint p, bool terrain) {
    const int T = m_tileset.tileSize;
    if (p.x() < 0 || p.y() < 0) return;
    const int cx = p.x() / T, cy = p.y() / T, r = m_brush->value() - 1;
    for (int y = cy - r; y <= cy + r; ++y)
        for (int x = cx - r; x <= cx + r; ++x) m_map.set(x, y, terrain);
    redraw();
}

void TerrainSandbox::redraw() {
    auto img = renderMap(m_map, m_tileset);
    if (!img) { m_status->setText(qs(img.error().message)); return; }
    m_view->setImage(img.value());
    const TilePlacement p = autotile(m_map, m_tileset.scheme);
    std::set<int> used;
    for (int t : p.tiles) if (t >= 0) used.insert(t);
    m_status->setText(tr("%1 × %2 cells · %3 of %4 tile variants in use").arg(m_map.width()).arg(m_map.height())
                          .arg(used.size()).arg(tileCount(m_tileset.scheme)));
}

void TerrainSandbox::exportMap() {
    const QString f = QFileDialog::getSaveFileName(this, tr("Export Map Image"), QStringLiteral("map.png"), tr("PNG image (*.png)"));
    if (f.isEmpty()) return;
    auto img = renderMap(m_map, m_tileset);
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
    t.tileSize = img.width() / tilesetGrid(s).first;
    auto rep = checkSeams(t);
    if (!rep) { QMessageBox::warning(parent, QObject::tr("Check Tileset Seams"), qs(rep.error().message)); return; }
    QDialog dlg(parent);
    dlg.setWindowTitle(QObject::tr("Seam Check"));
    auto* list = new QListWidget(&dlg);
    const auto [cols, rows] = tilesetGrid(s);
    (void)rows;
    for (const auto& i : rep.value().issues)
        list->addItem(QObject::tr("Tile %1 (column %2, row %3) %4 tile %5: jump %6% where the tiles vary %7%")
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
