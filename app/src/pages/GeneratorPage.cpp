#include "GeneratorPage.h"

#include "../AppContext.h"
#include "../DocumentView.h"
#include "../QtBridge.h"
#include "../ai/AiService.h"
#include "../ui/Icons.h"

#include "acestudio/core/FileUtil.h"
#include "acestudio/core/ModelCatalog.h"
#include "acestudio/core/PngCodec.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QGridLayout>
#include <QPainter>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QSplitter>
#include <QVBoxLayout>

namespace acestudio::ui {

using namespace acestudio::ai;
using nlohmann::json;

namespace {

const ModelEntry* installed(AiService& ai, const char* id) {
    if (!ai.catalog() || !ai.store()) return nullptr;
    const ModelEntry* m = ai.catalog()->find(id);
    return m && ai.store()->state(*m) == InstallState::Installed ? m : nullptr;
}

std::string utf8(const std::filesystem::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

QPixmap thumb(const Image& img, int box) {
    QImage q = toQImage(img);
    if (q.isNull()) return {};
    const int scale = std::max(1, box / std::max(q.width(), q.height()));
    if (scale > 1) q = q.scaled(q.width() * scale, q.height() * scale, Qt::KeepAspectRatio, Qt::FastTransformation);
    if (q.width() > box || q.height() > box) q = q.scaled(box, box, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    // A checkerboard behind transparent pixels.
    QPixmap pm(q.size());
    pm.fill(QColor("#20242C"));
    QPainter p(&pm);
    const int c = 8;
    for (int y = 0; y < pm.height(); y += c)
        for (int x = 0; x < pm.width(); x += c)
            if (((x / c) + (y / c)) % 2) p.fillRect(x, y, c, c, QColor("#2A2F39"));
    p.drawImage(0, 0, q);
    return pm;
}

QString severityMark(gen::Severity s) {
    return s == gen::Severity::Problem ? QStringLiteral("✖") : s == gen::Severity::Warning ? QStringLiteral("▲") : QStringLiteral("•");
}

} // namespace

GeneratorPage::GeneratorPage(AiService& ai, AppContext& ctx, QWidget* parent) : QWidget(parent), m_ai(ai), m_ctx(ctx) {
    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    auto* split = new QSplitter(Qt::Horizontal, this);
    split->addWidget(buildForm());
    split->addWidget(buildResults());
    split->setStretchFactor(1, 1);
    split->setSizes({400, 900});
    lay->addWidget(split);
    connect(&m_ai, &AiService::modelsChanged, this, &GeneratorPage::refreshModels);
    connect(&m_ai, &AiService::stateChanged, this, &GeneratorPage::refreshModels);
    connect(&m_ai, &AiService::jobChanged, this, &GeneratorPage::onJobChanged);
    connect(&m_ai, &AiService::jobFinished, this, &GeneratorPage::onJobFinished);
    refreshModels();
    refreshReferences();
    showResult(-1);
}

QWidget* GeneratorPage::buildForm() {
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setMinimumWidth(380);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* w = new QWidget(scroll);
    scroll->setWidget(w);
    auto* col = new QVBoxLayout(w);
    col->setContentsMargins(24, 20, 16, 20);
    col->setSpacing(8);

    auto* eyebrow = new QLabel(tr("AI STUDIO"), w);
    eyebrow->setProperty("role", "eyebrow");
    auto* title = new QLabel(tr("Generate an asset"), w);
    title->setProperty("role", "h1");
    col->addWidget(eyebrow);
    col->addWidget(title);

    auto* describe = new QLabel(tr("1  &Describe"), w);
    describe->setProperty("role", "h2");
    col->addWidget(describe);
    m_prompt = new QPlainTextEdit(w);
    m_prompt->setPlaceholderText(tr("A detailed pixel-art fantasy blacksmith in heavy leather armour, hammering at a glowing forge"));
    m_prompt->setAccessibleName(tr("Description"));
    m_prompt->setTabChangesFocus(true);
    m_prompt->setFixedHeight(92);
    describe->setBuddy(m_prompt);
    col->addWidget(m_prompt);

    auto* configure = new QLabel(tr("2  Configure"), w);
    configure->setProperty("role", "h2");
    col->addWidget(configure);
    auto* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_type = new QComboBox(w);
    for (const auto& t : gen::assetTypes()) m_type->addItem(tr(t.label), QString::fromLatin1(t.key));
    form->addRow(tr("Asset &type:"), m_type);
    m_style = new QComboBox(w);
    for (const auto& s : gen::artStyles()) m_style->addItem(tr(s.label), QString::fromLatin1(s.key));
    form->addRow(tr("Art st&yle:"), m_style);
    m_perspective = new QComboBox(w);
    for (const auto& p : gen::perspectives()) m_perspective->addItem(tr(p.label), QString::fromLatin1(p.key));
    form->addRow(tr("&Perspective:"), m_perspective);

    m_size = new QComboBox(w);
    m_size->setAccessibleName(tr("Output size"));
    auto* sizeRow = new QHBoxLayout;
    m_customW = new QSpinBox(w);
    m_customH = new QSpinBox(w);
    for (QSpinBox* s : {m_customW, m_customH}) {
        s->setRange(8, 1024);
        s->setValue(64);
    }
    m_customW->setAccessibleName(tr("Custom width"));
    m_customH->setAccessibleName(tr("Custom height"));
    sizeRow->addWidget(m_size, 1);
    sizeRow->addWidget(m_customW);
    auto* times = new QLabel(QStringLiteral("×"), w);
    times->setObjectName(QStringLiteral("sizeTimes"));
    sizeRow->addWidget(times);
    sizeRow->addWidget(m_customH);
    form->addRow(tr("Output size:"), sizeRow);
    auto fillSizes = [this] {
        const QString cur = m_size->currentData().toString();
        m_size->blockSignals(true);
        m_size->clear();
        if (pixelStyle()) {
            for (int s : {16, 24, 32, 48, 64, 96, 128, 192, 256}) m_size->addItem(QStringLiteral("%1 × %1 px").arg(s), QStringLiteral("%1x%1").arg(s));
            m_size->addItem(tr("Wide 320 × 180"), QStringLiteral("320x180"));
            m_size->addItem(tr("Wide 480 × 270"), QStringLiteral("480x270"));
        } else {
            m_size->addItem(QStringLiteral("512 × 512"), QStringLiteral("512x512"));
            m_size->addItem(QStringLiteral("768 × 768"), QStringLiteral("768x768"));
            m_size->addItem(QStringLiteral("1024 × 1024"), QStringLiteral("1024x1024"));
            m_size->addItem(QStringLiteral("1024 × 576"), QStringLiteral("1024x576"));
        }
        if (m_view) m_size->addItem(tr("Open document's canvas"), QStringLiteral("doc"));
        m_size->addItem(tr("Custom"), QStringLiteral("custom"));
        const int i = m_size->findData(cur);
        m_size->setCurrentIndex(i >= 0 ? i : (pixelStyle() ? 4 : 2));
        m_size->blockSignals(false);
        const bool custom = m_size->currentData().toString() == QLatin1String("custom");
        m_customW->setVisible(custom);
        m_customH->setVisible(custom);
        if (auto* t = findChild<QLabel*>(QStringLiteral("sizeTimes"))) t->setVisible(custom);
    };
    connect(m_style, &QComboBox::currentIndexChanged, this, fillSizes);
    connect(m_size, &QComboBox::currentIndexChanged, this, [this] {
        const bool custom = m_size->currentData().toString() == QLatin1String("custom");
        m_customW->setVisible(custom);
        m_customH->setVisible(custom);
        if (auto* t = findChild<QLabel*>(QStringLiteral("sizeTimes"))) t->setVisible(custom);
    });
    fillSizes();

    m_palette = new QComboBox(w);
    m_palette->addItem(tr("Automatic, 16 colours"), 16);
    m_palette->addItem(tr("Automatic, 32 colours"), 32);
    m_palette->addItem(tr("Automatic, 64 colours"), 64);
    m_palette->addItem(tr("The open document's palette"), -1);
    m_palette->setCurrentIndex(1);
    form->addRow(tr("Pa&lette:"), m_palette);
    m_transparent = new QCheckBox(tr("Transparent background"), w);
    m_transparent->setChecked(true);
    form->addRow(QString(), m_transparent);
    col->addLayout(form);

    auto* refLabel = new QLabel(tr("&References (up to 4)"), w);
    refLabel->setProperty("role", "h2");
    col->addWidget(refLabel);
    auto* refHint = new QLabel(tr("Images the result should follow: a character to keep, an art style, a pose. Needs FLUX.2 klein."), w);
    refHint->setProperty("role", "muted");
    refHint->setWordWrap(true);
    col->addWidget(refHint);
    m_refs = new QListWidget(w);
    m_refs->setAccessibleName(tr("Reference images"));
    m_refs->setViewMode(QListView::IconMode);
    m_refs->setIconSize(QSize(64, 64));
    m_refs->setFixedHeight(96);
    m_refs->setFlow(QListView::LeftToRight);
    refLabel->setBuddy(m_refs);
    col->addWidget(m_refs);
    auto* refButtons = new QHBoxLayout;
    auto* fromFile = new QPushButton(icon(Glyph::Image), tr("Add Image…"), w);
    auto* fromDoc = new QPushButton(tr("Add Open Frame"), w);
    auto* removeRef = new QPushButton(tr("Remove"), w);
    refButtons->addWidget(fromFile);
    refButtons->addWidget(fromDoc);
    refButtons->addWidget(removeRef);
    refButtons->addStretch();
    col->addLayout(refButtons);
    connect(fromFile, &QPushButton::clicked, this, [this] {
        const QString f = QFileDialog::getOpenFileName(this, tr("Reference Image"), QString(), tr("Images (*.png)"));
        if (f.isEmpty()) return;
        auto img = readPng(toPath(f));
        if (!img) return void(QMessageBox::warning(this, tr("Reference Image"), qs(img.error().message)));
        addReference(img.value(), QFileInfo(f).completeBaseName());
    });
    connect(fromDoc, &QPushButton::clicked, this, [this] {
        if (!m_view) return void(Q_EMIT statusMessage(tr("Open a project first.")));
        addReference(m_view->document().composite(m_view->currentFrame()), m_view->displayName());
    });
    connect(removeRef, &QPushButton::clicked, this, [this] {
        const int r = m_refs->currentRow();
        if (r >= 0 && r < static_cast<int>(m_references.size())) m_references.erase(m_references.begin() + r);
        refreshReferences();
    });

    auto* genLabel = new QLabel(tr("3  Generate"), w);
    genLabel->setProperty("role", "h2");
    col->addWidget(genLabel);
    auto* form2 = new QFormLayout;
    form2->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_model = new QComboBox(w);
    form2->addRow(tr("&Model:"), m_model);
    m_preset = new QComboBox(w);
    for (const auto& p : gen::qualityPresets()) m_preset->addItem(tr(p.label), QString::fromLatin1(p.key));
    m_preset->setCurrentIndex(1);
    form2->addRow(tr("&Quality:"), m_preset);
    m_presetInfo = new QLabel(w);
    m_presetInfo->setProperty("role", "muted");
    m_presetInfo->setWordWrap(true);
    form2->addRow(QString(), m_presetInfo);
    auto updatePreset = [this] {
        const auto p = gen::presetFromKey(m_preset->currentData().toString().toStdString()).value_or(gen::QualityPreset::Balanced);
        const auto& i = gen::presetInfo(p);
        m_presetInfo->setText(tr("%1 FLUX.2 klein: %2 steps; SDXL: %3 steps; generated at up to %4 px.")
                                  .arg(tr(i.description)).arg(i.kleinSteps).arg(i.sdxlSteps).arg(i.longSide));
    };
    connect(m_preset, &QComboBox::currentIndexChanged, this, updatePreset);
    updatePreset();
    m_count = new QSpinBox(w);
    m_count->setRange(1, 8);
    m_count->setValue(4);
    form2->addRow(tr("&Variations:"), m_count);
    m_seed = new QSpinBox(w);
    m_seed->setRange(-1, 2147483647);
    m_seed->setValue(-1);
    m_seed->setSpecialValueText(tr("Random"));
    form2->addRow(tr("&Seed:"), m_seed);
    col->addLayout(form2);

    auto* go = new QHBoxLayout;
    m_generate = new QPushButton(icon(Glyph::Sparkle), tr("Generate"), w);
    m_generate->setProperty("accent", true);
    m_generate->setMinimumHeight(36);
    m_cancel = new QPushButton(tr("Cancel"), w);
    m_cancel->setEnabled(false);
    go->addWidget(m_generate, 1);
    go->addWidget(m_cancel);
    col->addLayout(go);
    auto* models = new QPushButton(tr("Models…"), w);
    connect(models, &QPushButton::clicked, this, &GeneratorPage::showModelsRequested);
    col->addWidget(models, 0, Qt::AlignLeft);
    col->addStretch();
    connect(m_generate, &QPushButton::clicked, this, [this] { generate(); });
    connect(m_cancel, &QPushButton::clicked, this, [this] {
        if (!m_job.empty()) m_ai.cancel(m_job);
    });
    return scroll;
}

QWidget* GeneratorPage::buildResults() {
    auto* w = new QWidget(this);
    auto* col = new QVBoxLayout(w);
    col->setContentsMargins(16, 20, 24, 16);
    col->setSpacing(10);
    auto* top = new QHBoxLayout;
    m_progress = new QProgressBar(w);
    m_progress->setAccessibleName(tr("Generation progress"));
    m_progress->setRange(0, 1000);
    m_progress->setTextVisible(false);
    m_progress->setVisible(false);
    m_status = new QLabel(tr("Results appear here. Each one is checked for cut-off parts, stray pixels, soft edges and colours."), w);
    m_status->setProperty("role", "muted");
    m_status->setWordWrap(true);
    top->addWidget(m_status, 1);
    col->addLayout(top);
    col->addWidget(m_progress);

    m_results = new QListWidget(w);
    m_results->setAccessibleName(tr("Results"));
    m_results->setViewMode(QListView::IconMode);
    m_results->setIconSize(QSize(140, 140));
    m_results->setGridSize(QSize(170, 190));
    m_results->setResizeMode(QListView::Adjust);
    m_results->setMovement(QListView::Static);
    m_results->setMinimumHeight(220);
    col->addWidget(m_results, 2);
    connect(m_results, &QListWidget::currentRowChanged, this, &GeneratorPage::showResult);

    // 4 Refine: the selected result, its check and the actions.
    auto* detail = new QFrame(w);
    detail->setProperty("card", true);
    auto* dl = new QHBoxLayout(detail);
    dl->setContentsMargins(14, 12, 14, 12);
    m_preview = new QLabel(detail);
    m_preview->setFixedSize(200, 200);
    m_preview->setAlignment(Qt::AlignCenter);
    m_preview->setAccessibleName(tr("Selected result"));
    dl->addWidget(m_preview);
    auto* right = new QVBoxLayout;
    m_report = new QLabel(detail);
    m_report->setWordWrap(true);
    m_report->setTextFormat(Qt::RichText);
    m_report->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    right->addWidget(m_report, 1);
    auto* actions = new QGridLayout;
    auto act = [&](const QString& text, int r, int c, std::function<void(Result&)> run) {
        auto* b = new QPushButton(text, detail);
        actions->addWidget(b, r, c);
        m_resultActions << b;
        connect(b, &QPushButton::clicked, this, [this, run] {
            if (Result* res = selected()) run(*res);
        });
        return b;
    };
    act(tr("Open as Document"), 0, 0, [this](Result& r) { Q_EMIT openAsDocument(r.image, r.name); })->setProperty("accent", true);
    act(tr("Add as Layer"), 0, 1, [this](Result& r) {
        if (!m_view) return void(Q_EMIT statusMessage(tr("Open a project first, or use Open as Document.")));
        Q_EMIT addAsLayer(r.image, r.name);
    });
    act(tr("Animate…"), 0, 2, [this](Result& r) { Q_EMIT animateRequested(r.image, r.name); });
    act(tr("More Like This"), 1, 0, [this](Result& r) { generate({r.raw.isNull() ? r.image : r.raw}); });
    act(tr("Use as Reference"), 1, 1, [this](Result& r) { addReference(r.raw.isNull() ? r.image : r.raw, r.name); });
    act(tr("Save PNG…"), 1, 2, [this](Result& r) {
        const QString f = QFileDialog::getSaveFileName(this, tr("Save PNG"), r.name + QStringLiteral(".png"), tr("PNG (*.png)"));
        if (f.isEmpty()) return;
        if (auto st = writePng(r.image, toPath(f)); !st) QMessageBox::warning(this, tr("Save PNG"), qs(st.error().message));
        else Q_EMIT statusMessage(tr("Saved %1").arg(QDir::toNativeSeparators(f)));
    });
    right->addLayout(actions);
    dl->addLayout(right, 1);
    col->addWidget(detail, 1);
    return w;
}

void GeneratorPage::setView(DocumentView* view) {
    m_view = view;
    // The size list offers the open canvas only while one is open.
    const int keep = m_style->currentIndex();
    m_style->setCurrentIndex(keep);
    Q_EMIT m_style->currentIndexChanged(keep);
}

void GeneratorPage::setPrompt(const QString& prompt, const QString& typeKey) {
    if (!prompt.isEmpty()) m_prompt->setPlainText(prompt);
    if (!typeKey.isEmpty())
        if (const int i = m_type->findData(typeKey.toLower().replace(QLatin1String("ui element"), QStringLiteral("ui"))); i >= 0)
            m_type->setCurrentIndex(i);
    m_prompt->setFocus();
}

void GeneratorPage::addReference(const Image& img, const QString& name) {
    if (img.isNull()) return;
    if (m_references.size() >= 4) {
        Q_EMIT statusMessage(tr("Up to four references; remove one first."));
        return;
    }
    m_references.push_back({img, name});
    refreshReferences();
}

void GeneratorPage::refreshReferences() {
    m_refs->clear();
    for (const Reference& r : m_references) {
        auto* it = new QListWidgetItem(QIcon(thumb(r.image, 64)), r.name.left(14));
        it->setToolTip(r.name);
        m_refs->addItem(it);
    }
}

void GeneratorPage::refreshModels() {
    const QString keep = m_model->currentData().toString();
    m_model->clear();
    const bool klein = installed(m_ai, "flux2-klein-4b");
    const bool sdxl = installed(m_ai, "sdxl-base-1.0");
    m_model->addItem(tr("Automatic"), QStringLiteral("auto"));
    m_model->addItem(klein ? tr("FLUX.2 [klein] 4B") : tr("FLUX.2 [klein] 4B (not installed)"), QStringLiteral("klein"));
    m_model->addItem(sdxl ? tr("SDXL + Pixel Art XL") : tr("SDXL (not installed)"), QStringLiteral("sdxl"));
    const int i = m_model->findData(keep);
    m_model->setCurrentIndex(i >= 0 ? i : 0);
}

bool GeneratorPage::pixelStyle() const {
    const auto s = gen::artStyleFromKey(m_style->currentData().toString().toStdString()).value_or(gen::ArtStyle::DetailedPixel);
    return gen::isPixelStyle(s);
}

gen::Spec GeneratorPage::spec() const {
    gen::Spec s;
    s.description = m_prompt->toPlainText().trimmed().toStdString();
    s.type = gen::assetTypeFromKey(m_type->currentData().toString().toStdString()).value_or(gen::AssetType::Character);
    s.style = gen::artStyleFromKey(m_style->currentData().toString().toStdString()).value_or(gen::ArtStyle::DetailedPixel);
    s.perspective = gen::perspectiveFromKey(m_perspective->currentData().toString().toStdString()).value_or(gen::Perspective::Side);
    s.transparent = m_transparent->isChecked() && gen::isSingleSubject(s.type);
    return s;
}

std::pair<int, int> GeneratorPage::outputSize() const {
    const QString key = m_size->currentData().toString();
    if (key == QLatin1String("custom")) return {m_customW->value(), m_customH->value()};
    if (key == QLatin1String("doc") && m_view) return {m_view->document().width(), m_view->document().height()};
    const QStringList wh = key.split(QLatin1Char('x'));
    return wh.size() == 2 ? std::pair{wh[0].toInt(), wh[1].toInt()} : std::pair{64, 64};
}

gen::CheckOptions GeneratorPage::checkOptions() const {
    const gen::Spec s = spec();
    gen::CheckOptions o;
    o.pixelArt = gen::isPixelStyle(s.style);
    o.expectTransparent = s.transparent;
    o.singleSubject = gen::isSingleSubject(s.type);
    if (o.pixelArt) {
        const auto [w, h] = outputSize();
        o.expectWidth = w;
        o.expectHeight = h;
        const int pal = m_palette->currentData().toInt();
        o.maxColors = pal > 0 ? pal : (m_view ? static_cast<int>(m_view->document().palette().size()) : 0);
    }
    return o;
}

void GeneratorPage::generate(const std::vector<Image>& extraRefs) {
    if (!m_job.empty()) return void(Q_EMIT statusMessage(tr("A generation is already running.")));
    const gen::Spec s = spec();
    if (s.description.empty()) {
        m_prompt->setFocus();
        return void(Q_EMIT statusMessage(tr("Describe the asset first.")));
    }
    // Choose the backend: klein when installed (and wanted), else SDXL.
    const QString choice = m_model->currentData().toString();
    const ModelEntry* klein = installed(m_ai, "flux2-klein-4b");
    const ModelEntry* sdxl = installed(m_ai, "sdxl-base-1.0");
    gen::Backend backend;
    if (choice == QLatin1String("klein") || (choice == QLatin1String("auto") && klein)) {
        if (!klein) return void(Q_EMIT showModelsRequested());
        backend = gen::Backend::Klein;
    } else {
        if (!sdxl) return void(Q_EMIT showModelsRequested());
        backend = gen::Backend::Sdxl;
    }
    std::vector<const Image*> refs;
    for (const Reference& r : m_references) refs.push_back(&r.image);
    for (const Image& e : extraRefs) refs.insert(refs.begin(), &e);
    if (refs.size() > 4) refs.resize(4);
    if (!refs.empty() && backend == gen::Backend::Sdxl)
        Q_EMIT statusMessage(tr("References need FLUX.2 klein; generating from the description only."));

    const bool pixel = gen::isPixelStyle(s.style);
    const ModelEntry* lora = pixel ? installed(m_ai, backend == gen::Backend::Klein ? "pixel-art-klein-lora" : "pixel-art-xl") : nullptr;
    const gen::Prompt prompt = gen::composePrompt(s, backend, lora != nullptr);
    const auto preset = gen::presetFromKey(m_preset->currentData().toString().toStdString()).value_or(gen::QualityPreset::Balanced);
    const auto [outW, outH] = outputSize();
    auto [gw, gh] = pixel ? gen::generationSize(outW, outH, preset) : std::pair{outW / 16 * 16, outH / 16 * 16};
    const auto& info = gen::presetInfo(preset);
    const ModelEntry* model = backend == gen::Backend::Klein ? klein : sdxl;

    json p = {{"model_dir", utf8(m_ai.store()->entryPath(*model))},
              {"prompt", prompt.positive},
              {"width", gw},
              {"height", gh},
              {"steps", backend == gen::Backend::Klein ? info.kleinSteps : info.sdxlSteps},
              {"count", m_count->value()},
              {"seed", m_seed->value()},
              {"model_id", model->id}};
    if (backend == gen::Backend::Sdxl) {
        p["negative_prompt"] = prompt.negative;
        p["guidance"] = 6.0;
        if (gen::wantsTileable(s.type)) p["tileable"] = "xy";
    }
    if (lora) p["lora"] = {{"path", utf8(m_ai.store()->entryPath(*lora))}, {"scale", 1.0}};
    if (s.transparent) {
        const ModelEntry* bg = installed(m_ai, "birefnet-general");
        if (!bg) bg = installed(m_ai, "birefnet-lite");
        if (bg) p["cutout"] = {{"model", utf8(m_ai.store()->entryPath(*bg))}, {"threshold", 0.5}};
        else Q_EMIT statusMessage(tr("Install a BiRefNet model for transparent backgrounds (Models…)."));
    }
    if (pixel) {
        p["pixel"] = {{"width", outW}, {"height", outH}, {"crop", gen::isSingleSubject(s.type)}};
        const int pal = m_palette->currentData().toInt();
        if (pal < 0 && m_view && m_view->document().palette().size() > 0) {
            json colours = json::array();
            for (std::size_t i = 0; i < m_view->document().palette().size(); ++i) colours.push_back(m_view->document().palette().at(i).toHex(false));
            p["pixel"]["palette"] = colours;
        } else {
            p["pixel"]["colors"] = pal > 0 ? pal : 32;
        }
    }
    std::vector<std::pair<std::string, const Image*>> images;
    if (backend == gen::Backend::Klein)
        for (std::size_t i = 0; i < refs.size(); ++i) images.push_back({"reference" + std::to_string(i), refs[i]});

    const std::string task = backend == gen::Backend::Klein ? "image.klein" : "image.generate";
    if (m_ai.catalog() && m_ai.state() == WorkerState::Ready)
        if (auto problem = runtimeProblem(*model, m_ai.capabilities())) {
            QMessageBox::information(this, tr("AI Studio"), qs(*problem));
            return;
        }
    p.erase("model_id");
    auto id = m_ai.submit(task, std::move(p), tr("Generate: %1").arg(QString::fromStdString(s.description).left(40)), images);
    if (!id) {
        QMessageBox::information(this, tr("AI Studio"), qs(id.error().message));
        if (!m_ai.runtimeInstalled()) Q_EMIT showSetupRequested();
        return;
    }
    m_job = id.value();
    m_jobPixel = pixel;
    m_generate->setEnabled(false);
    m_cancel->setEnabled(true);
    m_progress->setValue(0);
    m_progress->setVisible(true);
    m_status->setText(tr("Queued…"));
}

void GeneratorPage::onJobChanged(const QString& qid) {
    if (qid.toStdString() != m_job) return;
    const Job* j = m_ai.job(m_job);
    if (!j) return;
    if (j->progress >= 0) m_progress->setValue(static_cast<int>(j->progress * 1000));
    if (!j->message.empty()) m_status->setText(qs(j->message));
}

void GeneratorPage::onJobFinished(const QString& qid) {
    if (qid.toStdString() != m_job) return;
    const Job* j = m_ai.job(m_job);
    m_job.clear();
    m_generate->setEnabled(true);
    m_cancel->setEnabled(false);
    m_progress->setVisible(false);
    if (!j) return;
    if (j->state != JobState::Succeeded) {
        m_status->setText(j->state == JobState::Cancelled ? tr("Cancelled.") : tr("Failed: %1").arg(qs(j->error).section(QLatin1Char('\n'), 0, 0)));
        return;
    }
    const gen::CheckOptions opt = checkOptions();
    const long long seed = j->info.value("seed", 0LL);
    int added = 0;
    for (int i = 0; i < 8; ++i) {
        auto o = j->outputs.find("image" + std::to_string(i));
        if (o == j->outputs.end()) break;
        auto img = readPng(o->second);
        if (!img) continue;
        Result r;
        r.image = std::move(img).value();
        if (auto raw = j->outputs.find("raw" + std::to_string(i)); raw != j->outputs.end())
            if (auto rimg = readPng(raw->second)) r.raw = std::move(rimg).value();
        r.seed = seed + i;
        r.name = QString::fromStdString(spec().description).left(24) + QStringLiteral(" #%1").arg(r.seed);
        r.report = gen::checkAsset(r.image, opt);
        const int score = r.report.score();
        auto* it = new QListWidgetItem(QIcon(thumb(r.image, 140)),
                                       QStringLiteral("%1  %2").arg(score >= 90 ? QStringLiteral("●") : score >= 70 ? QStringLiteral("▲") : QStringLiteral("✖"))
                                           .arg(tr("seed %1").arg(r.seed)));
        it->setToolTip(tr("Check score %1/100").arg(score));
        // Newest first: row i of the grid is m_resultList[i].
        m_resultList.insert(m_resultList.begin(), std::move(r));
        m_results->insertItem(0, it);
        ++added;
    }
    const json& info = j->info;
    QString timing;
    if (info.contains("secondsPerStep"))
        timing = tr(" · %1 s per step, model load %2 s").arg(info.value("secondsPerStep", 0.0)).arg(info.value("loadSeconds", 0.0));
    if (info.contains("peakVramGb")) timing += tr(" · peak VRAM %1 GB").arg(info.value("peakVramGb", 0.0));
    m_status->setText(tr("%n result(s), seed %1%2", nullptr, added).arg(seed).arg(timing));
    if (added) m_results->setCurrentRow(0);
}

GeneratorPage::Result* GeneratorPage::selected() {
    const int r = m_results->currentRow();
    return r >= 0 && r < static_cast<int>(m_resultList.size()) ? &m_resultList[static_cast<std::size_t>(r)] : nullptr;
}

void GeneratorPage::showResult(int index) {
    const bool has = index >= 0 && index < static_cast<int>(m_resultList.size());
    for (QPushButton* b : m_resultActions) b->setEnabled(has);
    if (!has) {
        m_preview->setPixmap({});
        m_report->setText(tr("<b>4  Refine</b><br>Select a result to check it, open it, add it as a layer, animate it or make more like it."));
        return;
    }
    const Result& r = m_resultList[static_cast<std::size_t>(index)];
    m_preview->setPixmap(thumb(r.image, 200));
    QString html = tr("<b>%1 × %2 px</b>, %3 colours, score %4/100<br>").arg(r.image.width()).arg(r.image.height())
                       .arg(r.report.colors).arg(r.report.score());
    if (r.report.issues.empty()) html += tr("No problems found.");
    for (const gen::Issue& i : r.report.issues) html += severityMark(i.severity) + QStringLiteral(" ") + qs(i.message).toHtmlEscaped() + QStringLiteral("<br>");
    m_report->setText(html);
}

} // namespace acestudio::ui
