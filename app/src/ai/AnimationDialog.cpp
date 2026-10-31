#include "AnimationDialog.h"

#include "../AppContext.h"
#include "../DocumentView.h"
#include "../QtBridge.h"
#include "AiService.h"
#include "CharacterDialog.h"
#include "PoseEditor.h"

#include "acestudio/core/Editor.h"
#include "acestudio/core/FileUtil.h"
#include "acestudio/core/Log.h"
#include "acestudio/core/PngCodec.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <random>

namespace acestudio::ui {

namespace fs = std::filesystem;
using namespace acestudio::ai;
using nlohmann::json;

namespace {
constexpr const char* kSdxl = "sdxl-base-1.0";
constexpr const char* kControlNet = "controlnet-openpose-sdxl";
constexpr const char* kIpAdapter = "ip-adapter-plus-sdxl";
constexpr const char* kLora = "pixel-art-xl";
constexpr const char* kTaesd = "taesdxl";
constexpr int kThumb = 96;

std::string utf8(const fs::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

QString defaultWords(PoseCycle c) {
    switch (c) {
    case PoseCycle::Idle: return QStringLiteral("standing, idle");
    case PoseCycle::Walk: return QStringLiteral("walking");
    case PoseCycle::Run: return QStringLiteral("running");
    case PoseCycle::Jump: return QStringLiteral("jumping");
    }
    return {};
}

// Thumbnail with a coloured frame: green = consistent, amber = fixable, red = regenerate.
QIcon frameIcon(const Image& img, QColor border) {
    QPixmap pm(kThumb, kThumb);
    pm.fill(QColor(32, 32, 38));
    QPainter p(&pm);
    for (int y = 0; y < kThumb; y += 8)   // checkerboard under transparency
        for (int x = 0; x < kThumb; x += 8)
            if (((x + y) / 8) % 2) p.fillRect(x, y, 8, 8, QColor(44, 44, 52));
    const QImage q = toQImage(img).scaled(kThumb - 6, kThumb - 6, Qt::KeepAspectRatio, Qt::FastTransformation);
    p.drawImage((kThumb - q.width()) / 2, (kThumb - q.height()) / 2, q);
    if (border.isValid()) {
        p.setPen(QPen(border, 3));
        p.drawRect(1, 1, kThumb - 3, kThumb - 3);
    }
    return QIcon(pm);
}
} // namespace

AnimationDialog::AnimationDialog(AppContext& ctx, AiService& ai, DocumentView* view, NewDocumentFn newDocument, QWidget* parent)
    : QDialog(parent), m_ctx(ctx), m_ai(ai), m_view(view), m_newDocument(std::move(newDocument)) {
    setWindowTitle(tr("Generate Animation"));
    m_session = ctx.dataDir() / "ai" / "anim" / QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-hhmmss-zzz")).toStdString();

    m_character = new QComboBox(this);
    m_character->setMinimumWidth(180);
    auto* manage = new QPushButton(tr("&Characters…"), this);
    auto* charRow = new QHBoxLayout;
    charRow->addWidget(m_character, 1);
    charRow->addWidget(manage);
    m_action = new QComboBox(this);
    for (PoseCycle c : {PoseCycle::Walk, PoseCycle::Run, PoseCycle::Idle, PoseCycle::Jump})
        m_action->addItem(QString::fromLatin1(toString(c).data()).replace(0, 1, QString::fromLatin1(toString(c).data()).left(1).toUpper()),
                          QString::fromLatin1(toString(c).data()));
    m_frames = new QSpinBox(this);
    m_frames->setRange(2, 16);
    m_frames->setValue(8);
    m_duration = new QSpinBox(this);
    m_duration->setRange(20, 2000);
    m_duration->setValue(100);
    m_duration->setSuffix(tr(" ms"));
    m_actionWords = new QLineEdit(this);
    m_actionWords->setToolTip(tr("Added to the character's description for every frame"));
    m_steps = new QSpinBox(this);
    m_steps->setRange(10, 60);
    m_steps->setValue(28);
    m_poseStrength = new QSpinBox(this);
    m_poseStrength->setRange(10, 200);
    m_poseStrength->setValue(85);
    m_poseStrength->setSuffix(QStringLiteral(" %"));
    m_poseStrength->setToolTip(tr("How strictly frames follow the skeleton"));
    m_precision = new QComboBox(this);
    m_precision->addItem(tr("Auto"), QStringLiteral("auto"));
    m_precision->addItem(tr("Full"), QStringLiteral("full"));
    m_precision->setToolTip(tr("Auto stores the model weights in FP8 on GPUs with less than 10 GB, so pose control fits in "
                               "memory (~1 s per step on 8 GB). Full keeps FP16 weights: same quality reference, but on "
                               "an 8 GB GPU it spills into shared memory and is ~30× slower."));
    m_useRefs = new QCheckBox(tr("Use the character's reference images"), this);
    m_useRefs->setChecked(true);
    m_removeBg = new QCheckBox(tr("Remove the background"), this);
    m_removeBg->setChecked(true);
    m_removeBg->setToolTip(tr("Makes the plain backdrop around the character transparent"));

    auto* form = new QFormLayout;
    form->addRow(tr("Character:"), charRow);
    auto* actRow = new QHBoxLayout;
    actRow->addWidget(m_action);
    actRow->addWidget(new QLabel(tr("Frames"), this));
    actRow->addWidget(m_frames);
    actRow->addWidget(new QLabel(tr("Duration"), this));
    actRow->addWidget(m_duration);
    actRow->addStretch();
    form->addRow(tr("Action:"), actRow);
    form->addRow(tr("Action words:"), m_actionWords);
    auto* qRow = new QHBoxLayout;
    qRow->addWidget(new QLabel(tr("Steps"), this));
    qRow->addWidget(m_steps);
    qRow->addWidget(new QLabel(tr("Pose strength"), this));
    qRow->addWidget(m_poseStrength);
    qRow->addWidget(new QLabel(tr("Precision"), this));
    qRow->addWidget(m_precision);
    qRow->addStretch();
    form->addRow(tr("Quality:"), qRow);
    form->addRow(QString(), m_useRefs);
    form->addRow(QString(), m_removeBg);

    m_strip = new QListWidget(this);
    m_strip->setViewMode(QListView::IconMode);
    m_strip->setFlow(QListView::LeftToRight);
    m_strip->setWrapping(true);
    m_strip->setIconSize(QSize(kThumb, kThumb));
    m_strip->setGridSize(QSize(kThumb + 14, kThumb + 30));
    m_strip->setMovement(QListView::Static);
    m_strip->setMinimumHeight(2 * (kThumb + 30) + 8);
    m_strip->setToolTip(tr("Double-click a frame to edit its pose"));
    m_status = new QLabel(this);
    m_status->setWordWrap(true);

    m_generate = new QPushButton(tr("&Generate"), this);
    m_regen = new QPushButton(tr("&Regenerate Flagged"), this);
    m_regen->setToolTip(tr("New attempts for frames whose shape or colours jumped"));
    m_correct = new QPushButton(tr("Auto-&Correct"), this);
    m_correct->setToolTip(tr("Fix ground line, height and off-palette colours"));
    m_commit = new QPushButton(tr("C&ommit"), this);
    m_commit->setToolTip(tr("Add the frames to the document as a new layer and tag (one undo step)"));
    m_cancel = new QPushButton(tr("Cancel &Jobs"), this);
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(close, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* buttons = new QHBoxLayout;
    for (QPushButton* b : {m_generate, m_regen, m_correct, m_commit, m_cancel}) buttons->addWidget(b);
    buttons->addStretch();
    buttons->addWidget(close);

    auto* root = new QVBoxLayout(this);
    root->addLayout(form);
    root->addWidget(m_strip, 1);
    root->addWidget(m_status);
    root->addLayout(buttons);
    resize(820, 600);

    connect(manage, &QPushButton::clicked, this, [this] {
        CharacterDialog dlg(m_ctx.dataDir() / "characters", m_view, this);
        dlg.exec();
        const auto sel = dlg.selectedPath();
        reloadCharacters(sel ? qs(sel->stem().string()) : QString());
    });
    connect(m_character, &QComboBox::currentIndexChanged, this, [this] {
        m_profile.reset();
        const QString path = m_character->currentData().toString();
        if (!path.isEmpty()) {
            auto c = loadCharacter(toPath(path));
            if (c) m_profile = std::move(c).value();
            else m_status->setText(qs(c.error().message));
        }
        resetPoses();
    });
    connect(m_action, &QComboBox::currentIndexChanged, this, [this] {
        if (auto c = poseCycleFromString(m_action->currentData().toString().toStdString())) m_actionWords->setText(defaultWords(*c));
        resetPoses();
    });
    connect(m_frames, &QSpinBox::valueChanged, this, [this] { resetPoses(); });
    connect(m_strip, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* it) { editPose(m_strip->row(it)); });
    connect(m_generate, &QPushButton::clicked, this, [this] {
        std::vector<int> all;
        for (int i = 0; i < static_cast<int>(m_poses.size()); ++i) all.push_back(i);
        generate(all, true);
    });
    connect(m_regen, &QPushButton::clicked, this, [this] {
        std::vector<int> todo;
        if (m_report) todo = m_report->toRegenerate();
        for (std::size_t i = 0; i < m_results.size(); ++i)
            if (!m_results[i] && std::find(todo.begin(), todo.end(), static_cast<int>(i)) == todo.end()) todo.push_back(static_cast<int>(i));
        if (!todo.empty()) generate(todo, false);
    });
    connect(m_correct, &QPushButton::clicked, this, &AnimationDialog::autoCorrect);
    connect(m_commit, &QPushButton::clicked, this, &AnimationDialog::commit);
    connect(m_cancel, &QPushButton::clicked, this, [this] {
        // Copy the ids: a cancelled job reports back (onJobFinished erases
        // it from m_jobs), which must not happen under a live iterator.
        std::vector<std::string> ids;
        for (const auto& [id, f] : m_jobs) ids.push_back(id);
        for (const auto& id : ids) m_ai.cancel(id);
    });
    connect(&m_ai, &AiService::jobFinished, this, &AnimationDialog::onJobFinished);
    connect(&m_ai, &AiService::jobChanged, this, [this](const QString& qid) {
        if (!m_jobs.count(qid.toStdString())) return;
        const Job* j = m_ai.job(qid.toStdString());
        if (!j || j->state != JobState::Running) return;
        const int f = m_jobs[qid.toStdString()];
        m_status->setText(tr("Frame %1: %2").arg(f + 1).arg(qs(j->message)));
        // Live preview (TAESD-XL) on the frame's tile while it generates.
        if (j->previewSerial > 0 && j->previewSerial != m_previewSerial && f < m_strip->count()) {
            m_previewSerial = j->previewSerial;
            QImage img;
            if (img.load(fromPath(j->preview)))
                m_strip->item(f)->setIcon(QIcon(QPixmap::fromImage(img.scaled(128, 128, Qt::KeepAspectRatio, Qt::SmoothTransformation))));
        }
    });

    m_actionWords->setText(defaultWords(PoseCycle::Walk));
    reloadCharacters();
}

AnimationDialog::~AnimationDialog() {
    // Stop listening first: the cancellations must not call back into a
    // dialog that is being destroyed.
    disconnect(&m_ai, nullptr, this, nullptr);
    const auto jobs = m_jobs;
    for (const auto& [id, f] : jobs) m_ai.cancel(id);
    std::error_code ec;
    fs::remove_all(m_session, ec);   // pose/reference inputs written for this dialog only
}

void AnimationDialog::reloadCharacters(const QString& selectId) {
    const QSignalBlocker block(m_character);
    m_character->clear();
    int pick = 0;
    for (const auto& e : listCharacters(m_ctx.dataDir() / "characters")) {
        if (qs(e.id) == selectId) pick = m_character->count();
        m_character->addItem(qs(e.name), fromPath(e.path));
    }
    if (m_character->count() == 0) m_character->addItem(tr("(no characters — use Characters… to create one)"), QString());
    m_character->setCurrentIndex(pick);
    m_profile.reset();
    if (const QString path = m_character->currentData().toString(); !path.isEmpty())
        if (auto c = loadCharacter(toPath(path))) m_profile = std::move(c).value();
    resetPoses();
}

void AnimationDialog::resetPoses() {
    const auto cycle = poseCycleFromString(m_action->currentData().toString().toStdString()).value_or(PoseCycle::Walk);
    m_poses = cyclePoses(cycle, m_frames->value());
    if (m_profile && !m_profile->facingRight)
        for (auto& p : m_poses) p = p.mirrored();
    m_results.assign(m_poses.size(), std::nullopt);
    m_errors.assign(m_poses.size(), QString());
    m_attempts.assign(m_poses.size(), 0);
    m_report.reset();
    m_box.reset();
    m_dupOf.clear();
    m_corrected = false;
    refreshStrip();
}

void AnimationDialog::refreshStrip() {
    m_strip->clear();
    for (std::size_t i = 0; i < m_poses.size(); ++i) {
        QIcon icon;
        QString label = tr("Frame %1").arg(i + 1), tip;
        bool pending = false;
        for (const auto& [id, f] : m_jobs) pending = pending || f == static_cast<int>(i);
        pending = pending || m_dupOf.count(static_cast<int>(i)) > 0;
        if (m_results[i]) {
            QColor border;
            if (m_report && i < m_report->frames.size()) {
                const FrameQA& q = m_report->frames[i];
                border = q.ok() ? QColor(70, 190, 100) : q.needsRegeneration() ? QColor(220, 70, 60) : QColor(230, 170, 40);
                label += QStringLiteral(" · %1").arg(qRound(q.score * 100));
                tip = qs(describe(q));
            }
            icon = frameIcon(*m_results[i], border);
        } else {
            icon = frameIcon(renderPose(m_poses[i], 128, 128), QColor());
            tip = pending ? tr("Generating…") : m_errors[i].isEmpty() ? tr("Pose (double-click to edit)") : m_errors[i];
            if (pending) label += tr(" …");
            else if (!m_errors[i].isEmpty()) label += tr(" ✕");
        }
        auto* it = new QListWidgetItem(icon, label, m_strip);
        it->setToolTip(tip);
    }
    updateButtons();
}

void AnimationDialog::updateButtons() {
    const bool busy = !m_jobs.empty();
    bool any = false, all = !m_results.empty();
    for (const auto& r : m_results) { any = any || r.has_value(); all = all && r.has_value(); }
    m_generate->setEnabled(!busy && m_profile.has_value());
    m_regen->setEnabled(!busy && any && (!all || (m_report && !m_report->toRegenerate().empty())));
    m_correct->setEnabled(!busy && all && m_report && !m_report->failing().empty());
    m_commit->setEnabled(!busy && all);
    m_cancel->setEnabled(busy);
    for (QWidget* w : std::initializer_list<QWidget*>{m_character, m_action, m_frames}) w->setEnabled(!busy);
}

bool AnimationDialog::checkModels(QString* missing) const {
    QStringList need;
    const ModelCatalog* cat = m_ai.catalog();
    ModelStore* store = const_cast<AiService&>(m_ai).store();
    auto installed = [&](const char* id) {
        const ModelEntry* m = cat ? cat->find(id) : nullptr;
        return m && store && store->state(*m) == InstallState::Installed;
    };
    if (!installed(kSdxl)) need << QStringLiteral("SDXL Base 1.0");
    if (!installed(kControlNet)) need << QStringLiteral("OpenPose ControlNet for SDXL");
    if (missing) *missing = need.join(QStringLiteral(", "));
    return need.isEmpty();
}

void AnimationDialog::generate(const std::vector<int>& frames, bool newSeed) {
    if (!m_profile) return;
    QString missing;
    if (!checkModels(&missing)) {
        QMessageBox::information(this, windowTitle(), tr("Animation generation needs these models: %1.\n\nUse AI Studio › Models… to download them.").arg(missing));
        return;
    }
    QString why;
    if (!m_ai.ensureStarted(&why)) { QMessageBox::information(this, windowTitle(), why); return; }
    const CharacterProfile& c = *m_profile;
    if (newSeed || m_seed < 0) {
        std::random_device rd;
        m_seed = c.seed >= 0 ? c.seed : static_cast<std::int64_t>(rd() % 2147483000u);
    }
    std::error_code ec;
    fs::create_directories(m_session, ec);
    if (ec) { m_status->setText(tr("Cannot create %1: %2").arg(fromPath(m_session), qs(ec.message()))); return; }
    const ModelCatalog& cat = *m_ai.catalog();
    ModelStore& store = *m_ai.store();
    // Generation size: longest side 1024, matching the sprite's aspect, multiple of 8.
    const double aspect = static_cast<double>(c.width) / c.height;
    const int gw = std::max(512, static_cast<int>(1024 * std::min(1.0, aspect)) / 8 * 8);
    const int gh = std::max(512, static_cast<int>(1024 * std::min(1.0, 1.0 / aspect)) / 8 * 8);
    // One framing box for the whole animation: regenerated frames reuse it so
    // they line up with the frames already made (even after a pose edit).
    if (newSeed || !m_box) m_box = poseFramingBox(m_poses, aspect * gh / static_cast<double>(gw));
    const auto box = *m_box;
    // Reference images (written once per session).
    json refs = json::array();
    const ModelEntry* ip = cat.find(kIpAdapter);
    const bool useRefs = m_useRefs->isChecked() && !c.references.empty() && ip && store.state(*ip) == InstallState::Installed;
    if (useRefs)
        for (std::size_t i = 0; i < c.references.size(); ++i) {
            const fs::path p = m_session / ("ref" + std::to_string(i) + ".png");
            if (!fs::exists(p, ec) && !writePng(c.references[i], p)) continue;
            refs.push_back(utf8(p));
        }
    json palette = json::array();
    for (const auto& col : c.palette.colors()) palette.push_back(col.toHex(false));
    const QString words = m_actionWords->text().trimmed();
    std::string prompt = c.prompt;
    if (!words.isEmpty()) prompt += (prompt.empty() ? "" : ", ") + words.toStdString();
    prompt += c.facingRight ? ", full body, side view facing right" : ", full body, side view facing left";
    prompt += ", single character, isolated on a plain white background, game sprite";
    std::string negative = c.negative;
    negative += std::string(negative.empty() ? "" : ", ") + "multiple characters, sprite sheet, cropped, shadow, floor, ground, scenery, text, watermark";

    // Frames with the same pose and attempt as one already submitted here
    // (a jump's rise and fall, an idle that returns to its start) would give
    // the same image: reuse that job's result instead of running SDXL again.
    std::vector<int> submitted;
    for (int f : frames) {
        if (f < 0 || f >= static_cast<int>(m_poses.size())) continue;
        const auto fi = static_cast<std::size_t>(f);
        bool dup = false;
        for (int g : submitted) {
            const auto gi = static_cast<std::size_t>(g);
            if (m_poses[gi] == m_poses[fi] && m_attempts[gi] == m_attempts[fi] + 1) {
                m_dupOf[f] = g;
                ++m_attempts[fi];
                m_results[fi].reset();
                m_errors[fi].clear();
                dup = true;
                break;
            }
        }
        if (dup) continue;
        submitted.push_back(f);
        const fs::path posePng = m_session / ("pose" + std::to_string(f) + "_" + std::to_string(m_attempts[static_cast<std::size_t>(f)]) + ".png");
        if (auto st = writePng(renderPose(m_poses[static_cast<std::size_t>(f)], gw, gh), posePng); !st) {
            m_errors[static_cast<std::size_t>(f)] = qs(st.error().message);
            continue;
        }
        const int attempt = m_attempts[static_cast<std::size_t>(f)]++;
        json p = {{"model_dir", utf8(store.entryPath(*cat.find(kSdxl)))},
                  {"prompt", prompt},
                  {"negative_prompt", negative},
                  {"width", gw}, {"height", gh},
                  {"steps", m_steps->value()},
                  {"guidance", 6.0},
                  {"weights", m_precision->currentData().toString().toStdString()},
                  // Same seed for every frame keeps the look; retries vary it.
                  {"seed", static_cast<std::int64_t>((m_seed + attempt * 7919) % 2147483000)},
                  {"controlnet", {{"dir", utf8(store.entryPath(*cat.find(kControlNet)))}, {"image", utf8(posePng)},
                                  {"scale", m_poseStrength->value() / 100.0}}},
                  {"pixel", {{"width", c.width}, {"height", c.height}, {"box", {box[0], box[1], box[2], box[3]}}}}};
        if (!palette.empty()) p["pixel"]["palette"] = palette;
        else p["pixel"]["colors"] = 32;
        if (const ModelEntry* l = cat.find(kLora); l && store.state(*l) == InstallState::Installed && c.loraStrength > 0)
            p["lora"] = {{"path", utf8(store.entryPath(*l))}, {"scale", c.loraStrength / 100.0}};
        if (const ModelEntry* tae = cat.find(kTaesd); tae && store.state(*tae) == InstallState::Installed)
            p["preview"] = {{"dir", utf8(store.entryPath(*tae))}, {"every", 4}};
        if (useRefs && !refs.empty())
            p["ip_adapter"] = {{"dir", utf8(store.entryPath(*ip))}, {"weight", "ip-adapter-plus_sdxl_vit-h.safetensors"},
                               {"encoder", "models/image_encoder"}, {"images", refs},
                               {"scale", c.referenceStrength / 100.0}};
        auto id = m_ai.submit("image.generate", std::move(p), tr("%1: frame %2").arg(qs(c.name)).arg(f + 1));
        if (!id) { m_errors[static_cast<std::size_t>(f)] = qs(id.error().message); continue; }
        m_jobs[id.value()] = f;
        m_results[static_cast<std::size_t>(f)].reset();
        m_errors[static_cast<std::size_t>(f)].clear();
    }
    m_report.reset();
    m_corrected = false;
    m_status->setText(tr("Generating %n frame(s)…", nullptr, static_cast<int>(m_jobs.size())));
    refreshStrip();
}

void AnimationDialog::onJobFinished(const QString& qid) {
    auto it = m_jobs.find(qid.toStdString());
    if (it == m_jobs.end()) return;
    const int f = it->second;
    m_jobs.erase(it);
    const Job* j = m_ai.job(qid.toStdString());
    if (j && j->state == JobState::Succeeded) {
        auto o = j->outputs.find("image0");
        auto img = o != j->outputs.end() ? readPng(o->second) : Result<Image>(Error{ErrorCode::NotFound, "no image"});
        if (img) {
            Image frame = std::move(img).value();
            if (m_removeBg->isChecked()) frame = removeDetachedBelow(removeBorderBackground(frame));
            m_results[static_cast<std::size_t>(f)] = std::move(frame);
        } else {
            m_errors[static_cast<std::size_t>(f)] = qs(img.error().message);
        }
    } else if (j) {
        m_errors[static_cast<std::size_t>(f)] = j->state == JobState::Cancelled ? tr("Cancelled") : qs(j->error).section(QLatin1Char('\n'), 0, 0);
    }
    for (auto d = m_dupOf.begin(); d != m_dupOf.end();) {
        if (d->second != f) { ++d; continue; }
        m_results[static_cast<std::size_t>(d->first)] = m_results[static_cast<std::size_t>(f)];
        m_errors[static_cast<std::size_t>(d->first)] = m_errors[static_cast<std::size_t>(f)];
        d = m_dupOf.erase(d);
    }
    if (m_jobs.empty()) {
        bool all = true;
        for (const auto& r : m_results) all = all && r.has_value();
        if (all) {
            evaluate();
        } else {
            int done = 0, cancelled = 0;
            for (std::size_t i = 0; i < m_results.size(); ++i) {
                if (m_results[i]) ++done;
                else if (m_errors[i] == tr("Cancelled")) ++cancelled;
            }
            const int failed = static_cast<int>(m_results.size()) - done - cancelled;
            if (done == 0 && failed == 0)
                m_status->setText(tr("Cancelled."));
            else if (done == 0)
                m_status->setText(tr("No frames were generated: %1").arg(m_errors.front().isEmpty() ? tr("see the log") : m_errors.front()));
            else
                m_status->setText(tr("%1 of %2 frame(s) missing (%3 failed, %4 cancelled); Regenerate Flagged fills them in.")
                                      .arg(static_cast<int>(m_results.size()) - done).arg(m_results.size()).arg(failed).arg(cancelled));
        }
    } else {
        m_status->setText(tr("Generating… %n frame(s) left", nullptr, static_cast<int>(m_jobs.size())));
    }
    refreshStrip();
}

QAOptions AnimationDialog::qaOptions() const {
    QAOptions o;
    // Each frame is judged against what its pose asks for (a jump leaves the
    // ground, a crouch is shorter), not against standing still.
    if (m_profile && m_box) o.expected = expectationsFromPoses(m_poses, *m_box, m_profile->height);
    // A jump does not loop back to its first frame; the other cycles do.
    o.cyclic = m_action->currentData().toString() != QLatin1String("jump");
    if (m_profile && m_useRefs->isChecked()) o.identity = m_profile->references;
    return o;
}

void AnimationDialog::evaluate() {
    if (!m_profile) return;
    std::vector<Image> frames;
    for (const auto& r : m_results) frames.push_back(r ? *r : Image(m_profile->width, m_profile->height));
    if (frames.empty()) return;
    // The first frame is the reference for ground line, height and colours.
    m_report = evaluateFrames(frames, frames.front(), m_profile->palette, qaOptions());
    const auto failing = m_report->failing(), regen = m_report->toRegenerate();
    QString text = tr("Consistency %1% (compared with frame 1).").arg(qRound(m_report->meanScore() * 100));
    if (failing.empty()) text += tr(" All frames pass.");
    else {
        text += tr(" %n frame(s) need attention", nullptr, static_cast<int>(failing.size()));
        if (!regen.empty()) text += tr(", %n of them need regenerating", nullptr, static_cast<int>(regen.size()));
        text += QStringLiteral(". ");
        text += m_corrected ? tr("Hover a frame for details.") : tr("Auto-Correct fixes ground line, height and palette.");
    }
    m_status->setText(text);
}

void AnimationDialog::autoCorrect() {
    if (!m_profile || !m_report) return;
    std::vector<Image> frames;
    for (const auto& r : m_results) frames.push_back(r ? *r : Image(m_profile->width, m_profile->height));
    const auto fixed = correctFrames(frames, *m_report, m_profile->palette, qaOptions());
    for (std::size_t i = 0; i < fixed.size(); ++i) if (m_results[i]) m_results[i] = fixed[i];
    m_corrected = true;
    evaluate();
    refreshStrip();
}

void AnimationDialog::editPose(int frame) {
    if (frame < 0 || frame >= static_cast<int>(m_poses.size()) || !m_jobs.empty()) return;
    QImage under;
    if (m_results[static_cast<std::size_t>(frame)]) under = toQImage(*m_results[static_cast<std::size_t>(frame)]);
    PoseDialog dlg(m_poses[static_cast<std::size_t>(frame)], under, this);
    if (dlg.exec() != QDialog::Accepted) return;
    m_poses[static_cast<std::size_t>(frame)] = dlg.pose();
    m_results[static_cast<std::size_t>(frame)].reset();   // the result no longer matches its pose
    m_report.reset();
    refreshStrip();
}

void AnimationDialog::commit() {
    if (!m_profile) return;
    std::vector<Image> frames;
    for (const auto& r : m_results) {
        if (!r) return;
        frames.push_back(*r);
    }
    const QString action = m_action->currentText();
    const QString name = tr("%1 %2").arg(qs(m_profile->name), action.toLower());
    const bool fits = m_view && m_view->document().width() == m_profile->width && m_view->document().height() == m_profile->height;
    if (!fits) {
        std::vector<int> durations(frames.size(), m_duration->value());
        if (m_newDocument) m_newDocument(frames, durations, name);
        accept();
        return;
    }
    Editor& ed = m_view->editor();
    // A blank document is filled from frame 1; otherwise the action is
    // appended after the existing frames with its own tag, so actions never
    // draw over each other (one tagged range per action, as in a sprite sheet).
    const int start = ed.contentBounds().empty() ? 0 : ed.document().frameCount();
    auto id = ed.addAnimationLayer(name.toStdString(), frames, start, m_duration->value(), action.toLower().toStdString());
    if (!id) { QMessageBox::warning(this, windowTitle(), qs(id.error().message)); return; }
    m_view->setCurrentLayer(id.value());
    accept();
}

} // namespace acestudio::ui
