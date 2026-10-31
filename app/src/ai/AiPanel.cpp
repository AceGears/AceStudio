#include "AiPanel.h"

#include <array>

#include "AiDialogs.h"
#include "AiService.h"
#include "AppContext.h"
#include "DocumentView.h"
#include "QtBridge.h"

#include "acestudio/core/PngCodec.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace acestudio::ui {

using namespace acestudio::ai;
using nlohmann::json;

namespace {
QString utf8Path(const std::filesystem::path& p) { return fromPath(p); }
std::string toUtf8(const std::filesystem::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}
// SDXL works best near 1024 px; pick a generation size that is a multiple
// of the target (for clean nearest-neighbour downsampling) and of 8.
std::pair<int, int> generationSizeFor(int w, int h) {
    const int longest = std::max(w, h);
    int factor = std::max(1, 1024 / longest);
    auto fit = [&](int v) { return std::max(64, (v * factor) / 8 * 8); };
    return {std::min(fit(w), 1536), std::min(fit(h), 1536)};
}
} // namespace

AiPanel::AiPanel(AiService& ai, AppContext& ctx, QWidget* parent) : QWidget(parent), m_ai(ai), m_ctx(ctx) {
    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_setup = new QPushButton(tr("Set Up…"), this);
    auto* models = new QPushButton(tr("Models…"), this);
    m_restart = new QPushButton(tr("Restart"), this);
    m_restart->setToolTip(tr("Restart the AI worker process"));
    connect(m_setup, &QPushButton::clicked, this, &AiPanel::showSetup);
    connect(models, &QPushButton::clicked, this, &AiPanel::showModels);
    connect(m_restart, &QPushButton::clicked, &m_ai, &AiService::restartWorker);
    auto* top = new QHBoxLayout;
    top->addWidget(m_setup);
    top->addWidget(models);
    top->addWidget(m_restart);
    top->addStretch();

    // Pages scroll, so the dock never forces the window taller than the screen.
    m_tabs = new QTabWidget(this);
    auto scrolled = [this](QWidget* page) {
        auto* area = new QScrollArea(m_tabs);
        area->setWidget(page);
        area->setWidgetResizable(true);
        area->setFrameShape(QFrame::NoFrame);
        area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        return area;
    };
    m_tabs->addTab(scrolled(buildGenerateTab()), tr("Generate"));
    m_tabs->addTab(scrolled(buildRemoveTab()), tr("Remove BG"));
    m_tabs->addTab(scrolled(buildUpscaleTab()), tr("Upscale"));
    m_tabs->setMinimumHeight(80);   // pages scroll; keep the dock short enough for small screens
    // The run buttons sit under the pages, outside the scroll areas, so they
    // stay visible on short screens; only the current tab's button shows.
    auto* runRow = new QHBoxLayout;
    runRow->addStretch();
    const std::array<QPushButton*, 3> runButtons{m_genButton, m_bgButton, m_upButton};
    for (QPushButton* b : runButtons) { b->setParent(this); runRow->addWidget(b); }
    auto showRun = [runButtons](int index) {
        for (int i = 0; i < 3; ++i) runButtons[static_cast<std::size_t>(i)]->setVisible(i == index);
    };
    connect(m_tabs, &QTabWidget::currentChanged, this, showRun);
    showRun(m_tabs->currentIndex());

    m_queue = new QTreeWidget(this);
    m_queue->setColumnCount(2);
    m_queue->setHeaderLabels({tr("Job"), tr("Progress")});
    m_queue->setRootIsDecorated(false);
    m_queue->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_queue->setMinimumHeight(40);
    m_queue->setMaximumHeight(180);
    m_queue->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_queue, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QTreeWidgetItem* item = m_queue->itemAt(pos);
        QMenu menu(this);
        if (item) {
            const std::string id = item->data(0, Qt::UserRole).toString().toStdString();
            const Job* j = m_ai.job(id);
            QAction* cancel = menu.addAction(tr("Cancel"), this, [this, id] { m_ai.cancel(id); });
            cancel->setEnabled(j && !isFinished(j->state));
            if (j && !j->error.empty())
                menu.addAction(tr("Show Error…"), this, [this, j] { QMessageBox::warning(this, tr("AI Job"), qs(j->error)); });
        }
        menu.addAction(tr("Clear Finished"), this, [this] {
            if (auto* sup = m_ai.supervisor()) sup->clearFinished(true);
            for (auto it = m_rows.begin(); it != m_rows.end();) {
                if (!m_ai.job(it->first)) { delete it->second; it = m_rows.erase(it); } else ++it;
            }
        });
        menu.exec(m_queue->viewport()->mapToGlobal(pos));
    });
    auto* cancelBtn = new QPushButton(tr("Cancel Job"), this);
    connect(cancelBtn, &QPushButton::clicked, this, [this] {
        if (QTreeWidgetItem* item = m_queue->currentItem()) m_ai.cancel(item->data(0, Qt::UserRole).toString().toStdString());
    });
    auto* queueHeader = new QHBoxLayout;
    queueHeader->addWidget(new QLabel(tr("Queue"), this));
    queueHeader->addStretch();
    queueHeader->addWidget(cancelBtn);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 4, 6, 6);
    layout->addWidget(m_status);
    layout->addLayout(top);
    layout->addWidget(m_tabs, 3);
    layout->addLayout(runRow);
    layout->addLayout(queueHeader);
    layout->addWidget(m_queue, 1);

    connect(&m_ai, &AiService::stateChanged, this, &AiPanel::refreshStatus);
    connect(&m_ai, &AiService::modelsChanged, this, &AiPanel::refreshModels);
    connect(&m_ai, &AiService::jobChanged, this, &AiPanel::refreshJob);
    connect(&m_ai, &AiService::jobFinished, this, &AiPanel::onJobFinished);
    connect(&m_ai, &AiService::message, this, &AiPanel::statusMessage);
    refreshStatus();
    refreshModels();
    setView(nullptr);
}

QWidget* AiPanel::buildGenerateTab() {
    auto* w = new QWidget(this);
    m_genModel = new QComboBox(w);
    m_genLora = new QCheckBox(tr("Pixel Art XL style"), w);
    m_genLoraStrength = new QSpinBox(w);
    m_genLoraStrength->setRange(0, 200);
    m_genLoraStrength->setSingleStep(10);
    m_genLoraStrength->setSuffix(QStringLiteral(" %"));
    m_genLoraStrength->setToolTip(tr("How strongly the Pixel Art XL style is applied (100 % = as trained)"));
    m_genLoraStrength->setValue(std::clamp(m_ctx.settings().get<int>("ai.generate.loraStrength"), 0, 200));
    m_genLora->setChecked(m_ctx.settings().get<bool>("ai.generate.lora"));
    connect(m_genLora, &QCheckBox::toggled, this, [this](bool on) {
        m_genLoraStrength->setEnabled(on && m_genLora->isEnabled());
        if (m_genLora->isEnabled()) (void)m_ctx.settings().set("ai.generate.lora", on);   // not when greyed out
    });
    connect(m_genLoraStrength, &QSpinBox::valueChanged, this, [this](int v) { (void)m_ctx.settings().set("ai.generate.loraStrength", v); });
    m_prompt = new QPlainTextEdit(w);
    m_prompt->setPlaceholderText(tr("Describe the asset, e.g. “knight with a red shield, side view, game sprite”"));
    m_prompt->setFixedHeight(64);
    m_prompt->setTabChangesFocus(true);   // Tab moves on to the next field instead of typing a tab
    m_negative = new QLineEdit(w);
    m_negative->setPlaceholderText(tr("Avoid (optional): blurry, text, watermark"));
    m_pixelMode = new QCheckBox(tr("Pixel art for this canvas"), w);
    m_pixelMode->setToolTip(tr("Generates near 1024 px, then downsamples to the canvas size with nearest neighbour and reduces colours"));
    m_pixelMode->setChecked(true);
    m_usePalette = new QCheckBox(tr("Use document palette"), w);
    m_usePalette->setChecked(true);
    m_cropSubject = new QCheckBox(tr("Frame the subject"), w);
    m_cropSubject->setToolTip(tr("Crop to the subject before reducing to the canvas size, so small subjects keep more "
                                 "detail. Works when the image has a plain background."));
    m_cropSubject->setChecked(m_ctx.settings().get<bool>("ai.generate.cropSubject"));
    connect(m_cropSubject, &QCheckBox::toggled, this, [this](bool on) { (void)m_ctx.settings().set("ai.generate.cropSubject", on); });
    m_width = new QSpinBox(w);
    m_height = new QSpinBox(w);
    for (QSpinBox* s : {m_width, m_height}) { s->setRange(64, 2048); s->setSingleStep(64); s->setValue(1024); }
    m_steps = new QSpinBox(w);
    m_steps->setRange(1, 150);
    m_steps->setValue(30);
    m_guidance = new QDoubleSpinBox(w);
    m_guidance->setRange(1.0, 20.0);
    m_guidance->setSingleStep(0.5);
    m_guidance->setValue(6.0);
    m_seed = new QSpinBox(w);
    m_seed->setRange(-1, 2147483647);
    m_seed->setValue(-1);
    m_seed->setSpecialValueText(tr("Random"));
    m_count = new QSpinBox(w);
    m_count->setRange(1, 4);
    m_img2img = new QCheckBox(tr("Start from current frame"), w);
    m_img2img->setToolTip(tr("Image-to-image: the visible frame is the starting point; Strength sets how much changes"));
    m_strength = new QDoubleSpinBox(w);
    m_strength->setRange(0.05, 1.0);
    m_strength->setSingleStep(0.05);
    m_strength->setValue(0.6);
    m_strength->setEnabled(false);
    connect(m_img2img, &QCheckBox::toggled, m_strength, &QWidget::setEnabled);
    auto sizeEnabled = [this] {
        m_width->setEnabled(!m_pixelMode->isChecked());
        m_height->setEnabled(!m_pixelMode->isChecked());
        m_usePalette->setEnabled(m_pixelMode->isChecked());
        m_cropSubject->setEnabled(m_pixelMode->isChecked());
    };
    connect(m_pixelMode, &QCheckBox::toggled, this, sizeEnabled);
    sizeEnabled();
    m_genButton = new QPushButton(tr("&Generate"), w);
    connect(m_genButton, &QPushButton::clicked, this, &AiPanel::runGenerate);

    auto* size = new QHBoxLayout;
    size->addWidget(m_width);
    size->addWidget(new QLabel(QStringLiteral("×"), w));
    size->addWidget(m_height);
    auto* f = new QFormLayout(w);
    f->addRow(tr("Model:"), m_genModel);
    auto* loraRow = new QHBoxLayout;
    loraRow->addWidget(m_genLora);
    loraRow->addWidget(m_genLoraStrength);
    loraRow->addStretch();
    f->addRow(QString(), loraRow);
    f->addRow(tr("Prompt:"), m_prompt);
    f->addRow(tr("Negative:"), m_negative);
    f->addRow(QString(), m_pixelMode);
    f->addRow(QString(), m_usePalette);
    f->addRow(QString(), m_cropSubject);
    f->addRow(tr("Size:"), size);
    f->addRow(tr("Steps:"), m_steps);
    f->addRow(tr("Guidance:"), m_guidance);
    f->addRow(tr("Seed:"), m_seed);
    f->addRow(tr("Images:"), m_count);
    f->addRow(QString(), m_img2img);
    f->addRow(tr("Strength:"), m_strength);
    m_genPreview = new QLabel(w);
    m_genPreview->setAlignment(Qt::AlignCenter);
    m_genPreview->setMinimumHeight(0);
    m_genPreview->setToolTip(tr("Live preview of the image being generated"));
    m_genPreview->hide();
    f->addRow(m_genPreview);
    return w;
}

QWidget* AiPanel::buildRemoveTab() {
    auto* w = new QWidget(this);
    m_bgModel = new QComboBox(w);
    m_bgHard = new QCheckBox(tr("Hard edges (pixel art)"), w);
    m_bgHard->setToolTip(tr("Every pixel ends fully opaque or fully transparent"));
    m_bgMerged = new QCheckBox(tr("All visible layers"), w);
    m_bgMerged->setToolTip(tr("Process the visible image instead of only the current layer"));
    m_bgButton = new QPushButton(tr("&Remove Background"), w);
    connect(m_bgButton, &QPushButton::clicked, this, &AiPanel::runRemoveBackground);
    auto* note = new QLabel(tr("The result is added as a new layer above the source; the original is untouched."), w);
    note->setWordWrap(true);
    auto* f = new QFormLayout(w);
    f->addRow(tr("Model:"), m_bgModel);
    f->addRow(QString(), m_bgHard);
    f->addRow(QString(), m_bgMerged);
    f->addRow(note);
    return w;
}

QWidget* AiPanel::buildUpscaleTab() {
    auto* w = new QWidget(this);
    m_upModel = new QComboBox(w);
    m_upButton = new QPushButton(tr("&Upscale ×4"), w);
    m_upButton->setToolTip(tr("Upscales into a new document"));
    connect(m_upButton, &QPushButton::clicked, this, &AiPanel::runUpscale);
    m_upAllFrames = new QCheckBox(tr("All frames (keeps the animation)"), w);
    m_upAllFrames->setToolTip(tr("Upscale every frame in one job; the result is a new animated document with the same durations"));
    auto* note = new QLabel(tr("For painted or HD art. Pixel art keeps crisp pixels with Image › Scale (nearest neighbour) instead."), w);
    note->setWordWrap(true);
    auto* f = new QFormLayout(w);
    f->addRow(tr("Model:"), m_upModel);
    f->addRow(QString(), m_upAllFrames);
    f->addRow(note);
    return w;
}

void AiPanel::setView(DocumentView* view) {
    m_view = view;
    const bool has = view != nullptr;
    for (QPushButton* b : {m_genButton, m_bgButton, m_upButton}) b->setEnabled(has || b == m_genButton);
    m_pixelMode->setEnabled(has);
    m_img2img->setEnabled(has);
    // Only disable: unticking here lost the pixel-art default for every
    // session that started on the welcome page (seen on Windows: a 64x64
    // canvas got a 1024x1024 result). runGenerate checks m_view anyway.
}

void AiPanel::focusTab(int index) { m_tabs->setCurrentIndex(index); }

void AiPanel::refreshStatus() {
    m_status->setText(m_ai.statusText());
    m_setup->setText(m_ai.runtimeInstalled() ? tr("Add Components…") : tr("Set Up…"));
    m_setup->setEnabled(m_ai.environmentOk() && !m_ai.settingUp());
    m_restart->setEnabled(m_ai.runtimeInstalled() && !m_ai.settingUp());
}

void AiPanel::fillModelCombo(QComboBox* combo, const std::string& task) {
    // Long model names must not widen the dock; the full name is in the tooltip.
    combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    combo->setMinimumContentsLength(14);
    const QString keep = combo->currentData().toString();
    combo->clear();
    if (m_ai.catalog() && m_ai.store())
        for (const ModelEntry* m : m_ai.catalog()->forTask(task))
            if (m->kind == "model" && m_ai.store()->state(*m) == InstallState::Installed)
                combo->addItem(qs(m->name), qs(m->id));
    if (combo->count() == 0) combo->addItem(tr("No model installed — open Models…"), QString());
    for (int i = 0; i < combo->count(); ++i) combo->setItemData(i, combo->itemText(i), Qt::ToolTipRole);
    const int idx = combo->findData(keep);
    if (idx >= 0) combo->setCurrentIndex(idx);
}

void AiPanel::refreshModels() {
    fillModelCombo(m_genModel, "image.generate");
    fillModelCombo(m_bgModel, "background.remove");
    fillModelCombo(m_upModel, "upscale.esrgan");
    const ModelEntry* lora = m_ai.catalog() ? m_ai.catalog()->find("pixel-art-xl") : nullptr;
    const bool loraOk = lora && m_ai.store() && m_ai.store()->state(*lora) == InstallState::Installed;
    {
        const QSignalBlocker block(m_genLora);   // greying out must not change the saved choice
        m_genLora->setEnabled(loraOk);
        m_genLora->setChecked(loraOk && m_ctx.settings().get<bool>("ai.generate.lora"));
    }
    m_genLoraStrength->setEnabled(loraOk && m_genLora->isChecked());
    m_genLora->setToolTip(loraOk ? QString() : tr("Download “Pixel Art XL” in Models… to enable"));
}

QString AiPanel::modelPath(QComboBox* combo) const {
    const QString id = combo->currentData().toString();
    if (id.isEmpty() || !m_ai.catalog()) return {};
    const ModelEntry* m = m_ai.catalog()->find(id.toStdString());
    return m ? utf8Path(m_ai.store()->entryPath(*m)) : QString();
}

void AiPanel::showSetup() {
    AiSetupDialog dlg(m_ai, m_ctx, this);
    if (dlg.exec() == QDialog::Accepted) m_ai.setUpRuntime(dlg.profiles());
    refreshStatus();
}

void AiPanel::showModels() {
    ModelsDialog dlg(m_ai, this);
    dlg.exec();
    refreshModels();
}

bool AiPanel::submit(const std::string& task, json params, const QString& label, Pending pending,
                     const std::vector<std::pair<std::string, const Image*>>& images) {
    // Check the installed packages can run the chosen model before queuing.
    if (params.contains("model_id") && m_ai.catalog()) {
        const ModelEntry* m = m_ai.catalog()->find(params["model_id"].get<std::string>());
        if (m && m_ai.state() == WorkerState::Ready)
            if (auto problem = runtimeProblem(*m, m_ai.capabilities())) {
                QMessageBox::information(this, tr("AI Studio"), qs(*problem) + tr("\n\nUse AI Studio › Add Components…"));
                return false;
            }
        params.erase("model_id");
    }
    auto id = m_ai.submit(task, std::move(params), label, images);
    if (!id) {
        QMessageBox::information(this, tr("AI Studio"), qs(id.error().message));
        return false;
    }
    m_pending[id.value()] = std::move(pending);
    return true;
}

void AiPanel::runGenerate() {
    const QString model = modelPath(m_genModel);
    if (model.isEmpty()) return showModels();
    const QString prompt = m_prompt->toPlainText().trimmed();
    if (prompt.isEmpty()) {
        m_prompt->setFocus();
        return void(Q_EMIT statusMessage(tr("Enter a prompt first.")));
    }
    json p = {{"model_dir", toUtf8(toPath(model))}, {"prompt", prompt.toStdString()},
              {"negative_prompt", m_negative->text().toStdString()}, {"steps", m_steps->value()},
              {"guidance", m_guidance->value()}, {"seed", m_seed->value()}, {"count", m_count->value()},
              {"model_id", m_genModel->currentData().toString().toStdString()}};
    Pending pend;
    pend.kind = Pending::Kind::Generate;
    pend.view = m_view;
    pend.name = prompt.left(32);
    const bool pixel = m_view && m_pixelMode->isChecked();
    if (pixel) {
        const Document& d = m_view->document();
        auto [gw, gh] = generationSizeFor(d.width(), d.height());
        p["width"] = gw;
        p["height"] = gh;
        p["pixel"] = {{"width", d.width()}, {"height", d.height()}, {"crop", m_cropSubject->isChecked()}};
        if (m_usePalette->isChecked() && d.palette().size() > 0) {
            json pal = json::array();
            for (std::size_t i = 0; i < d.palette().size(); ++i) pal.push_back(d.palette().at(i).toHex(false));
            p["pixel"]["palette"] = pal;
        } else {
            p["pixel"]["colors"] = 32;
        }
        pend.pixel = true;
        pend.layer = m_view->currentLayer();
        pend.frame = m_view->currentFrame();
    } else {
        p["width"] = m_width->value() / 8 * 8;
        p["height"] = m_height->value() / 8 * 8;
    }
    if (m_genLora->isChecked() && m_ai.catalog())
        if (const ModelEntry* l = m_ai.catalog()->find("pixel-art-xl"))
            p["lora"] = {{"path", toUtf8(m_ai.store()->entryPath(*l))}, {"scale", m_genLoraStrength->value() / 100.0}};
    if (m_ai.catalog() && m_ai.store())
        if (const ModelEntry* tae = m_ai.catalog()->find("taesdxl"); tae && m_ai.store()->state(*tae) == InstallState::Installed)
            p["preview"] = {{"dir", toUtf8(m_ai.store()->entryPath(*tae))}, {"every", 5}};
    std::optional<Image> init;
    if (m_img2img->isChecked() && m_view) {
        init = m_view->document().composite(m_view->currentFrame());
        p["strength"] = m_strength->value();
    }
    std::vector<std::pair<std::string, const Image*>> images;
    if (init) images.push_back({"init_image", &*init});
    submit("image.generate", std::move(p), tr("Generate: %1").arg(prompt.left(40)), std::move(pend), images);
}

void AiPanel::runRemoveBackground() {
    if (!m_view) return;
    const QString model = modelPath(m_bgModel);
    if (model.isEmpty()) return showModels();
    const Document& d = m_view->document();
    const bool merged = m_bgMerged->isChecked();
    const LayerId layer = m_view->currentLayer();
    const Layer* L = d.layer(layer);
    if (!merged && (!L || L->kind != LayerKind::Raster))
        return void(Q_EMIT statusMessage(tr("Select a raster layer (or use all visible layers).")));
    const Image src = merged ? d.composite(m_view->currentFrame()) : d.renderLayer(layer, m_view->currentFrame());
    if (src.isFullyTransparent()) return void(Q_EMIT statusMessage(tr("There is nothing to process on this frame.")));
    json p = {{"model", toUtf8(toPath(model))}, {"model_id", m_bgModel->currentData().toString().toStdString()}};
    if (m_bgHard->isChecked()) p["threshold"] = 0.5;
    if (m_ai.catalog())
        if (const ModelEntry* e = m_ai.catalog()->find(m_bgModel->currentData().toString().toStdString()); e && e->cpuMemoryMb > 0)
            p["cpu_memory_mb"] = e->cpuMemoryMb;
    Pending pend;
    pend.kind = Pending::Kind::RemoveBackground;
    pend.view = m_view;
    pend.layer = layer;
    pend.frame = m_view->currentFrame();
    pend.name = (merged ? tr("Merged") : qs(L->name)) + tr(" (no background)");
    submit("background.remove", std::move(p), tr("Remove background: %1").arg(pend.name), std::move(pend), {{"input", &src}});
}

void AiPanel::runUpscale() {
    if (!m_view) return;
    const QString model = modelPath(m_upModel);
    if (model.isEmpty()) return showModels();
    const Document& d = m_view->document();
    json p = {{"model", toUtf8(toPath(model))}, {"model_id", m_upModel->currentData().toString().toStdString()}};
    Pending pend;
    pend.kind = Pending::Kind::Upscale;
    pend.view = m_view;
    pend.name = m_view->displayName() + tr(" ×4");
    if (m_upAllFrames->isChecked() && d.frameCount() > 1) {
        std::vector<Image> frames;
        frames.reserve(static_cast<std::size_t>(d.frameCount()));
        for (int f = 0; f < d.frameCount(); ++f) {
            frames.push_back(d.composite(f));
            pend.durations.push_back(d.frames()[static_cast<std::size_t>(f)].durationMs);
        }
        std::vector<std::pair<std::string, const Image*>> inputs;
        for (std::size_t f = 0; f < frames.size(); ++f) inputs.emplace_back("input" + std::to_string(f), &frames[f]);
        p["frames"] = d.frameCount();
        submit("upscale.esrgan", std::move(p), tr("Upscale %n frame(s): %1", nullptr, d.frameCount()).arg(m_view->displayName()),
               std::move(pend), inputs);
        return;
    }
    const Image src = d.composite(m_view->currentFrame());
    submit("upscale.esrgan", std::move(p), tr("Upscale: %1").arg(m_view->displayName()), std::move(pend), {{"input", &src}});
}

namespace {
// " (on CUDA, 2.6 s)": where a finished job actually ran (after any fallback) and how long it took.
QString ranOn(const Job& j) {
    std::string dev = j.info.value("provider", std::string());
    if (dev.empty()) dev = j.info.value("device", std::string());
    static const std::pair<const char*, const char*> names[] = {
        {"CUDAExecutionProvider", "CUDA"}, {"DmlExecutionProvider", "DirectML"}, {"CPUExecutionProvider", "CPU"},
        {"cuda", "CUDA"}, {"cpu", "CPU"}, {"mps", "Metal"}};
    QString where = QString::fromStdString(dev);
    for (const auto& [k, v] : names)
        if (dev == k || dev.rfind(std::string(k) + ":", 0) == 0) where = QString::fromLatin1(v);
    QString out = where.isEmpty() ? QString() : AiPanel::tr("on %1").arg(where);
    if (j.started != Clock::time_point{} && j.finished > j.started) {
        const double secs = std::chrono::duration<double>(j.finished - j.started).count();
        const QString t = AiPanel::tr("%1 s").arg(secs, 0, 'f', secs < 10 ? 1 : 0);
        out = out.isEmpty() ? t : AiPanel::tr("%1, %2").arg(out, t);
    }
    return out.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(out);
}
} // namespace

void AiPanel::refreshJob(const QString& qid) {
    const std::string id = qid.toStdString();
    const Job* j = m_ai.job(id);
    if (!j) return;
    QTreeWidgetItem*& item = m_rows[id];
    if (!item) {
        item = new QTreeWidgetItem(m_queue);
        item->setData(0, Qt::UserRole, qid);
        auto* bar = new QProgressBar(m_queue);
        bar->setRange(0, 100);
        bar->setTextVisible(true);
        bar->setMaximumHeight(18);
        m_queue->setItemWidget(item, 1, bar);
        m_queue->scrollToItem(item);
    }
    item->setText(0, qs(j->label));
    QString tip = qs(j->label) + QStringLiteral("\n") + qs(toString(j->state));
    if (!j->message.empty()) tip += QStringLiteral(" — ") + qs(j->message);
    if (!j->error.empty()) tip += QStringLiteral("\n") + qs(j->error);
    item->setToolTip(0, tip);
    // Live preview of a running generation.
    if (j->task == "image.generate") {
        auto pit = m_previewSerial.find(id);
        if (j->state == JobState::Running && j->previewSerial > 0 && (pit == m_previewSerial.end() || pit->second != j->previewSerial)) {
            m_previewSerial[id] = j->previewSerial;
            QPixmap pm;
            if (pm.load(fromPath(j->preview))) {
                m_genPreview->setPixmap(pm.scaled(QSize(192, 192), Qt::KeepAspectRatio, Qt::SmoothTransformation));
                m_genPreview->show();
            }
        } else if (j->state != JobState::Running && j->state != JobState::Queued) {
            m_previewSerial.erase(id);
            m_genPreview->hide();
            m_genPreview->clear();
        }
    }
    auto* bar = qobject_cast<QProgressBar*>(m_queue->itemWidget(item, 1));
    if (!bar) return;
    bar->setToolTip(tip);
    switch (j->state) {
    case JobState::Queued: bar->setRange(0, 100); bar->setValue(0); bar->setFormat(tr("Queued")); break;
    case JobState::Running:
        if (j->progress < 0) { bar->setRange(0, 0); }
        else { bar->setRange(0, 100); bar->setValue(qRound(j->progress * 100)); }
        // The column is narrow: show only the percentage (or "Working" when
        // indeterminate) and keep the step message in the tooltip.
        bar->setFormat(j->progress < 0 ? tr("Working") : QStringLiteral("%p%"));
        break;
    case JobState::Succeeded: bar->setRange(0, 100); bar->setValue(100); bar->setFormat(tr("Done")); break;
    case JobState::Failed: bar->setRange(0, 100); bar->setValue(0); bar->setFormat(tr("Failed")); break;
    case JobState::Cancelled: bar->setRange(0, 100); bar->setValue(0); bar->setFormat(tr("Cancelled")); break;
    }
}

void AiPanel::onJobFinished(const QString& qid) {
    refreshJob(qid);
    const std::string id = qid.toStdString();
    const Job* j = m_ai.job(id);
    auto it = m_pending.find(id);
    if (!j) return;
    if (j->state == JobState::Failed) Q_EMIT statusMessage(tr("%1 failed: %2").arg(qs(j->label), qs(j->error).section(QLatin1Char('\n'), 0, 0)));
    if (j->task == "runtime.install" || j->task == "model.download") refreshModels();
    if (it == m_pending.end()) return;
    Pending pend = it->second;
    m_pending.erase(it);
    if (j->state != JobState::Succeeded) return;
    auto load = [&](const std::string& name) -> std::optional<Image> {
        auto o = j->outputs.find(name);
        if (o == j->outputs.end()) return std::nullopt;
        auto img = readPng(o->second);
        if (!img) { Q_EMIT statusMessage(qs(img.error().message)); return std::nullopt; }
        return std::move(img).value();
    };
    switch (pend.kind) {
    case Pending::Kind::Generate: {
        int n = 0;
        for (int i = 0; i < 8; ++i) {
            auto img = load("image" + std::to_string(i));
            if (!img) break;
            ++n;
            const QString name = tr("AI %1%2").arg(pend.name).arg(i ? QStringLiteral(" %1").arg(i + 1) : QString());
            if (pend.pixel && pend.view && img->width() == pend.view->document().width() && img->height() == pend.view->document().height())
                Q_EMIT resultAsLayer(pend.view, *img, name, pend.layer, pend.frame);
            else
                Q_EMIT resultAsDocument(*img, name);
        }
        const QString seed = QString::number(j->info.value("seed", 0LL));
        Q_EMIT statusMessage(tr("Generated %n image(s) with seed %1%2", nullptr, n).arg(seed, ranOn(*j)));
        break;
    }
    case Pending::Kind::RemoveBackground:
        Q_EMIT statusMessage(tr("Background removed%1").arg(ranOn(*j)));
        if (auto img = load("image")) {
            if (pend.view) Q_EMIT resultAsLayer(pend.view, *img, pend.name, pend.layer, pend.frame);
            else Q_EMIT resultAsDocument(*img, pend.name);
        }
        break;
    case Pending::Kind::Upscale:
        Q_EMIT statusMessage(tr("Upscaled%1").arg(ranOn(*j)));
        if (!pend.durations.empty()) {
            std::vector<Image> frames;
            for (std::size_t f = 0; f < pend.durations.size(); ++f) {
                auto img = load("image" + std::to_string(f));
                if (!img) break;
                frames.push_back(std::move(*img));
            }
            if (frames.size() == pend.durations.size()) Q_EMIT resultAsAnimation(frames, pend.durations, pend.name);
            else Q_EMIT statusMessage(tr("Upscale returned %1 of %2 frames").arg(frames.size()).arg(pend.durations.size()));
        } else if (auto img = load("image")) {
            Q_EMIT resultAsDocument(*img, pend.name);
        }
        break;
    case Pending::Kind::Other: break;
    }
}

} // namespace acestudio::ui
