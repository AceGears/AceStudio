#include "SpriteAnimateDialog.h"

#include "QtBridge.h"
#include "ai/AiService.h"

#include "acestudio/core/AiWorker.h"
#include "acestudio/core/AnimRouter.h"
#include "acestudio/core/ModelCatalog.h"
#include "acestudio/core/PngCodec.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <cmath>
#include <QApplication>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <initializer_list>

namespace acestudio::ui {

namespace {
const char* bodyLabel(::acestudio::motion::Body b) {
    switch (b) {
    case ::acestudio::motion::Body::Biped: return QT_TRANSLATE_NOOP("SpriteAnimateDialog", "Two legs");
    case ::acestudio::motion::Body::Quadruped: return QT_TRANSLATE_NOOP("SpriteAnimateDialog", "Four legs");
    case ::acestudio::motion::Body::Blob: return QT_TRANSLATE_NOOP("SpriteAnimateDialog", "Blob (hops)");
    case ::acestudio::motion::Body::Flyer: return QT_TRANSLATE_NOOP("SpriteAnimateDialog", "Flyer (hovers)");
    case ::acestudio::motion::Body::Object: return QT_TRANSLATE_NOOP("SpriteAnimateDialog", "Object (rocks)");
    case ::acestudio::motion::Body::Swimmer: return QT_TRANSLATE_NOOP("SpriteAnimateDialog", "Swimmer (body wave)");
    }
    return "";
}
} // namespace

SpriteAnimateDialog::SpriteAnimateDialog(const Image& sprite, const QString& name, OpenFn open, QWidget* parent, const Mask& wingMask,
                                         AiService* ai)
    : QDialog(parent), m_sprite(sprite), m_wingMask(wingMask), m_open(std::move(open)), m_ai(ai) {
    m_name = name;
    setWindowTitle(tr("Animate Sprite"));
    m_subject = ::acestudio::motion::analyzeSubject(m_sprite);
    // Rendering every chosen action of a 1,000 px sprite took seconds per
    // change on the test PC; the preview works on a copy at most kPreview px.
    constexpr int kPreview = 320;
    const int longest = std::max(m_sprite.width(), m_sprite.height());
    if (longest > kPreview) {
        m_previewScale = static_cast<double>(kPreview) / longest;
        const int sw = std::max(1, static_cast<int>(std::lround(m_sprite.width() * m_previewScale)));
        const int sh = std::max(1, static_cast<int>(std::lround(m_sprite.height() * m_previewScale)));
        m_small = fromQImage(toQImage(m_sprite).scaled(sw, sh, Qt::IgnoreAspectRatio, Qt::FastTransformation));
        if (!m_wingMask.isNull()) {
            m_smallMask = Mask(sw, sh);
            for (int y = 0; y < sh; ++y)
                for (int x = 0; x < sw; ++x)
                    m_smallMask.set(x, y, m_wingMask.get(x * m_wingMask.width() / sw, y * m_wingMask.height() / sh));
        }
    }

    auto* root = new QHBoxLayout(this);
    auto* left = new QVBoxLayout;
    root->addLayout(left, 0);

    auto* actionsLabel = new QLabel(tr("&Actions:"), this);
    m_actions = new QListWidget(this);
    actionsLabel->setBuddy(m_actions);
    for (const ::acestudio::motion::ActionInfo& info : ::acestudio::motion::actions()) {
        auto* it = new QListWidgetItem(tr(info.label), m_actions);
        it->setData(Qt::UserRole, static_cast<int>(info.action));
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        const bool basic = info.action == ::acestudio::motion::Action::Idle || info.action == ::acestudio::motion::Action::Walk;
        it->setCheckState(basic ? Qt::Checked : Qt::Unchecked);
    }
    m_actions->setMinimumWidth(190);
    left->addWidget(actionsLabel);
    left->addWidget(m_actions, 1);
    auto* presets = new QHBoxLayout;
    auto* basic = new QPushButton(tr("Basic Set"), this);
    basic->setToolTip(tr("Idle, walk, run, jump, attack, hurt and death"));
    auto* all = new QPushButton(tr("All"), this);
    auto* none = new QPushButton(tr("None"), this);
    // Enter in a spin box must not press one of these (it replaced the chosen
    // actions with the basic set on the test PC).
    for (QPushButton* b : {basic, all, none}) b->setAutoDefault(false);
    presets->addWidget(basic);
    presets->addWidget(all);
    presets->addWidget(none);
    left->addLayout(presets);
    auto setChecked = [this](const std::function<bool(::acestudio::motion::Action)>& pred) {
        m_actions->blockSignals(true);
        for (int i = 0; i < m_actions->count(); ++i) {
            const auto a = static_cast<::acestudio::motion::Action>(m_actions->item(i)->data(Qt::UserRole).toInt());
            m_actions->item(i)->setCheckState(pred(a) ? Qt::Checked : Qt::Unchecked);
        }
        m_actions->blockSignals(false);
        rebuild();
    };
    connect(basic, &QPushButton::clicked, this, [setChecked] {
        setChecked([](::acestudio::motion::Action a) {
            using A = ::acestudio::motion::Action;
            return a == A::Idle || a == A::Walk || a == A::Run || a == A::Jump || a == A::Attack1 || a == A::Hurt || a == A::Death;
        });
    });
    connect(all, &QPushButton::clicked, this, [setChecked] { setChecked([](::acestudio::motion::Action a) { return a != ::acestudio::motion::Action::Custom; }); });
    connect(none, &QPushButton::clicked, this, [setChecked] { setChecked([](::acestudio::motion::Action) { return false; }); });

    auto* right = new QVBoxLayout;
    root->addLayout(right, 1);
    auto* form = new QFormLayout;
    // Method: body motion or an environmental effect. The router suggests
    // one from the sprite's name and pixels; the user can pick another.
    const ::acestudio::anim::Route route = ::acestudio::anim::routeAnimation(m_sprite, m_name.toStdString());
    m_method = new QComboBox(this);
    m_method->setAccessibleName(tr("Animation method"));
    m_method->addItem(tr("Body motion (actions)"), -1);
    // Item data: effect * 16 + fire style.
    using FS = ::acestudio::fx::FireStyle;
    auto code = [](::acestudio::fx::Effect e, FS s = FS::Campfire) { return static_cast<int>(e) * 16 + static_cast<int>(s); };
    m_method->addItem(tr("Fire: campfire"), code(::acestudio::fx::Effect::Fire, FS::Campfire));
    m_method->addItem(tr("Fire: torch (narrow, fast)"), code(::acestudio::fx::Effect::Fire, FS::Torch));
    m_method->addItem(tr("Fire: furnace (strong, bright)"), code(::acestudio::fx::Effect::Fire, FS::Furnace));
    m_method->addItem(tr("Fire: raging (big, splitting)"), code(::acestudio::fx::Effect::Fire, FS::Raging));
    m_method->addItem(tr("Fire: magic (swirling)"), code(::acestudio::fx::Effect::Fire, FS::Magic));
    m_method->addItem(tr("Water (ripple)"), code(::acestudio::fx::Effect::Water));
    m_method->addItem(tr("Plant (sway)"), code(::acestudio::fx::Effect::Sway));
    m_method->addItem(tr("Flag or cloth (wave)"), code(::acestudio::fx::Effect::Flag));
    m_method->addItem(tr("Smoke (rise)"), code(::acestudio::fx::Effect::Smoke));
    m_method->addItem(tr("Magic (pulse and glow)"), code(::acestudio::fx::Effect::Pulse));
    // New drawings where moving pixels cannot work (a wingbeat): FLUX.2 klein
    // redraws the wings around the protected body, on this PC.
    if (m_ai) m_method->addItem(tr("AI key poses: wing flap (FLUX.2 klein)"), -2);
    if (route.method == ::acestudio::anim::Method::Effect)
        m_method->setCurrentIndex(std::max(0, m_method->findData(code(route.effect, route.fireStyle))));
    m_method->setToolTip(tr("Effects animate only the part that should move (the flames, the canopy, the cloth) "
                            "in the sprite's own colours, as a seamless loop. No AI is used."));
    form->addRow(tr("Me&thod:"), m_method);
    m_route = new QLabel(tr("Suggested: %1").arg(qs(route.reason)), this);
    m_route->setWordWrap(true);
    m_route->setProperty("role", "muted");
    form->addRow(QString(), m_route);
    m_body = new QComboBox(this);
    m_body->addItem(tr("Automatic (%1)").arg(tr(bodyLabel(m_subject.body))), QString());
    for (::acestudio::motion::Body b : {::acestudio::motion::Body::Biped, ::acestudio::motion::Body::Quadruped, ::acestudio::motion::Body::Blob, ::acestudio::motion::Body::Flyer, ::acestudio::motion::Body::Object, ::acestudio::motion::Body::Swimmer})
        m_body->addItem(tr(bodyLabel(b)), QString::fromLatin1(::acestudio::motion::toString(b)));
    m_body->setToolTip(tr("How the sprite moves. Automatic is measured from its silhouette: separate feet walk, "
                          "a round shape hops, anything else rocks."));
    form->addRow(tr("&Body:"), m_body);
    // A water creature by name (a fish's fins can read as feet).
    if (route.method == ::acestudio::anim::Method::Body && route.body != m_subject.body)
        m_body->setCurrentIndex(std::max(0, m_body->findData(QString::fromLatin1(::acestudio::motion::toString(route.body)))));
    m_swim = new QComboBox(this);
    m_swim->setAccessibleName(tr("Swimming style"));
    m_swim->addItem(tr("Automatic (from the build)"), static_cast<int>(::acestudio::motion::SwimStyle::Auto));
    m_swim->addItem(tr("Eel: whole body undulates"), static_cast<int>(::acestudio::motion::SwimStyle::Anguilliform));
    m_swim->addItem(tr("Most fish: rear half bends"), static_cast<int>(::acestudio::motion::SwimStyle::Subcarangiform));
    m_swim->addItem(tr("Deep-bodied fish: rear third"), static_cast<int>(::acestudio::motion::SwimStyle::Carangiform));
    m_swim->addItem(tr("Tuna or shark: stiff body, fast tail"), static_cast<int>(::acestudio::motion::SwimStyle::Thunniform));
    m_swim->setCurrentIndex(std::max(0, m_swim->findData(static_cast<int>(route.swimStyle))));
    m_swim->setToolTip(tr("How much of a swimmer's body the wave runs through. Only used for swimmers."));
    form->addRow(tr("S&wimming:"), m_swim);
    m_face = new QComboBox(this);
    m_face->addItem(tr("Right (as drawn)"));
    m_face->addItem(tr("Left (mirrored)"));
    form->addRow(tr("&Facing:"), m_face);
    m_amount = new QSlider(Qt::Horizontal, this);
    m_amount->setRange(25, 200);
    m_amount->setValue(100);
    m_amount->setAccessibleName(tr("Movement amount"));
    form->addRow(tr("&Movement:"), m_amount);
    m_frames = new QSpinBox(this);
    m_frames->setRange(0, 32);
    m_frames->setSpecialValueText(tr("Default"));
    m_frames->setToolTip(tr("Frames per action when one action is chosen; Default uses each action's own count."));
    form->addRow(tr("F&rames:"), m_frames);
    m_speed = new QSpinBox(this);
    m_speed->setRange(25, 400);
    m_speed->setSingleStep(25);
    m_speed->setValue(100);
    m_speed->setSuffix(QStringLiteral(" %"));
    m_speed->setToolTip(tr("Preview playback speed (the frame durations you open or export do not change)"));
    form->addRow(tr("Preview &speed:"), m_speed);
    m_poseButton = new QPushButton(tr("Draw the Poses"), this);
    m_poseButton->setToolTip(tr("FLUX.2 klein redraws the wings in three new positions around the unchanged body "
                                "(two tries each, the more consistent one is kept). About a minute on this PC."));
    m_poseButton->setAutoDefault(false);
    m_poseButton->setVisible(false);
    connect(m_poseButton, &QPushButton::clicked, this, &SpriteAnimateDialog::startPoses);
    form->addRow(QString(), m_poseButton);
    m_wingLine = new QSlider(Qt::Horizontal, this);
    m_wingLine->setRange(10, 90);
    m_wingLine->setValue(45);
    m_wingLine->setToolTip(tr("Wing flap: everything above this line (percent of the height from the top) folds about it."));
    m_wingLabel = new QLabel(tr("&Wing line:"), this);
    m_wingLabel->setBuddy(m_wingLine);
    form->addRow(m_wingLabel, m_wingLine);
    connect(m_wingLine, &QSlider::valueChanged, this, &SpriteAnimateDialog::rebuild);
    auto* span = new QHBoxLayout;
    m_wingFrom = new QSpinBox(this);
    m_wingTo = new QSpinBox(this);
    for (QSpinBox* sb : {m_wingFrom, m_wingTo}) {
        sb->setRange(0, 100);
        sb->setSuffix(QStringLiteral(" %"));
        span->addWidget(sb);
    }
    // Values first, connections after: rebuild() needs every widget, and the
    // custom-motion box does not exist yet.
    m_wingFrom->setValue(0);
    m_wingTo->setValue(100);
    for (QSpinBox* sb : {m_wingFrom, m_wingTo}) connect(sb, &QSpinBox::valueChanged, this, &SpriteAnimateDialog::rebuild);
    m_wingFrom->setAccessibleName(tr("Wings start at (percent of the width)"));
    m_wingTo->setAccessibleName(tr("Wings end at (percent of the width)"));
    m_spanLabel = new QLabel(tr("Wing &span:"), this);
    m_spanLabel->setBuddy(m_wingFrom);
    m_spanLabel->setToolTip(tr("Only this part of the width folds, so a head or tail above the wing line stays still. "
                               "For exact control, select the wings before opening Animate Sprite: the selection is used instead."));
    if (!m_wingMask.isNull()) {
        m_wingFrom->setEnabled(false);
        m_wingTo->setEnabled(false);
        m_spanLabel->setText(tr("Wing &span: the selection"));
    }
    form->addRow(m_spanLabel, span);
    right->addLayout(form);

    m_customBox = new QGroupBox(tr("Custom motion"), this);
    auto* cf = new QFormLayout(m_customBox);
    auto spin = [this, cf](const QString& label, double max, double value, double step) {
        auto* s = new QDoubleSpinBox(m_customBox);
        s->setRange(-max, max);
        s->setSingleStep(step);
        s->setDecimals(2);
        s->setValue(value);
        cf->addRow(label, s);
        connect(s, &QDoubleSpinBox::valueChanged, this, &SpriteAnimateDialog::rebuild);
        return s;
    };
    m_bob = spin(tr("Bob (height):"), 0.5, 0.05, 0.01);
    m_sway = spin(tr("Sway (width):"), 0.5, 0.0, 0.01);
    m_squash = spin(tr("Squash:"), 0.5, 0.05, 0.01);
    m_tilt = spin(tr("Tilt (radians):"), 1.0, 0.0, 0.02);
    m_legSwing = spin(tr("Leg swing (radians):"), 1.2, 0.3, 0.05);
    right->addWidget(m_customBox);

    m_preview = new QLabel(this);
    m_preview->setMinimumSize(256, 256);
    m_preview->setAlignment(Qt::AlignCenter);
    m_preview->setProperty("card", true);
    m_preview->setAccessibleName(tr("Animation preview"));
    right->addWidget(m_preview, 1);
    m_info = new QLabel(this);
    m_info->setWordWrap(true);
    m_info->setProperty("role", "muted");
    right->addWidget(m_info);

    auto* buttons = new QDialogButtonBox(this);
    auto* openBtn = buttons->addButton(tr("Open as Document"), QDialogButtonBox::AcceptRole);
    auto* exportBtn = buttons->addButton(tr("Export Strips…"), QDialogButtonBox::ActionRole);
    buttons->addButton(QDialogButtonBox::Close);
    right->addWidget(buttons);
    connect(openBtn, &QPushButton::clicked, this, &SpriteAnimateDialog::openDocument);
    connect(exportBtn, &QPushButton::clicked, this, &SpriteAnimateDialog::exportStrips);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    for (QAbstractButton* b : buttons->buttons())
        if (auto* pb = qobject_cast<QPushButton*>(b)) pb->setAutoDefault(false);   // Enter in a field does nothing

    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &SpriteAnimateDialog::showFrame);
    connect(m_actions, &QListWidget::itemChanged, this, &SpriteAnimateDialog::rebuild);
    connect(m_actions, &QListWidget::currentRowChanged, this, [this] {
        // Preview the highlighted action when it is one of the checked ones.
        const auto checked = checkedActions();
        if (QListWidgetItem* it = m_actions->currentItem()) {
            const auto a = static_cast<::acestudio::motion::Action>(it->data(Qt::UserRole).toInt());
            const auto pos = std::find(checked.begin(), checked.end(), a);
            if (pos != checked.end()) {
                m_previewClip = static_cast<int>(pos - checked.begin());
                m_previewFrame = 0;
                showFrame();
            }
        }
    });
    connect(m_body, &QComboBox::currentIndexChanged, this, &SpriteAnimateDialog::rebuild);
    connect(m_face, &QComboBox::currentIndexChanged, this, &SpriteAnimateDialog::rebuild);
    connect(m_swim, &QComboBox::currentIndexChanged, this, &SpriteAnimateDialog::rebuild);
    connect(m_amount, &QSlider::valueChanged, this, &SpriteAnimateDialog::rebuild);
    connect(m_frames, &QSpinBox::valueChanged, this, &SpriteAnimateDialog::rebuild);
    connect(m_method, &QComboBox::currentIndexChanged, this, &SpriteAnimateDialog::rebuild);
    rebuild();
}

std::vector<::acestudio::motion::Action> SpriteAnimateDialog::checkedActions() const {
    std::vector<::acestudio::motion::Action> out;
    for (int i = 0; i < m_actions->count(); ++i)
        if (m_actions->item(i)->checkState() == Qt::Checked)
            out.push_back(static_cast<::acestudio::motion::Action>(m_actions->item(i)->data(Qt::UserRole).toInt()));
    return out;
}

::acestudio::motion::Options SpriteAnimateDialog::options(bool full) const {
    ::acestudio::motion::Options o;
    o.frames = m_frames->value();
    o.amount = m_amount->value() / 100.0;
    const QString body = m_body->currentData().toString();
    if (!body.isEmpty()) o.body = ::acestudio::motion::bodyFromString(body.toStdString());
    // The reduced copy moves like the full sprite: same measured body.
    else if (!full && m_previewScale < 1.0) o.body = m_subject.body;
    o.faceLeft = m_face->currentIndex() == 1;
    o.swim = static_cast<::acestudio::motion::SwimStyle>(m_swim->currentData().toInt());
    o.custom = {m_bob->value(), m_sway->value(), m_squash->value(), m_tilt->value(), m_legSwing->value()};
    o.wingLine = m_wingLine->value() / 100.0;
    o.wingFrom = m_wingFrom->value() / 100.0;
    o.wingTo = m_wingTo->value() / 100.0;
    o.wingMask = full || m_previewScale >= 1.0 ? m_wingMask : m_smallMask;
    return o;
}

bool SpriteAnimateDialog::aiMode() const { return m_method && m_method->currentData().toInt() == -2; }

SpriteAnimateDialog::~SpriteAnimateDialog() {
    // Jobs still running are cancelled: their result has nowhere to go.
    if (m_ai)
        for (const std::string& id : m_poseJobs)
            if (const ai::Job* j = m_ai->job(id); j && (j->state == ai::JobState::Queued || j->state == ai::JobState::Running)) m_ai->cancel(id);
}

void SpriteAnimateDialog::startPoses() {
    if (!m_ai || !m_ai->catalog() || !m_ai->store()) return;
    auto installed = [this](const char* id) -> const ::acestudio::ai::ModelEntry* {
        const ::acestudio::ai::ModelEntry* m = m_ai->catalog()->find(id);
        return m && m_ai->store()->state(*m) == ::acestudio::ai::InstallState::Installed ? m : nullptr;
    };
    const ::acestudio::ai::ModelEntry* klein = installed("flux2-klein-4b");
    const ::acestudio::ai::ModelEntry* cut = installed("birefnet-general");
    if (!cut) cut = installed("birefnet-lite");
    if (!klein || !cut) {
        m_info->setText(tr("AI key poses need FLUX.2 klein and a BiRefNet model (AI Studio › Models…)."));
        return;
    }
    auto path = [this](const ::acestudio::ai::ModelEntry& m) {
        const auto u = m_ai->store()->entryPath(m).u8string();
        return std::string(u.begin(), u.end());
    };
    m_plan = ::acestudio::keypose::planFor(::acestudio::motion::Action::Flap, m_subject.body);
    m_poseWings = ::acestudio::keypose::wingMask(m_sprite, m_wingLine->value() / 100.0, m_wingFrom->value() / 100.0,
                                                 m_wingTo->value() / 100.0, m_wingMask);
    m_inpaint = ::acestudio::keypose::inpaintInputs(m_sprite, m_poseWings, 512);
    m_poseJobs.clear();
    m_aiClip.reset();
    m_poseNotes.clear();
    const std::string subject = m_name.isEmpty() ? std::string("winged creature") : m_name.toStdString();
    for (const ::acestudio::keypose::PoseSpec& pose : m_plan.poses) {
        if (pose.original) continue;
        nlohmann::json p = {{"model_dir", path(*klein)},
                            {"prompt", ::acestudio::keypose::posePrompt(subject, pose)},
                            {"width", m_inpaint.init.width()},
                            {"height", m_inpaint.init.height()},
                            {"steps", 4},
                            {"seed", 4242},
                            {"count", 2},
                            {"strength", 1.0},
                            {"optimize", true},
                            {"cutout", {{"model", path(*cut)}, {"threshold", 0.5}}}};
        if (const ::acestudio::ai::ModelEntry* lora = installed("pixel-art-klein-lora")) p["lora"] = {{"path", path(*lora)}, {"scale", lora->loraScale}};
        auto id = m_ai->submit("image.klein", std::move(p), tr("Key pose: %1").arg(qs(pose.key)),
                               {{"init_image", &m_inpaint.init}, {"mask", &m_inpaint.mask}, {"reference0", &m_inpaint.init}});
        if (!id) {
            m_info->setText(qs(id.error().message));
            return;
        }
        m_poseJobs.push_back(id.value());
    }
    disconnect(m_poseConn);
    m_poseConn = connect(m_ai, &AiService::jobFinished, this, &SpriteAnimateDialog::onPoseJob);
    m_poseButton->setEnabled(false);
    m_info->setText(tr("Drawing %n pose(s) on this PC…", nullptr, static_cast<int>(m_poseJobs.size())));
}

void SpriteAnimateDialog::onPoseJob(const QString& qid) {
    if (std::find(m_poseJobs.begin(), m_poseJobs.end(), qid.toStdString()) == m_poseJobs.end()) return;
    int done = 0;
    for (const std::string& id : m_poseJobs)
        if (const ai::Job* j = m_ai->job(id); j && j->state != ai::JobState::Queued && j->state != ai::JobState::Running) ++done;
    if (done < static_cast<int>(m_poseJobs.size())) {
        m_info->setText(tr("Drawn %1 of %2 poses…").arg(done).arg(m_poseJobs.size()));
        return;
    }
    disconnect(m_poseConn);
    m_poseButton->setEnabled(true);
    std::vector<::acestudio::keypose::Aligned> chosen;
    std::size_t g = 0;
    for (const ::acestudio::keypose::PoseSpec& pose : m_plan.poses) {
        if (pose.original) continue;
        const ai::Job* j = m_ai->job(m_poseJobs[g++]);
        std::vector<::acestudio::keypose::Aligned> variants;
        if (j && j->state == ai::JobState::Succeeded)
            for (int i = 0; i < 2; ++i)
                if (auto o = j->outputs.find("image" + std::to_string(i)); o != j->outputs.end())
                    if (auto raw = readPng(o->second)) {
                        const Image reduced = ::acestudio::keypose::reduceToSprite(raw.value(), m_inpaint, m_sprite);
                        auto a = ::acestudio::keypose::align(reduced, m_sprite, m_plan, 3);
                        ::acestudio::keypose::restoreBody(a.image, m_sprite, m_poseWings);
                        variants.push_back(std::move(a));
                    }
        if (variants.empty()) {
            m_poseNotes << tr("%1: not drawn (%2)").arg(qs(pose.key), j ? qs(j->error).section(QLatin1Char('\n'), 0, 0) : tr("job lost"));
            m_info->setText(m_poseNotes.join(QStringLiteral("<br>")));
            return;
        }
        auto& b = variants[::acestudio::keypose::best(variants)];
        QString note = tr("%1: body match %2%, identity %3%").arg(qs(pose.key)).arg(qRound(b.anchorMatch * 100)).arg(qRound(b.identity * 100));
        for (const std::string& issue : b.issues) note += QStringLiteral(" · ") + qs(issue);
        m_poseNotes << note;
        chosen.push_back(std::move(b));
    }
    m_aiClip = ::acestudio::keypose::assemble(m_plan, m_sprite, chosen);
    rebuild();
}

std::optional<::acestudio::fx::Effect> SpriteAnimateDialog::effect() const {
    const int e = m_method ? m_method->currentData().toInt() : -1;
    if (e < 0) return std::nullopt;
    return static_cast<::acestudio::fx::Effect>(e / 16);
}

std::vector<::acestudio::motion::Clip> SpriteAnimateDialog::effectClips(const Image& sprite) const {
    ::acestudio::fx::EffectOptions o;
    o.effect = *effect();
    o.fire = static_cast<::acestudio::fx::FireStyle>(m_method->currentData().toInt() % 16);
    o.frames = m_frames->value();   // 0 (Default) = the effect's own loop length
    o.amount = m_amount->value() / 100.0;
    const ::acestudio::fx::EffectClip fxc = ::acestudio::fx::animateEffect(sprite, o);
    ::acestudio::motion::Clip c;
    c.name = ::acestudio::fx::toString(o.effect);
    c.frames = fxc.frames;
    c.durations = fxc.durations;
    c.loops = true;
    if (m_face->currentIndex() == 1)
        for (Image& f : c.frames) {
            Image m(f.width(), f.height());
            for (int y = 0; y < f.height(); ++y)
                for (int x = 0; x < f.width(); ++x) m.setPixel(f.width() - 1 - x, y, f.pixel(x, y));
            f = std::move(m);
        }
    return {std::move(c)};
}

std::vector<::acestudio::motion::Clip> SpriteAnimateDialog::fullClips() {
    if (aiMode() || m_previewScale >= 1.0 || m_clips.empty()) return m_clips;   // AI poses are drawn at full size
    if (effect()) {
        QApplication::setOverrideCursor(Qt::WaitCursor);
        auto clips = effectClips(m_sprite);
        QApplication::restoreOverrideCursor();
        return clips;
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    auto pack = ::acestudio::motion::animatePack(m_sprite, checkedActions(), options(true));
    QApplication::restoreOverrideCursor();
    if (!pack) {
        QMessageBox::warning(this, windowTitle(), qs(pack.error().message));
        return {};
    }
    return std::move(pack).value();
}

void SpriteAnimateDialog::rebuild() {
    if (!m_info || !m_preview || !m_customBox || !m_speed || !m_poseButton) return;   // still being built
    m_poseButton->setVisible(aiMode());
    if (aiMode()) {
        // The wing line and span say which pixels are wings (the rest of the
        // body is protected while the wings are redrawn).
        m_actions->setEnabled(false);
        m_body->setEnabled(false);
        m_customBox->setVisible(false);
        for (QWidget* w : std::initializer_list<QWidget*>{m_wingLine, m_wingLabel, m_wingFrom, m_wingTo, m_spanLabel}) w->setVisible(true);
        m_clips.clear();
        if (m_aiClip) {
            m_clips = {*m_aiClip};
            m_previewClip = 0;
            m_previewFrame = 0;
            m_info->setText(tr("Wing flap from AI key poses, %1 frames, the sprite's own colours; the body is unchanged.").arg(m_aiClip->frames.size()) +
                            QStringLiteral("<br>") + m_poseNotes.join(QStringLiteral("<br>")));
            showFrame();
        } else if (m_poseJobs.empty() || m_poseButton->isEnabled()) {
            m_timer->stop();
            m_preview->clear();
            m_info->setText(tr("FLUX.2 klein draws the wings in three new positions (level, down, rising) around the unchanged body. "
                               "Set the wing line so that only the wings are above it, then Draw the Poses."));
        }
        return;
    }
    const bool fxMode = effect().has_value();
    m_actions->setEnabled(!fxMode);
    m_body->setEnabled(!fxMode);
    if (fxMode) {
        m_customBox->setVisible(false);
        for (QWidget* w : std::initializer_list<QWidget*>{m_wingLine, m_wingLabel, m_wingFrom, m_wingTo, m_spanLabel}) w->setVisible(false);
        m_clips = effectClips(m_previewScale < 1.0 ? m_small : m_sprite);
        m_previewClip = 0;
        m_previewFrame = 0;
        const ::acestudio::motion::ClipCheck chk = ::acestudio::motion::checkClip(m_clips.front());
        QString text = tr("%1 frames of %2 × %3 px, seamless loop, the sprite's own colours.")
                           .arg(m_clips.front().frames.size())
                           .arg(std::lround(m_clips.front().frames.front().width() / m_previewScale))
                           .arg(std::lround(m_clips.front().frames.front().height() / m_previewScale));
        if (m_previewScale < 1.0) text += tr(" (previewed at reduced size)");
        for (const std::string& issue : chk.issues) text += QStringLiteral("<br>") + qs(issue);
        m_info->setText(text);
        showFrame();
        return;
    }
    const auto list = checkedActions();
    m_customBox->setVisible(std::find(list.begin(), list.end(), ::acestudio::motion::Action::Custom) != list.end());
    const bool flap = std::find(list.begin(), list.end(), ::acestudio::motion::Action::Flap) != list.end();
    m_wingLine->setVisible(flap);
    m_wingLabel->setVisible(flap);
    m_wingFrom->setVisible(flap);
    m_wingTo->setVisible(flap);
    m_spanLabel->setVisible(flap);
    m_clips.clear();
    if (list.empty()) {
        m_timer->stop();
        m_preview->clear();
        m_info->setText(tr("Tick one or more actions."));
        return;
    }
    auto pack = ::acestudio::motion::animatePack(m_previewScale < 1.0 ? m_small : m_sprite, list, options(false));
    if (!pack) {
        m_timer->stop();
        m_preview->clear();
        m_info->setText(qs(pack.error().message));
        return;
    }
    m_clips = std::move(pack).value();
    m_previewClip = std::clamp(m_previewClip, 0, static_cast<int>(m_clips.size()) - 1);
    m_previewFrame = 0;
    int frames = 0;
    QStringList problems;
    for (const ::acestudio::motion::Clip& c : m_clips) {
        frames += static_cast<int>(c.frames.size());
        for (const std::string& issue : ::acestudio::motion::checkClip(c).issues) problems << tr("%1: %2").arg(qs(c.name), qs(issue));
    }
    QString text = tr("%n action(s), %1 frames of %2 × %3 px.", nullptr, static_cast<int>(m_clips.size()))
                       .arg(frames)
                       .arg(std::lround(m_clips.front().frames.front().width() / m_previewScale))
                       .arg(std::lround(m_clips.front().frames.front().height() / m_previewScale));
    if (m_previewScale < 1.0) text += tr(" (previewed at reduced size)");
    text += problems.isEmpty() ? tr(" Checks passed.") : QStringLiteral("<br>") + problems.join(QStringLiteral("<br>"));
    m_info->setText(text);
    showFrame();
}

void SpriteAnimateDialog::showFrame() {
    if (m_clips.empty()) return;
    const ::acestudio::motion::Clip& c = m_clips[static_cast<std::size_t>(m_previewClip)];
    if (m_previewFrame >= static_cast<int>(c.frames.size())) m_previewFrame = 0;
    const Image& f = c.frames[static_cast<std::size_t>(m_previewFrame)];
    // Whole-number zoom for pixel art; a frame larger than the preview is
    // shrunk to fit (found on the test PC with a 481 px knight).
    const int scale = std::min(m_preview->width() / std::max(1, f.width()), m_preview->height() / std::max(1, f.height()));
    const QImage q = toQImage(f);
    m_preview->setPixmap(QPixmap::fromImage(scale >= 1 ? q.scaled(f.width() * scale, f.height() * scale, Qt::KeepAspectRatio, Qt::FastTransformation)
                                                       : q.scaled(m_preview->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    const int ms = std::max(10, c.durations[static_cast<std::size_t>(m_previewFrame)] * 100 / std::max(1, m_speed->value()));
    // A non-looping action holds its last frame for a moment before replaying.
    const bool last = m_previewFrame + 1 == static_cast<int>(c.frames.size());
    m_timer->start(last && !c.loops ? std::max(ms, 600) : ms);
    ++m_previewFrame;
}

void SpriteAnimateDialog::openDocument() {
    const std::vector<::acestudio::motion::Clip> clips = fullClips();
    if (clips.empty()) return;
    std::vector<Image> frames;
    std::vector<int> durations;
    std::vector<AnimationTag> tags;
    for (const ::acestudio::motion::Clip& c : clips) {
        AnimationTag t;
        t.name = c.name;
        t.fromFrame = static_cast<int>(frames.size());
        frames.insert(frames.end(), c.frames.begin(), c.frames.end());
        durations.insert(durations.end(), c.durations.begin(), c.durations.end());
        t.toFrame = static_cast<int>(frames.size()) - 1;
        t.repeat = c.loops ? 0 : 1;
        tags.push_back(std::move(t));
    }
    m_open(frames, durations, tags, m_name);
    accept();
}

void SpriteAnimateDialog::exportStrips() {
    if (m_clips.empty()) return;
    // Starts in the last folder used this session, else the home folder (not
    // the working directory, which is the install folder when started from
    // the Start menu).
    static QString lastDir;
    const QString start = !lastDir.isEmpty() && QDir(lastDir).exists() ? lastDir : QDir::homePath();
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Export Animation Strips"), start);
    if (dir.isEmpty()) return;
    lastDir = dir;
    const std::vector<::acestudio::motion::Clip> clips = fullClips();
    QStringList written;
    for (const ::acestudio::motion::Clip& c : clips) {
        const int fw = c.frames[0].width(), fh = c.frames[0].height();
        Image strip(fw * static_cast<int>(c.frames.size()), fh);
        for (std::size_t i = 0; i < c.frames.size(); ++i) strip.blit(c.frames[i], static_cast<int>(i) * fw, 0);
        const QString base = (m_name.isEmpty() ? QStringLiteral("sprite") : m_name) + QLatin1Char('_') + qs(c.name) + QStringLiteral(".png");
        const auto path = toPath(dir) / toPath(base);
        std::error_code ec;
        if (std::filesystem::exists(path, ec) &&
            QMessageBox::question(this, tr("Export Animation Strips"), tr("%1 exists. Replace it?").arg(base)) != QMessageBox::Yes)
            continue;
        if (auto st = writePng(strip, path); !st) {
            QMessageBox::warning(this, tr("Export Animation Strips"), qs(st.error().message));
            return;
        }
        written << base;
    }
    m_info->setText(tr("Wrote %1 to %2.").arg(written.join(QStringLiteral(", ")), dir));
}

} // namespace acestudio::ui
