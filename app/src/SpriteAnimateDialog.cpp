#include "SpriteAnimateDialog.h"

#include "QtBridge.h"

#include "acestudio/core/PngCodec.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
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

namespace acestudio::ui {

namespace {
const char* bodyLabel(::acestudio::motion::Body b) {
    switch (b) {
    case ::acestudio::motion::Body::Biped: return QT_TRANSLATE_NOOP("SpriteAnimateDialog", "Two legs");
    case ::acestudio::motion::Body::Quadruped: return QT_TRANSLATE_NOOP("SpriteAnimateDialog", "Four legs");
    case ::acestudio::motion::Body::Blob: return QT_TRANSLATE_NOOP("SpriteAnimateDialog", "Blob (hops)");
    case ::acestudio::motion::Body::Flyer: return QT_TRANSLATE_NOOP("SpriteAnimateDialog", "Flyer (hovers)");
    case ::acestudio::motion::Body::Object: return QT_TRANSLATE_NOOP("SpriteAnimateDialog", "Object (rocks)");
    }
    return "";
}
} // namespace

SpriteAnimateDialog::SpriteAnimateDialog(const Image& sprite, const QString& name, OpenFn open, QWidget* parent, const Mask& wingMask)
    : QDialog(parent), m_sprite(sprite), m_wingMask(wingMask), m_open(std::move(open)) {
    m_name = name;
    setWindowTitle(tr("Animate Sprite"));
    m_subject = ::acestudio::motion::analyzeSubject(m_sprite);

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
    m_body = new QComboBox(this);
    m_body->addItem(tr("Automatic (%1)").arg(tr(bodyLabel(m_subject.body))), QString());
    for (::acestudio::motion::Body b : {::acestudio::motion::Body::Biped, ::acestudio::motion::Body::Quadruped, ::acestudio::motion::Body::Blob, ::acestudio::motion::Body::Flyer, ::acestudio::motion::Body::Object})
        m_body->addItem(tr(bodyLabel(b)), QString::fromLatin1(::acestudio::motion::toString(b)));
    m_body->setToolTip(tr("How the sprite moves. Automatic is measured from its silhouette: separate feet walk, "
                          "a round shape hops, anything else rocks."));
    form->addRow(tr("&Body:"), m_body);
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
    connect(m_amount, &QSlider::valueChanged, this, &SpriteAnimateDialog::rebuild);
    connect(m_frames, &QSpinBox::valueChanged, this, &SpriteAnimateDialog::rebuild);
    rebuild();
}

std::vector<::acestudio::motion::Action> SpriteAnimateDialog::checkedActions() const {
    std::vector<::acestudio::motion::Action> out;
    for (int i = 0; i < m_actions->count(); ++i)
        if (m_actions->item(i)->checkState() == Qt::Checked)
            out.push_back(static_cast<::acestudio::motion::Action>(m_actions->item(i)->data(Qt::UserRole).toInt()));
    return out;
}

::acestudio::motion::Options SpriteAnimateDialog::options() const {
    ::acestudio::motion::Options o;
    o.frames = m_frames->value();
    o.amount = m_amount->value() / 100.0;
    const QString body = m_body->currentData().toString();
    if (!body.isEmpty()) o.body = ::acestudio::motion::bodyFromString(body.toStdString());
    o.faceLeft = m_face->currentIndex() == 1;
    o.custom = {m_bob->value(), m_sway->value(), m_squash->value(), m_tilt->value(), m_legSwing->value()};
    o.wingLine = m_wingLine->value() / 100.0;
    o.wingFrom = m_wingFrom->value() / 100.0;
    o.wingTo = m_wingTo->value() / 100.0;
    o.wingMask = m_wingMask;
    return o;
}

void SpriteAnimateDialog::rebuild() {
    if (!m_info || !m_preview || !m_customBox) return;   // still being built
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
    auto pack = ::acestudio::motion::animatePack(m_sprite, list, options());
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
                       .arg(m_clips.front().frames.front().width())
                       .arg(m_clips.front().frames.front().height());
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
    const int ms = c.durations[static_cast<std::size_t>(m_previewFrame)];
    // A non-looping action holds its last frame for a moment before replaying.
    const bool last = m_previewFrame + 1 == static_cast<int>(c.frames.size());
    m_timer->start(last && !c.loops ? std::max(ms, 600) : ms);
    ++m_previewFrame;
}

void SpriteAnimateDialog::openDocument() {
    if (m_clips.empty()) return;
    std::vector<Image> frames;
    std::vector<int> durations;
    std::vector<AnimationTag> tags;
    for (const ::acestudio::motion::Clip& c : m_clips) {
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
    QStringList written;
    for (const ::acestudio::motion::Clip& c : m_clips) {
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
