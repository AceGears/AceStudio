#include "GeneratorPage.h"

#include "../AppContext.h"
#include "../DocumentView.h"
#include "../QtBridge.h"
#include "../ai/AiService.h"
#include "../ui/Icons.h"

#include "acestudio/core/FileUtil.h"
#include "acestudio/core/ModelCatalog.h"
#include "acestudio/core/AssetFiles.h"
#include "acestudio/core/PaletteIO.h"
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
#include <QRegularExpression>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDialog>
#include <QLineEdit>
#include <QInputDialog>
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

GeneratorPage::GeneratorPage(AiService& ai, AppContext& ctx, QWidget* parent)
    : QWidget(parent), m_ai(ai), m_ctx(ctx), m_styles(ctx.dataDir() / "styles") {
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
    refreshStyles();
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
    auto* roleRow = new QHBoxLayout;
    auto* roleLabel = new QLabel(tr("Selected reference is &for:"), w);
    m_role = new QComboBox(w);
    for (style::RefRole r : {style::RefRole::Character, style::RefRole::Style, style::RefRole::Palette, style::RefRole::Environment,
                             style::RefRole::Creature, style::RefRole::Equipment, style::RefRole::Animation, style::RefRole::Composition})
        m_role->addItem(tr(style::roleLabel(r)), QString::fromLatin1(style::toString(r)));
    roleLabel->setBuddy(m_role);
    roleRow->addWidget(roleLabel);
    roleRow->addWidget(m_role, 1);
    col->addLayout(roleRow);
    connect(m_refs, &QListWidget::currentRowChanged, this, [this](int r) {
        m_role->setEnabled(r >= 0);
        if (r >= 0 && r < static_cast<int>(m_references.size()))
            m_role->setCurrentIndex(m_role->findData(QString::fromLatin1(style::toString(m_references[static_cast<std::size_t>(r)].role))));
    });
    connect(m_role, &QComboBox::activated, this, [this] {
        const int r = m_refs->currentRow();
        if (r < 0 || r >= static_cast<int>(m_references.size())) return;
        m_references[static_cast<std::size_t>(r)].role =
            style::refRoleFromString(m_role->currentData().toString().toStdString()).value_or(style::RefRole::Style);
        refreshReferences();
        m_refs->setCurrentRow(r);
    });
    m_role->setEnabled(false);

    col->addWidget(buildStyleBox(w));

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
    auto* more = new QHBoxLayout;
    auto* bestiary = new QPushButton(tr("Bestiary Sheet…"), w);
    bestiary->setToolTip(tr("A grid of different creatures, cut into separate sprites"));
    auto* pack = new QPushButton(tr("Asset Pack…"), w);
    pack->setToolTip(tr("A themed set of matching assets, or one item in five tiers"));
    auto* pet = new QPushButton(tr("Pet to Pixel…"), w);
    pet->setToolTip(tr("A sprite of your pet from a photo"));
    more->addWidget(bestiary);
    more->addWidget(pack);
    more->addWidget(pet);
    col->addLayout(more);
    connect(bestiary, &QPushButton::clicked, this, &GeneratorPage::startBestiary);
    connect(pack, &QPushButton::clicked, this, &GeneratorPage::startAssetPack);
    connect(pet, &QPushButton::clicked, this, &GeneratorPage::startPetToPixel);
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

    // 4 Refine: the selected result, its check and the actions (two columns,
    // so the page fits a 1280 px screen with Windows fonts).
    auto* detail = new QFrame(w);
    detail->setProperty("card", true);
    auto* dl = new QHBoxLayout(detail);
    dl->setContentsMargins(14, 12, 14, 12);
    m_preview = new QLabel(detail);
    m_preview->setFixedSize(180, 180);
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
    act(tr("Animate…"), 1, 0, [this](Result& r) { Q_EMIT animateRequested(r.image, r.name); });
    act(tr("More Like This"), 1, 1, [this](Result& r) { generate({r.raw.isNull() ? r.image : r.raw}); });
    act(tr("Use as Reference"), 2, 0, [this](Result& r) { addReference(r.raw.isNull() ? r.image : r.raw, r.name, style::RefRole::Character); });
    act(tr("Add to Style"), 3, 0, [this](Result& r) {
        const std::string id = m_styleCombo->currentData().toString().toStdString();
        if (id.empty()) return void(Q_EMIT statusMessage(tr("Create a style first (Game style › New Style from References).")));
        auto p = m_styles.addReferences(id, {{r.image, style::RefRole::Style}});
        if (!p) return void(Q_EMIT statusMessage(qs(p.error().message)));
        refreshStyles();
        Q_EMIT statusMessage(tr("Added to the style “%1”.").arg(qs(p.value().name)));
    });
    act(tr("Character Pack…"), 3, 1, [this](Result& r) { startCharacterPack(r); })
        ->setToolTip(tr("Generates the other directions from this character (FLUX.2 klein) and animates every direction."));
    act(tr("Save PNG…"), 2, 1, [this](Result& r) {
        // The name comes from the prompt: characters Windows refuses in a file
        // name (a ":" on the test PC) become "-".
        QString safe = r.name;
        safe.replace(QRegularExpression(QStringLiteral(R"([<>:"/\\|?*\x00-\x1f])")), QStringLiteral("-"));
        safe = safe.trimmed();
        if (safe.isEmpty()) safe = QStringLiteral("asset");
        const QString f = QFileDialog::getSaveFileName(this, tr("Save PNG"), QDir::home().filePath(safe + QStringLiteral(".png")), tr("PNG (*.png)"));
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

void GeneratorPage::addReference(const Image& img, const QString& name, style::RefRole role) {
    if (img.isNull()) return;
    if (m_references.size() >= 4) {
        Q_EMIT statusMessage(tr("Up to four references; remove one first."));
        return;
    }
    m_references.push_back({img, name, role});
    refreshReferences();
    m_refs->setCurrentRow(static_cast<int>(m_references.size()) - 1);
}

void GeneratorPage::refreshReferences() {
    m_refs->clear();
    for (const Reference& r : m_references) {
        auto* it = new QListWidgetItem(QIcon(thumb(r.image, 64)), tr(style::roleLabel(r.role)));
        it->setToolTip(tr("%1 — %2").arg(r.name, tr(style::roleLabel(r.role))));
        m_refs->addItem(it);
    }
}

QWidget* GeneratorPage::buildStyleBox(QWidget* parent) {
    auto* box = new QFrame(parent);
    box->setProperty("card", true);
    auto* lay = new QVBoxLayout(box);
    lay->setContentsMargins(12, 10, 12, 12);
    auto* head = new QLabel(tr("Game style"), box);
    head->setProperty("role", "h2");
    lay->addWidget(head);
    auto* row = new QHBoxLayout;
    m_styleCombo = new QComboBox(box);
    m_styleCombo->setAccessibleName(tr("Style profile"));
    row->addWidget(m_styleCombo, 1);
    m_styleStrength = new QComboBox(box);
    m_styleStrength->setAccessibleName(tr("Style strength"));
    m_styleStrength->addItem(tr("Light"), 1);
    m_styleStrength->addItem(tr("Medium"), 2);
    m_styleStrength->addItem(tr("Strong"), 3);
    m_styleStrength->setToolTip(tr("Light: the style's words and palette check. Medium: also its reference images. "
                                   "Strong: also its exact palette."));
    row->addWidget(m_styleStrength);
    lay->addLayout(row);
    m_lockStyle = new QCheckBox(tr("Lock Game Style (apply to every generation)"), box);
    lay->addWidget(m_lockStyle);
    m_styleInfo = new QLabel(box);
    m_styleInfo->setProperty("role", "muted");
    m_styleInfo->setWordWrap(true);
    lay->addWidget(m_styleInfo);
    auto* buttons = new QHBoxLayout;
    auto* fromRefs = new QPushButton(tr("New Style from References…"), box);
    auto* del = new QPushButton(tr("Delete Style"), box);
    buttons->addWidget(fromRefs);
    buttons->addWidget(del);
    buttons->addStretch();
    lay->addLayout(buttons);

    connect(fromRefs, &QPushButton::clicked, this, [this] {
        std::vector<std::pair<Image, style::RefRole>> refs;
        for (const Reference& r : m_references) refs.push_back({r.image, r.role});
        if (refs.empty()) return void(QMessageBox::information(this, tr("New Style"), tr("Add one or more reference images first.")));
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("New Style"), tr("Name of the style:"), QLineEdit::Normal, tr("My game"), &ok);
        if (!ok || name.trimmed().isEmpty()) return;
        auto p = m_styles.create(name.trimmed().toStdString(), refs);
        if (!p) return void(QMessageBox::warning(this, tr("New Style"), qs(p.error().message)));
        m_ctx.setSetting("style.locked", p.value().id);
        refreshStyles();
    });
    connect(del, &QPushButton::clicked, this, [this] {
        const QString id = m_styleCombo->currentData().toString();
        if (id.isEmpty()) return;
        if (QMessageBox::question(this, tr("Delete Style"), tr("Delete the style “%1” and its copied reference images?").arg(m_styleCombo->currentText())) !=
            QMessageBox::Yes)
            return;
        if (auto st = m_styles.remove(id.toStdString()); !st) QMessageBox::warning(this, tr("Delete Style"), qs(st.error().message));
        m_ctx.setSetting("style.locked", std::string());
        refreshStyles();
    });
    auto persist = [this] {
        const QString id = m_lockStyle->isChecked() ? m_styleCombo->currentData().toString() : QString();
        m_ctx.setSetting("style.locked", id.toStdString());
        // The strength is the profile's own setting.
        if (auto p = m_styles.load(m_styleCombo->currentData().toString().toStdString())) {
            style::StyleProfile prof = std::move(p).value();
            prof.strength = m_styleStrength->currentData().toInt();
            (void)m_styles.save(prof);
        }
        refreshStyles();
    };
    connect(m_lockStyle, &QCheckBox::toggled, this, persist);
    connect(m_styleStrength, &QComboBox::activated, this, persist);
    connect(m_styleCombo, &QComboBox::activated, this, persist);
    return box;
}

void GeneratorPage::refreshStyles() {
    const std::string locked = m_ctx.settings().get<std::string>("style.locked");
    const QString current = m_styleCombo->currentData().toString();
    m_styleCombo->blockSignals(true);
    m_lockStyle->blockSignals(true);
    m_styleCombo->clear();
    const auto list = m_styles.list();
    for (const auto& p : list) m_styleCombo->addItem(qs(p.name), qs(p.id));
    if (list.empty()) m_styleCombo->addItem(tr("No styles yet"), QString());
    int idx = m_styleCombo->findData(locked.empty() ? current : qs(locked));
    m_styleCombo->setCurrentIndex(std::max(0, idx));
    m_lockStyle->setChecked(!locked.empty() && idx >= 0);
    m_lockStyle->setEnabled(!list.empty());
    m_styleStrength->setEnabled(!list.empty());
    m_styleCombo->blockSignals(false);
    m_lockStyle->blockSignals(false);
    if (auto p = m_styles.load(m_styleCombo->currentData().toString().toStdString())) {
        m_styleStrength->setCurrentIndex(std::max(0, m_styleStrength->findData(p.value().strength)));
        m_styleInfo->setText(tr("%1 reference(s). Measured: %2.")
                                 .arg(p.value().references.size())
                                 .arg(qs(style::describe(p.value().features))));
    } else {
        m_styleInfo->setText(tr("Make a style from reference images to keep new assets in your game's look."));
    }
}

std::optional<style::StyleProfile> GeneratorPage::lockedStyle() const {
    const std::string id = m_ctx.settings().get<std::string>("style.locked");
    if (id.empty()) return std::nullopt;
    auto p = m_styles.load(id);
    return p ? std::optional<style::StyleProfile>(std::move(p).value()) : std::nullopt;
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
    const std::string id = submitJob(extraRefs, JobOptions());
    if (id.empty()) return;
    m_job = id;
    m_generate->setEnabled(false);
    m_cancel->setEnabled(true);
    m_progress->setValue(0);
    m_progress->setVisible(true);
    m_status->setText(tr("Queued…"));
}

std::string GeneratorPage::submitJob(const std::vector<Image>& extraRefs, const JobOptions& o) {
    gen::Spec s = spec();
    if (o.description) s.description = *o.description;
    if (o.type) {
        s.type = *o.type;
        s.transparent = m_transparent->isChecked() && gen::isSingleSubject(s.type);
    }
    s.sheetColumns = o.sheetColumns;
    s.sheetRows = o.sheetRows;
    const bool sheet = o.sheetColumns > 0 && o.sheetRows > 0;
    if (s.description.empty()) {
        m_prompt->setFocus();
        Q_EMIT statusMessage(tr("Describe the asset first."));
        return {};
    }
    // A view of a character pack goes first and as an instruction: tried on
    // the test PC with the words appended after the description, klein
    // returned the reference's side view unchanged for the front view.
    if (!o.extraWords.empty())
        s.description = extraRefs.empty() ? s.description + ", " + o.extraWords
                                          : "Turn the character around to a " + o.extraWords +
                                                ", showing that side of the body. The character: " + s.description;
    const auto size = o.size;
    const auto seed = o.seed;
    // Choose the backend: klein when installed (and wanted), else SDXL.
    const QString choice = m_model->currentData().toString();
    const ModelEntry* klein = installed(m_ai, "flux2-klein-4b");
    const ModelEntry* sdxl = installed(m_ai, "sdxl-base-1.0");
    gen::Backend backend;
    if (choice == QLatin1String("klein") || (choice == QLatin1String("auto") && klein)) {
        if (!klein) { Q_EMIT showModelsRequested(); return {}; }
        backend = gen::Backend::Klein;
    } else {
        if (!sdxl) { Q_EMIT showModelsRequested(); return {}; }
        backend = gen::Backend::Sdxl;
    }
    // References: images sent to the model (with their roles) and palette-only references.
    gen::Spec sp = s;
    std::vector<Image> refImages;
    std::vector<Image> paletteSources;
    auto take = [&](const Image& img, style::RefRole role) {
        if (role == style::RefRole::Palette) { paletteSources.push_back(img); return; }
        if (refImages.size() >= 4) return;
        refImages.push_back(img);
        sp.referenceRoles.push_back(style::toString(role));
    };
    for (const Image& e : extraRefs) take(e, style::RefRole::Character);
    for (const Reference& r : m_references) take(r.image, r.role);
    const std::optional<style::StyleProfile> locked = lockedStyle();
    if (locked) {
        sp.styleNotes = style::describe(locked->features);
        if (!locked->notes.empty()) sp.styleNotes += ", " + locked->notes;
        if (locked->strength >= 2)
            for (const auto& ref : locked->references)
                if (ref.role != style::RefRole::Character && ref.role != style::RefRole::Creature)
                    if (auto img = m_styles.referenceImage(*locked, ref)) take(img.value(), ref.role);
    }
    if (!refImages.empty() && backend == gen::Backend::Sdxl)
        Q_EMIT statusMessage(tr("Reference images need FLUX.2 klein; SDXL uses the description and the style's words only."));
    std::vector<const Image*> refs;
    for (const Image& r : refImages) refs.push_back(&r);

    const bool pixel = gen::isPixelStyle(s.style);
    const ModelEntry* lora = pixel ? installed(m_ai, backend == gen::Backend::Klein ? "pixel-art-klein-lora" : "pixel-art-xl") : nullptr;
    if (backend == gen::Backend::Sdxl) sp.referenceRoles.clear();
    const gen::Prompt prompt = gen::composePrompt(sp, backend, lora != nullptr);
    const auto preset = gen::presetFromKey(m_preset->currentData().toString().toStdString()).value_or(gen::QualityPreset::Balanced);
    const auto [outW, outH] = size.value_or(outputSize());
    auto [gw, gh] = pixel ? gen::generationSize(outW, outH, preset) : std::pair{outW / 16 * 16, outH / 16 * 16};
    const auto& info = gen::presetInfo(preset);
    const ModelEntry* model = backend == gen::Backend::Klein ? klein : sdxl;

    json p = {{"model_dir", utf8(m_ai.store()->entryPath(*model))},
              {"prompt", prompt.positive},
              {"width", gw},
              {"height", gh},
              {"steps", backend == gen::Backend::Klein ? info.kleinSteps : info.sdxlSteps},
              {"count", o.count > 0 ? o.count : m_count->value()},
              {"seed", seed.value_or(m_seed->value())},
              {"model_id", model->id}};
    if (backend == gen::Backend::Sdxl) {
        p["negative_prompt"] = prompt.negative;
        p["guidance"] = 6.0;
        if (gen::wantsTileable(s.type)) p["tileable"] = "xy";
    }
    if (lora) p["lora"] = {{"path", utf8(m_ai.store()->entryPath(*lora))}, {"scale", lora->loraScale}};
    if (s.transparent) {
        const ModelEntry* bg = installed(m_ai, "birefnet-general");
        if (!bg) bg = installed(m_ai, "birefnet-lite");
        if (bg) p["cutout"] = {{"model", utf8(m_ai.store()->entryPath(*bg))}, {"threshold", 0.5}};
        else Q_EMIT statusMessage(tr("Install a BiRefNet model for transparent backgrounds (Models…)."));
    }
    if (pixel) {
        p["pixel"] = {{"width", outW}, {"height", outH}, {"crop", gen::isSingleSubject(s.type) && !sheet}};
        const int pal = m_palette->currentData().toInt();
        // A palette reference, or a strongly locked style, fixes the colours.
        Palette fixed;
        if (!paletteSources.empty()) {
            Image strip(static_cast<int>(paletteSources.size()) * 64, 64);
            for (std::size_t i = 0; i < paletteSources.size(); ++i) {
                const Palette part = extractPalette(paletteSources[i], 64);
                for (std::size_t c = 0; c < part.size(); ++c)
                    strip.setPixel(static_cast<int>(i * 64 + c % 64), static_cast<int>(c / 64), part.at(c));
            }
            fixed = extractPalette(strip, 32);
        } else if (locked && locked->strength >= 3) {
            fixed = locked->features.palette;
        }
        if (fixed.size() > 0) {
            json colours = json::array();
            for (std::size_t i = 0; i < fixed.size(); ++i) colours.push_back(fixed.at(i).toHex(false));
            p["pixel"]["palette"] = colours;
        } else if (pal < 0 && m_view && m_view->document().palette().size() > 0) {
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
            return {};
        }
    p.erase("model_id");
    auto id = m_ai.submit(task, std::move(p), tr("Generate: %1").arg(QString::fromStdString(s.description).left(40)), images);
    if (!id) {
        QMessageBox::information(this, tr("AI Studio"), qs(id.error().message));
        if (!m_ai.runtimeInstalled()) Q_EMIT showSetupRequested();
        return {};
    }
    m_jobPixel = pixel;
    return id.value();
}

void GeneratorPage::onJobChanged(const QString& qid) {
    if (qid.toStdString() != m_job) return;
    const Job* j = m_ai.job(m_job);
    if (!j) return;
    if (j->progress >= 0) m_progress->setValue(static_cast<int>(j->progress * 1000));
    if (!j->message.empty()) m_status->setText(qs(j->message));
}

void GeneratorPage::onJobFinished(const QString& qid) {
    if (m_pack && m_pack->jobs.count(qid.toStdString())) return onPackJobFinished(qid.toStdString());
    if (m_batch && m_batch->jobs.count(qid.toStdString())) return onBatchJobFinished(qid.toStdString());
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
        addResult(std::move(r));
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

namespace {
struct DirectionInfo { const char* key; const char* words; const char* mirrorOf; };
// The base result is the side view facing right ("e"). Left-facing views are mirrors.
const std::vector<DirectionInfo> kDirections = {
    {"e", "", nullptr},
    {"se", "three-quarter front view, turned towards the viewer and to the right", nullptr},
    {"s", "front view, facing the viewer", nullptr},
    {"sw", "", "se"},
    {"w", "", "e"},
    {"nw", "", "ne"},
    {"n", "back view, facing away from the viewer", nullptr},
    {"ne", "three-quarter back view, turned away from the viewer and to the right", nullptr},
};
Image mirrored(const Image& img) {
    Image out(img.width(), img.height());
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) out.setPixel(img.width() - 1 - x, y, img.pixel(x, y));
    return out;
}
} // namespace

void GeneratorPage::startCharacterPack(const Result& base) {
    if (m_pack) return void(Q_EMIT statusMessage(tr("A character pack is already being made.")));
    if (!installed(m_ai, "flux2-klein-4b") || m_model->currentData().toString() == QLatin1String("sdxl")) {
        QMessageBox::information(this, tr("Character Pack"),
                                 tr("Other directions are drawn from this character as a reference, which needs FLUX.2 [klein] "
                                    "(Models…). For a side view only, use Animate…"));
        return;
    }
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Character Pack"));
    auto* form = new QFormLayout(&dlg);
    auto* dirs = new QComboBox(&dlg);
    dirs->addItem(tr("2 directions (right, and left mirrored)"), 2);
    dirs->addItem(tr("4 directions (adds front and back)"), 4);
    dirs->addItem(tr("8 directions (adds the diagonals)"), 8);
    dirs->setCurrentIndex(1);
    form->addRow(tr("&Directions:"), dirs);
    auto* set = new QComboBox(&dlg);
    set->addItem(tr("Basic: idle, walk, run, jump, attack, hurt, death"), 0);
    set->addItem(tr("Locomotion: idle, walk, run, sprint, jump, fall, land"), 1);
    set->addItem(tr("Full: all 22 actions"), 2);
    form->addRow(tr("&Actions:"), set);
    auto* note = new QLabel(tr("New views are generated with this result as the character reference (FLUX.2 klein, same seed); "
                               "then every view is animated without AI. Check the views before using them in a game."),
                            &dlg);
    note->setWordWrap(true);
    note->setProperty("role", "muted");
    form->addRow(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);
    if (dlg.exec() != QDialog::Accepted) return;

    using A = ::acestudio::motion::Action;
    Pack pack;
    pack.name = base.name;
    switch (set->currentData().toInt()) {
    case 0: pack.actions = {A::Idle, A::Walk, A::Run, A::Jump, A::Attack1, A::Hurt, A::Death}; break;
    case 1: pack.actions = {A::Idle, A::Walk, A::Run, A::Sprint, A::Jump, A::Fall, A::Land}; break;
    default:
        for (const auto& info : ::acestudio::motion::actions())
            if (info.action != A::Custom) pack.actions.push_back(info.action);
    }
    const int count = dirs->currentData().toInt();
    pack.directions = count == 2 ? std::vector<std::string>{"e", "w"}
                    : count == 4 ? std::vector<std::string>{"e", "s", "w", "n"}
                                 : std::vector<std::string>{"e", "se", "s", "sw", "w", "nw", "n", "ne"};
    pack.views["e"] = base.image;
    const Image& ref = base.raw.isNull() ? base.image : base.raw;
    m_pack = std::move(pack);
    for (const std::string& d : m_pack->directions)
        for (const DirectionInfo& info : kDirections)
            if (d == info.key && info.mirrorOf == nullptr && *info.words) {
                JobOptions jo;
                jo.extraWords = info.words;
                jo.seed = base.seed;
                jo.size = std::pair{base.image.width(), base.image.height()};
                jo.count = 1;
                const std::string id = submitJob({ref}, jo);
                if (id.empty()) { m_pack.reset(); return; }
                m_pack->jobs[id] = d;
            }
    if (m_pack->jobs.empty()) return finishPack();
    m_status->setText(tr("Character pack: generating %n view(s)…", nullptr, static_cast<int>(m_pack->jobs.size())));
}

void GeneratorPage::onPackJobFinished(const std::string& id) {
    const std::string dir = m_pack->jobs[id];
    m_pack->jobs.erase(id);
    const Job* j = m_ai.job(id);
    if (j && j->state == JobState::Succeeded) {
        if (auto o = j->outputs.find("image0"); o != j->outputs.end())
            if (auto img = readPng(o->second)) m_pack->views[dir] = std::move(img).value();
    } else {
        m_pack->problems << tr("The %1 view failed: %2").arg(qs(dir), j ? qs(j->error).section(QLatin1Char('\n'), 0, 0) : tr("job lost"));
    }
    if (m_pack->jobs.empty()) finishPack();
    else m_status->setText(tr("Character pack: %n view(s) to go…", nullptr, static_cast<int>(m_pack->jobs.size())));
}

void GeneratorPage::finishPack() {
    Pack pack = std::move(*m_pack);
    m_pack.reset();
    // Mirrored views.
    for (const DirectionInfo& info : kDirections)
        if (info.mirrorOf && pack.views.count(info.mirrorOf)) pack.views[info.key] = mirrored(pack.views[info.mirrorOf]);
    // Animate every view; all frames share one size (feet on one line, centred).
    struct Out { std::string dir; std::vector<::acestudio::motion::Clip> clips; };
    std::vector<Out> outs;
    int W = 0, H = 0;
    for (const std::string& d : pack.directions) {
        auto it = pack.views.find(d);
        if (it == pack.views.end()) continue;
        ::acestudio::motion::Options o;
        // Front and back views walk with two legs side by side; the source decides for side views.
        auto clips = ::acestudio::motion::animatePack(it->second, pack.actions, o);
        if (!clips) { pack.problems << tr("%1: %2").arg(qs(d), qs(clips.error().message)); continue; }
        for (const auto& c : clips.value()) {
            W = std::max(W, c.frames[0].width());
            H = std::max(H, c.frames[0].height());
            for (const std::string& issue : ::acestudio::motion::checkClip(c).issues) pack.problems << qs(c.name + "_" + d + ": " + issue);
        }
        outs.push_back({d, std::move(clips).value()});
    }
    if (outs.empty()) {
        m_status->setText(tr("Character pack failed. %1").arg(pack.problems.join(QStringLiteral(" "))));
        return;
    }
    std::vector<Image> frames;
    std::vector<int> durations;
    std::vector<AnimationTag> tags;
    for (const Out& o : outs)
        for (const auto& c : o.clips) {
            AnimationTag t;
            t.name = c.name + "_" + o.dir;
            t.fromFrame = static_cast<int>(frames.size());
            t.repeat = c.loops ? 0 : 1;
            for (std::size_t i = 0; i < c.frames.size(); ++i) {
                Image f(W, H);
                f.blit(c.frames[i], (W - c.frames[i].width()) / 2, H - c.frames[i].height());
                frames.push_back(std::move(f));
                durations.push_back(c.durations[i]);
            }
            t.toFrame = static_cast<int>(frames.size()) - 1;
            tags.push_back(std::move(t));
        }
    m_status->setText(tr("Character pack: %1 animations in %2 directions.%3")
                          .arg(tags.size())
                          .arg(outs.size())
                          .arg(pack.problems.isEmpty() ? QString() : QStringLiteral(" ") + pack.problems.join(QStringLiteral("; "))));
    Q_EMIT openAnimation(frames, durations, tags, pack.name);
}

// ------------------------------------------------------------------ batches

namespace {
struct PackItem { const char* words; gen::AssetType type; };
struct PackTemplate { const char* label; std::vector<PackItem> items; };
const std::vector<PackTemplate>& packTemplates() {
    using T = gen::AssetType;
    static const std::vector<PackTemplate> t = {
        {QT_TRANSLATE_NOOP("GeneratorPage", "Dungeon"),
         {{"stone dungeon door", T::Prop}, {"wooden treasure chest", T::Prop}, {"burning wall torch", T::Prop}, {"skeleton warrior", T::Monster},
          {"giant rat", T::Monster}, {"red health potion", T::Item}, {"iron key", T::Item}, {"spike trap", T::Prop}}},
        {QT_TRANSLATE_NOOP("GeneratorPage", "Forest"),
         {{"oak tree", T::Prop}, {"berry bush", T::Prop}, {"mossy rock", T::Prop}, {"wooden fence section", T::Prop},
          {"cluster of mushrooms", T::Prop}, {"grey wolf", T::Animal}, {"wild boar", T::Animal}, {"wooden signpost", T::Prop}}},
        {QT_TRANSLATE_NOOP("GeneratorPage", "Sci-fi"),
         {{"laser rifle", T::Weapon}, {"metal supply crate", T::Prop}, {"security robot", T::Monster}, {"computer terminal", T::Furniture},
          {"glowing energy cell", T::Item}, {"sliding metal door", T::Prop}, {"alien plant", T::Prop}, {"medkit", T::Item}}},
        {QT_TRANSLATE_NOOP("GeneratorPage", "Town"),
         {{"market stall", T::Building}, {"stone well", T::Prop}, {"street lamp", T::Prop}, {"wooden barrel", T::Prop},
          {"blacksmith anvil", T::Prop}, {"villager", T::Npc}, {"town guard", T::Npc}, {"wooden cart", T::Prop}}},
    };
    return t;
}
const std::vector<const char*> kTiers = {"wooden", "iron", "steel", "golden", "legendary glowing"};
} // namespace

void GeneratorPage::startBestiary() {
    if (m_batch || m_pack) return void(Q_EMIT statusMessage(tr("A batch is already running.")));
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Bestiary Sheet"));
    auto* form = new QFormLayout(&dlg);
    auto* subject = new QLineEdit(m_prompt->toPlainText().trimmed().isEmpty() ? tr("fantasy monsters") : m_prompt->toPlainText().trimmed(), &dlg);
    form->addRow(tr("&Creatures:"), subject);
    auto* grid = new QComboBox(&dlg);
    for (int n = 3; n <= 7; ++n) grid->addItem(tr("%1 × %1 (%2 creatures)").arg(n).arg(n * n), n);
    grid->addItem(tr("Custom"), 0);
    form->addRow(tr("&Grid:"), grid);
    auto* cols = new QSpinBox(&dlg);
    cols->setRange(1, 10);
    cols->setValue(4);
    auto* rows = new QSpinBox(&dlg);
    rows->setRange(1, 10);
    rows->setValue(2);
    auto* custom = new QHBoxLayout;
    custom->addWidget(cols);
    custom->addWidget(new QLabel(QStringLiteral("×"), &dlg));
    custom->addWidget(rows);
    form->addRow(tr("C&ustom columns × rows:"), custom);
    cols->setAccessibleName(tr("Columns"));
    rows->setAccessibleName(tr("Rows"));
    auto syncCustom = [=] { cols->setEnabled(grid->currentData().toInt() == 0); rows->setEnabled(grid->currentData().toInt() == 0); };
    connect(grid, &QComboBox::currentIndexChanged, &dlg, syncCustom);
    syncCustom();
    auto* mode = new QComboBox(&dlg);
    mode->addItem(tr("One sheet, cut into creatures (fast)"), 0);
    mode->addItem(tr("One image per creature (staged, cleaner)"), 1);
    form->addRow(tr("&Method:"), mode);
    auto* idle = new QCheckBox(tr("Animate an &idle for every creature"), &dlg);
    form->addRow(idle);
    auto* note = new QLabel(tr("Both methods report their time per creature, so you can compare them on your GPU."), &dlg);
    note->setWordWrap(true);
    note->setProperty("role", "muted");
    form->addRow(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);
    if (dlg.exec() != QDialog::Accepted || subject->text().trimmed().isEmpty()) return;

    const int n = grid->currentData().toInt();
    const int c = n > 0 ? n : cols->value(), r = n > 0 ? n : rows->value();
    const std::string what = subject->text().trimmed().toStdString();
    const auto [outW, outH] = outputSize();
    Batch b;
    b.title = tr("Bestiary");
    b.animateIdle = idle->isChecked();
    b.startedMs = QDateTime::currentMSecsSinceEpoch();
    m_batch = std::move(b);
    if (mode->currentData().toInt() == 0) {
        // One image holds at most 3 x 3 distinct, separate creatures: on the
        // test PC a 7 x 7 request came back as about a dozen alike beasts
        // across the cell lines. Larger grids are made as several sheets.
        const int sc = std::min(c, 3), sr = std::min(r, 3);
        const int sheets = (c * r + sc * sr - 1) / (sc * sr);
        m_batch->sheetColumns = sc;
        m_batch->sheetRows = sr;
        m_batch->expected = c * r;
        for (int k = 0; k < sheets; ++k) {
            JobOptions o;
            const std::string kinds = gen::bestiaryKinds(what, k, sc * sr);
            o.description = kinds.empty() ? what : what + " (" + kinds + ")";
            o.type = gen::AssetType::Monster;
            o.sheetColumns = sc;
            o.sheetRows = sr;
            o.size = std::pair{outW * sc, outH * sr};
            o.count = 1;
            if (m_seed->value() >= 0) o.seed = m_seed->value() + k;
            const std::string id = submitJob({}, o);
            if (id.empty()) break;
            m_batch->jobs[id] = tr("sheet %1").arg(k + 1);
        }
        if (m_batch->jobs.empty()) { m_batch.reset(); return; }
    } else {
        // Different creatures: one seed each, the same words.
        for (int i = 0; i < c * r; ++i) {
            JobOptions o;
            o.description = "one of many " + what + ", creature number " + std::to_string(i + 1) + " of a varied bestiary";
            o.type = gen::AssetType::Monster;
            o.seed = m_seed->value() >= 0 ? m_seed->value() + i : -1;
            o.count = 1;
            const std::string id = submitJob({}, o);
            if (id.empty()) break;
            m_batch->jobs[id] = tr("creature %1").arg(i + 1);
        }
        if (m_batch->jobs.empty()) { m_batch.reset(); return; }
    }
    m_status->setText(tr("%1: %n generation(s) queued…", nullptr, static_cast<int>(m_batch->jobs.size())).arg(m_batch->title));
}

void GeneratorPage::startAssetPack() {
    if (m_batch || m_pack) return void(Q_EMIT statusMessage(tr("A batch is already running.")));
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Asset Pack"));
    auto* form = new QFormLayout(&dlg);
    auto* kind = new QComboBox(&dlg);
    for (std::size_t i = 0; i < packTemplates().size(); ++i)
        kind->addItem(tr("%1 pack (%2 assets)").arg(tr(packTemplates()[i].label)).arg(packTemplates()[i].items.size()), static_cast<int>(i));
    kind->addItem(tr("Item family: the prompt in five tiers"), -1);
    kind->addItem(tr("My own list (one asset per line)"), -2);
    form->addRow(tr("&Pack:"), kind);
    auto* theme = new QLineEdit(&dlg);
    theme->setPlaceholderText(tr("e.g. frozen, haunted, desert (optional)"));
    form->addRow(tr("&Theme:"), theme);
    auto* list = new QPlainTextEdit(&dlg);
    list->setPlaceholderText(tr("wooden shield\nhealing herb\ngoblin archer"));
    list->setMaximumHeight(110);
    list->setAccessibleName(tr("Asset list"));
    form->addRow(tr("&List:"), list);
    auto sync = [=] { list->setEnabled(kind->currentData().toInt() == -2); };
    connect(kind, &QComboBox::currentIndexChanged, &dlg, sync);
    sync();
    auto* folder = new QCheckBox(tr("&Save every asset to a folder, with a manifest"), &dlg);
    form->addRow(folder);
    auto* note = new QLabel(tr("Every asset uses the form's style, perspective, size and the locked game style, so the pack "
                               "matches. Lock a style first for the closest match."),
                            &dlg);
    note->setWordWrap(true);
    note->setProperty("role", "muted");
    form->addRow(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);
    if (dlg.exec() != QDialog::Accepted) return;

    std::vector<std::pair<std::string, std::optional<gen::AssetType>>> items;
    const int k = kind->currentData().toInt();
    const std::string t = theme->text().trimmed().toStdString();
    auto themed = [&](const std::string& w) { return t.empty() ? w : t + " " + w; };
    if (k >= 0) {
        for (const PackItem& it : packTemplates()[static_cast<std::size_t>(k)].items) items.push_back({themed(it.words), it.type});
    } else if (k == -1) {
        const std::string item = m_prompt->toPlainText().trimmed().toStdString();
        if (item.empty()) return void(QMessageBox::information(this, tr("Asset Pack"), tr("Describe the item in the prompt first (e.g. sword).")));
        for (const char* tier : kTiers) items.push_back({themed(std::string(tier) + " " + item), std::nullopt});
    } else {
        for (const QString& line : list->toPlainText().split(QLatin1Char('\n'), Qt::SkipEmptyParts))
            if (!line.trimmed().isEmpty()) items.push_back({themed(line.trimmed().toStdString()), std::nullopt});
    }
    if (items.empty()) return;
    QString dir;
    if (folder->isChecked()) {
        dir = QFileDialog::getExistingDirectory(this, tr("Save the Asset Pack in"));
        if (dir.isEmpty()) return;
    }
    Batch b;
    b.title = tr("Asset pack");
    b.saveDir = dir;
    b.startedMs = QDateTime::currentMSecsSinceEpoch();
    m_batch = std::move(b);
    for (const auto& [words, type] : items) {
        JobOptions o;
        o.description = words;
        o.type = type;
        o.count = 1;
        const std::string id = submitJob({}, o);
        if (id.empty()) break;
        m_batch->jobs[id] = qs(words);
    }
    if (m_batch->jobs.empty()) { m_batch.reset(); return; }
    m_status->setText(tr("%1: %n generation(s) queued…", nullptr, static_cast<int>(m_batch->jobs.size())).arg(m_batch->title));
}

void GeneratorPage::startPetToPixel() {
    if (!installed(m_ai, "flux2-klein-4b") || m_model->currentData().toString() == QLatin1String("sdxl"))
        return void(QMessageBox::information(this, tr("Pet to Pixel"),
                                             tr("Turning a photo into a sprite uses the photo as a reference, which needs FLUX.2 [klein] "
                                                "(Models…).")));
    const QString path = QFileDialog::getOpenFileName(this, tr("Photo of Your Pet"), QString(), tr("Photos (*.png *.jpg *.jpeg *.webp *.bmp)"));
    if (path.isEmpty()) return;
    QImage photo(path);
    if (photo.isNull()) return void(QMessageBox::warning(this, tr("Pet to Pixel"), tr("Could not read %1.").arg(QFileInfo(path).fileName())));
    // Keep the reference to a sensible size; the model works at about one megapixel.
    if (std::max(photo.width(), photo.height()) > 1024) photo = photo.scaled(1024, 1024, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    m_references.clear();
    addReference(fromQImage(photo.convertToFormat(QImage::Format_RGBA8888)), QFileInfo(path).completeBaseName(), style::RefRole::Creature);
    m_type->setCurrentIndex(std::max(0, m_type->findData(QStringLiteral("pet"))));
    if (m_prompt->toPlainText().trimmed().isEmpty())
        m_prompt->setPlainText(tr("my pet as a cute game sprite, same breed, fur colours and markings as the photo"));
    generate();
}

void GeneratorPage::onBatchJobFinished(const std::string& id) {
    const QString label = m_batch->jobs[id];
    m_batch->jobs.erase(id);
    const Job* j = m_ai.job(id);
    if (j && j->state == JobState::Succeeded) {
        if (auto o = j->outputs.find("image0"); o != j->outputs.end()) {
            if (auto img = readPng(o->second)) {
                if (m_batch->sheetColumns > 0) {
                    // A sheet: cut the creatures out of the full-size image when there is one (cleaner edges), else the reduced one.
                    const int perSheet = m_batch->sheetColumns * m_batch->sheetRows;
                    int found = 0;
                    for (const gen::GridCell& c : gen::extractGrid(img.value(), m_batch->sheetColumns, m_batch->sheetRows)) {
                        ++found;
                        if (static_cast<int>(m_batch->results.size()) >= m_batch->expected) break;   // the last sheet may give extras
                        m_batch->results.push_back({c.image, tr("creature %1").arg(m_batch->results.size() + 1)});
                    }
                    if (found < perSheet) m_batch->problems << tr("%1: %2 of %3 creatures found.").arg(label).arg(found).arg(perSheet);
                } else {
                    m_batch->results.push_back({std::move(img).value(), label});
                }
            }
        }
    } else {
        m_batch->problems << tr("%1 failed: %2").arg(label, j ? qs(j->error).section(QLatin1Char('\n'), 0, 0) : tr("job lost"));
    }
    if (m_batch->jobs.empty()) finishBatch();
    else m_status->setText(tr("%1: %n to go…", nullptr, static_cast<int>(m_batch->jobs.size())).arg(m_batch->title));
}

void GeneratorPage::finishBatch() {
    Batch b = std::move(*m_batch);
    m_batch.reset();
    const double seconds = static_cast<double>(QDateTime::currentMSecsSinceEpoch() - b.startedMs) / 1000.0;
    long long n = 0;
    for (auto& [img, name] : b.results) {
        Result r;
        r.image = img;
        r.name = name;
        r.seed = n++;
        addResult(std::move(r));
    }
    if (!b.results.empty()) m_results->setCurrentRow(0);
    QString text = tr("%1: %n asset(s) in %2 s (%3 s each).", nullptr, static_cast<int>(b.results.size()))
                       .arg(b.title)
                       .arg(seconds, 0, 'f', 1)
                       .arg(b.results.empty() ? 0.0 : seconds / static_cast<double>(b.results.size()), 0, 'f', 1);
    if (!b.saveDir.isEmpty() && !b.results.empty()) {
        json manifest{{"format", "acestudio.assetpack"}, {"version", 1}, {"assets", json::array()}};
        int written = 0;
        for (std::size_t i = 0; i < b.results.size(); ++i) {
            QString base = b.results[i].second;
            base.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]+")), QStringLiteral("_"));
            const QString file = QStringLiteral("%1_%2.png").arg(i + 1, 2, 10, QLatin1Char('0')).arg(base.left(40));
            const auto path = toPath(b.saveDir) / toPath(file);
            std::error_code ec;
            if (std::filesystem::exists(path, ec)) { b.problems << tr("%1 exists; not replaced.").arg(file); continue; }
            if (auto st = writePng(b.results[i].first, path); !st) { b.problems << qs(st.error().message); continue; }
            manifest["assets"].push_back({{"file", file.toStdString()}, {"name", b.results[i].second.toStdString()},
                                          {"width", b.results[i].first.width()}, {"height", b.results[i].first.height()}});
            ++written;
        }
        (void)writeTextFile(toPath(b.saveDir) / "assetpack.json", manifest.dump(2));
        text += tr(" Saved %1 to %2.").arg(written).arg(b.saveDir);
    }
    if (!b.problems.isEmpty()) text += QStringLiteral(" ") + b.problems.join(QStringLiteral(" "));
    m_status->setText(text);
    if (b.animateIdle && !b.results.empty()) {
        // One document: an idle per creature, all in one frame size.
        std::vector<std::pair<::acestudio::motion::Clip, QString>> clips;
        int W = 0, H = 0;
        for (const auto& [img, name] : b.results) {
            ::acestudio::motion::Options o;
            o.action = ::acestudio::motion::Action::Idle;
            auto c = ::acestudio::motion::animate(img, o);
            if (!c) continue;
            W = std::max(W, c.value().frames[0].width());
            H = std::max(H, c.value().frames[0].height());
            clips.push_back({std::move(c).value(), name});
        }
        std::vector<Image> frames;
        std::vector<int> durations;
        std::vector<AnimationTag> tags;
        for (const auto& [c, name] : clips) {
            AnimationTag t;
            t.name = name.toStdString();
            t.fromFrame = static_cast<int>(frames.size());
            for (std::size_t i = 0; i < c.frames.size(); ++i) {
                Image f(W, H);
                f.blit(c.frames[i], (W - c.frames[i].width()) / 2, H - c.frames[i].height());
                frames.push_back(std::move(f));
                durations.push_back(c.durations[i]);
            }
            t.toFrame = static_cast<int>(frames.size()) - 1;
            tags.push_back(std::move(t));
        }
        if (!frames.empty()) Q_EMIT openAnimation(frames, durations, tags, b.title);
    }
}

void GeneratorPage::addResult(Result r) {
    r.report = gen::checkAsset(r.image, checkOptions());
    if (const auto locked = lockedStyle(); locked && locked->features.sampleCount > 0) {
        r.styleMatch = style::styleSimilarity(locked->features, style::analyze(r.image));
        const double floor = locked->variation == 0 ? 0.8 : locked->variation == 1 ? 0.7 : 0.55;
        if (*r.styleMatch < floor)
            r.report.issues.push_back({gen::Severity::Warning, "style",
                                       "Off-style: " + std::to_string(static_cast<int>(*r.styleMatch * 100)) + "% match with “" +
                                           locked->name + "”."});
    }
    const int score = r.report.score();
    auto* it = new QListWidgetItem(QIcon(thumb(r.image, 140)),
                                   QStringLiteral("%1  %2").arg(score >= 90 ? QStringLiteral("●") : score >= 70 ? QStringLiteral("▲") : QStringLiteral("✖"))
                                       .arg(tr("seed %1").arg(r.seed)));
    it->setToolTip(tr("Check score %1/100").arg(score));
    // Newest first: row i of the grid is m_resultList[i].
    m_resultList.insert(m_resultList.begin(), std::move(r));
    m_results->insertItem(0, it);
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
    m_preview->setPixmap(thumb(r.image, 180));
    QString html = tr("<b>%1 × %2 px</b>, %3 colours, score %4/100<br>").arg(r.image.width()).arg(r.image.height())
                       .arg(r.report.colors).arg(r.report.score());
    if (r.styleMatch) html += tr("Style match: %1%<br>").arg(static_cast<int>(*r.styleMatch * 100));
    if (r.report.issues.empty()) html += tr("No problems found.");
    for (const gen::Issue& i : r.report.issues) html += severityMark(i.severity) + QStringLiteral(" ") + qs(i.message).toHtmlEscaped() + QStringLiteral("<br>");
    m_report->setText(html);
}

} // namespace acestudio::ui
