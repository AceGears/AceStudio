#include "MotionStudioPanel.h"

#include "QtBridge.h"

#include "acestudio/core/MotionLibrary.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHideEvent>
#include <QShowEvent>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace acestudio::ui {

namespace mc = ::acestudio::mocap;

// ---- SkeletonView ------------------------------------------------------------------

SkeletonView::SkeletonView(QWidget* parent) : QWidget(parent) {
    setMinimumSize(220, 220);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(tr("3D source motion"));
    setToolTip(tr("Drag to turn and tilt the camera, wheel to zoom. Arrow keys turn and tilt too."));
}

void SkeletonView::setMotion(const mc::Motion* m, const mc::BoneMap* map) {
    m_motion = m;
    m_map = map;
    update();
}

void SkeletonView::setCamera(const mc::Camera& c) {
    m_camera = c;
    update();
}

void SkeletonView::setTime(double seconds, bool inPlace) {
    m_time = seconds;
    m_inPlace = inPlace;
    update();
}

void SkeletonView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor("#16161C"));
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QColor("#8A8A96"));
    p.drawText(rect().adjusted(8, 6, -8, -6), Qt::AlignTop | Qt::AlignLeft, tr("3D: drag to turn, wheel to zoom"));
    if (!m_motion || !m_map || m_motion->joints.empty()) {
        p.drawText(rect(), Qt::AlignCenter, tr("Load a motion (BVH, glTF or GLB)"));
        return;
    }
    const mc::Motion& m = *m_motion;
    const mc::BoneMap& map = *m_map;
    const std::vector<mc::Vec3> rest = mc::restPositions(m);
    double minY = 0, maxY = 0;
    for (const mc::Vec3& v : rest) { minY = std::min(minY, v.y); maxY = std::max(maxY, v.y); }
    const double figH = std::max(1e-6, maxY - minY);
    const int hips = map[mc::Bone::Hips] >= 0 ? map[mc::Bone::Hips] : 0;
    const std::vector<mc::Vec3> p0 = mc::jointPositions(m, 0);
    std::vector<mc::Vec3> w = mc::jointPositions(m, m_time);
    if (m_inPlace) {
        const mc::Vec3 d{w[static_cast<std::size_t>(hips)].x - p0[static_cast<std::size_t>(hips)].x, 0,
                         w[static_cast<std::size_t>(hips)].z - p0[static_cast<std::size_t>(hips)].z};
        for (mc::Vec3& v : w) v = {v.x - d.x, v.y, v.z - d.z};
    }
    double floorY = 1e300;
    for (const mc::Vec3& v : w) floorY = std::min(floorY, v.y);
    const mc::Vec3 centre{p0[static_cast<std::size_t>(hips)].x, floorY, p0[static_cast<std::size_t>(hips)].z};
    const mc::Vec3 fwd = mc::characterForward(m, map);
    const double scale = std::min(width(), height()) * 0.62 * m_zoom;
    const double ox = width() / 2.0, oy = height() * 0.82;

    // Floor grid, one character height each way.
    std::vector<mc::Vec3> grid;
    for (int i = -4; i <= 4; ++i) {
        const double a = i * figH / 4;
        grid.push_back({centre.x + a, floorY, centre.z - figH});
        grid.push_back({centre.x + a, floorY, centre.z + figH});
        grid.push_back({centre.x - figH, floorY, centre.z + a});
        grid.push_back({centre.x + figH, floorY, centre.z + a});
    }
    const mc::Projected gp = mc::project(grid, m_camera, fwd, figH, centre);
    p.setPen(QPen(QColor("#2E2E38"), 1));
    for (std::size_t i = 0; i + 1 < grid.size(); i += 2)
        p.drawLine(QPointF(ox + gp.x[i] * scale, oy + gp.y[i] * scale), QPointF(ox + gp.x[i + 1] * scale, oy + gp.y[i + 1] * scale));

    const mc::Projected pr = mc::project(w, m_camera, fwd, figH, centre);
    auto colourOf = [&](int j) {
        for (mc::Bone b : {mc::Bone::LShoulder, mc::Bone::LUpperArm, mc::Bone::LForearm, mc::Bone::LHand, mc::Bone::LThigh, mc::Bone::LShin,
                           mc::Bone::LFoot, mc::Bone::LToe})
            if (map[b] == j) return QColor("#4C8DFF");
        for (mc::Bone b : {mc::Bone::RShoulder, mc::Bone::RUpperArm, mc::Bone::RForearm, mc::Bone::RHand, mc::Bone::RThigh, mc::Bone::RShin,
                           mc::Bone::RFoot, mc::Bone::RToe})
            if (map[b] == j) return QColor("#FF5A4A");
        for (mc::Bone b : {mc::Bone::Hips, mc::Bone::Spine, mc::Bone::Chest, mc::Bone::Neck, mc::Bone::Head})
            if (map[b] == j) return QColor("#F2CF4A");
        return QColor("#8A8A96");
    };
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < m.joints.size(); ++i)
        if (m.joints[i].parent >= 0) order.push_back(i);
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return pr.depth[a] < pr.depth[b]; });
    for (std::size_t i : order) {
        const auto pi = static_cast<std::size_t>(m.joints[i].parent);
        p.setPen(QPen(colourOf(m.joints[i].parent), 2.5, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(ox + pr.x[pi] * scale, oy + pr.y[pi] * scale), QPointF(ox + pr.x[i] * scale, oy + pr.y[i] * scale));
    }
    p.setPen(Qt::NoPen);
    p.setBrush(QColor("#E8E8EE"));
    for (std::size_t i = 0; i < m.joints.size(); ++i)
        if (!m.joints[i].endSite) p.drawEllipse(QPointF(ox + pr.x[i] * scale, oy + pr.y[i] * scale), 2.2, 2.2);
}

void SkeletonView::mousePressEvent(QMouseEvent* e) {
    m_dragFrom = e->position().toPoint();
    m_dragTurn = m_camera.turn;
    m_dragTilt = m_camera.tilt;
}

void SkeletonView::mouseMoveEvent(QMouseEvent* e) {
    if (!(e->buttons() & Qt::LeftButton)) return;
    const QPoint d = e->position().toPoint() - m_dragFrom;
    double turn = m_dragTurn - d.x() * 0.6;
    while (turn > 180) turn -= 360;
    while (turn < -180) turn += 360;
    const double tilt = std::clamp(m_dragTilt + d.y() * 0.4, -90.0, 90.0);
    Q_EMIT cameraDragged(turn, tilt);
}

void SkeletonView::wheelEvent(QWheelEvent* e) {
    m_zoom = std::clamp(m_zoom * (e->angleDelta().y() > 0 ? 1.1 : 1 / 1.1), 0.3, 4.0);
    update();
}

void SkeletonView::keyPressEvent(QKeyEvent* e) {
    double turn = m_camera.turn, tilt = m_camera.tilt;
    switch (e->key()) {
    case Qt::Key_Left: turn += 5; break;
    case Qt::Key_Right: turn -= 5; break;
    case Qt::Key_Up: tilt = std::min(90.0, tilt + 5); break;
    case Qt::Key_Down: tilt = std::max(-90.0, tilt - 5); break;
    default: QWidget::keyPressEvent(e); return;
    }
    while (turn > 180) turn -= 360;
    while (turn < -180) turn += 360;
    Q_EMIT cameraDragged(turn, tilt);
}

// ---- MotionStudioPanel ----------------------------------------------------------------

namespace {

QSlider* angleSlider(QWidget* parent, int lo, int hi, const QString& name) {
    auto* s = new QSlider(Qt::Horizontal, parent);
    s->setRange(lo, hi);
    s->setAccessibleName(name);
    s->setMinimumWidth(180);
    return s;
}

} // namespace

MotionStudioPanel::MotionStudioPanel(OpenFn open, QWidget* parent) : QWidget(parent), m_open(std::move(open)) {
    auto* root = new QVBoxLayout(this);

    // The sprite the motion goes on.
    auto* spriteRow = new QHBoxLayout;
    m_spriteLabel = new QLabel(tr("No sprite: open one and press Use Current Sprite"), this);
    m_spriteLabel->setWordWrap(true);
    auto* useSprite = new QPushButton(tr("Use Current Sprite"), this);
    useSprite->setToolTip(tr("Put the motion on the current frame of the open sprite"));
    spriteRow->addWidget(m_spriteLabel, 1);
    spriteRow->addWidget(useSprite);
    root->addLayout(spriteRow);

    // Motion and camera controls.
    auto* top = new QHBoxLayout;
    root->addLayout(top);
    auto* motionBox = new QGroupBox(tr("Motion"), this);
    auto* mf = new QFormLayout(motionBox);
    auto* load = new QPushButton(tr("Load Motion…"), motionBox);
    load->setAutoDefault(false);
    load->setToolTip(tr("A BVH motion (Blender, CMU mocap, Rokoko) or a glTF/GLB animation (Mixamo via Blender, Unity, Unreal, most 3D tools). "
                        "FBX: import it into Blender and export glTF first."));
    m_motionLabel = new QLabel(tr("No motion loaded"), motionBox);
    m_motionLabel->setWordWrap(true);
    mf->addRow(load, m_motionLabel);
    // The motion library: built-in presets and the user's saved motions.
    auto* libRow = new QHBoxLayout;
    m_presets = new QComboBox(motionBox);
    m_presets->setAccessibleName(tr("Motion presets"));
    m_presets->setToolTip(tr("Ready motions (idle, walk, run, jump, attack, hurt; flap for wings; quad-walk for four legs) and the motions you saved"));
    m_savePreset = new QPushButton(tr("Save as Preset…"), motionBox);
    m_savePreset->setAutoDefault(false);
    m_savePreset->setToolTip(tr("Keep the loaded motion in your motion library (saved as BVH)"));
    m_savePreset->setEnabled(false);
    m_removePreset = new QPushButton(tr("Remove"), motionBox);
    m_removePreset->setAutoDefault(false);
    m_removePreset->setToolTip(tr("Delete the selected saved preset (built-in presets stay)"));
    m_removePreset->setEnabled(false);
    libRow->addWidget(m_presets, 1);
    libRow->addWidget(m_savePreset);
    libRow->addWidget(m_removePreset);
    mf->addRow(tr("Library:"), libRow);
    m_start = new QDoubleSpinBox(motionBox);
    m_start->setRange(0, 3600);
    m_start->setDecimals(2);
    m_start->setSuffix(tr(" s"));
    m_start->setAccessibleName(tr("Start time"));
    m_length = new QDoubleSpinBox(motionBox);
    m_length->setRange(0, 3600);
    m_length->setDecimals(2);
    m_length->setSuffix(tr(" s"));
    m_length->setSpecialValueText(tr("To the end"));
    m_length->setAccessibleName(tr("Length"));
    m_fps = new QSpinBox(motionBox);
    m_fps->setRange(1, 60);
    m_fps->setValue(12);
    m_fps->setSuffix(tr(" fps"));
    m_fps->setAccessibleName(tr("Frames per second"));
    m_amount = new QDoubleSpinBox(motionBox);
    m_amount->setRange(0.25, 2.0);
    m_amount->setSingleStep(0.05);
    m_amount->setValue(1.0);
    m_amount->setAccessibleName(tr("Motion amount"));
    m_inPlace = new QCheckBox(tr("In place (no travel)"), motionBox);
    m_inPlace->setChecked(true);
    // What the motion drives: read from the sprite, or chosen. Wings are
    // never read from the sprite; a winged side view takes the motion's arms.
    m_body = new QComboBox(motionBox);
    m_body->addItem(tr("From the sprite"), QStringLiteral("auto"));
    m_body->addItem(tr("Two legs"), QStringLiteral("biped"));
    m_body->addItem(tr("Four legs"), QStringLiteral("quadruped"));
    m_body->addItem(tr("Wings (the motion's arms)"), QStringLiteral("flyer"));
    m_body->setAccessibleName(tr("Sprite body"));
    m_body->setToolTip(tr("What the motion moves. Wings: each wing turns as the motion's arm rises and falls "
                          "(try the flap preset). The wings are the part above 45% of the sprite's height."));
    mf->addRow(tr("Start:"), m_start);
    mf->addRow(tr("Length:"), m_length);
    mf->addRow(tr("Frame rate:"), m_fps);
    mf->addRow(tr("Amount:"), m_amount);
    mf->addRow(QString(), m_inPlace);
    mf->addRow(tr("Body:"), m_body);
    top->addWidget(motionBox, 1);

    auto* camBox = new QGroupBox(tr("Camera"), this);
    auto* cf = new QFormLayout(camBox);
    m_view = new QComboBox(camBox);
    m_view->setAccessibleName(tr("View"));
    m_view->addItem(tr("As drawn (auto)"), QString());
    const QStringList labels{tr("Front"), tr("Back"), tr("Right"), tr("Left"), tr("Front-right"), tr("Front-left"), tr("Back-right"),
                             tr("Back-left"), tr("Three-quarter"), tr("Top-down"), tr("Isometric")};
    const std::vector<std::string> keys = mc::cameraPresetNames();
    for (std::size_t i = 0; i < keys.size() && i < static_cast<std::size_t>(labels.size()); ++i)
        m_view->addItem(labels[static_cast<int>(i)], QString::fromStdString(keys[i]));
    m_view->addItem(tr("Custom"), QStringLiteral("custom"));
    cf->addRow(tr("View:"), m_view);
    auto angleRow = [&](QSlider*& s, QLabel*& value, int lo, int hi, const QString& label, const QString& accessible) {
        s = angleSlider(camBox, lo, hi, accessible);
        value = new QLabel(camBox);
        value->setMinimumWidth(56);
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        auto* row = new QHBoxLayout;
        row->addWidget(s, 1);
        row->addWidget(value);
        auto* l = new QLabel(label, camBox);
        l->setBuddy(s);
        cf->addRow(l, row);
    };
    angleRow(m_turn, m_turnValue, -180, 180, tr("Turn:"), tr("Camera turn"));
    angleRow(m_tilt, m_tiltValue, -90, 90, tr("Tilt:"), tr("Camera tilt"));
    angleRow(m_roll, m_rollValue, -180, 180, tr("Roll:"), tr("Camera roll"));
    m_perspective = new QCheckBox(tr("Perspective"), camBox);
    cf->addRow(QString(), m_perspective);
    top->addWidget(camBox, 1);

    // Source and result, side by side.
    auto* split = new QSplitter(Qt::Horizontal, this);
    m_source = new SkeletonView(split);
    auto* rightPane = new QWidget(split);
    auto* rl = new QVBoxLayout(rightPane);
    rl->setContentsMargins(0, 0, 0, 0);
    auto* yourRig = new QLabel(tr("Your sprite"), rightPane);
    m_preview = new QLabel(rightPane);
    m_preview->setMinimumSize(220, 220);
    m_preview->setAlignment(Qt::AlignCenter);
    m_preview->setAccessibleName(tr("Retargeted sprite preview"));
    m_preview->setStyleSheet(QStringLiteral("background:#22222A;"));
    rl->addWidget(yourRig);
    rl->addWidget(m_preview, 1);
    split->addWidget(m_source);
    split->addWidget(rightPane);
    root->addWidget(split, 1);

    auto* transport = new QHBoxLayout;
    m_play = new QPushButton(tr("Pause"), this);
    m_play->setAutoDefault(false);
    m_scrub = new QSlider(Qt::Horizontal, this);
    m_scrub->setAccessibleName(tr("Frame"));
    transport->addWidget(m_play);
    transport->addWidget(m_scrub, 1);
    root->addLayout(transport);

    auto* bottom = new QHBoxLayout;
    m_bones = new QTreeWidget(this);
    m_bones->setColumnCount(2);
    m_bones->setHeaderLabels({tr("Bone"), tr("Motion joint")});
    m_bones->setRootIsDecorated(false);
    m_bones->setAccessibleName(tr("Bone mapping"));
    m_bones->setMaximumHeight(140);
    m_bones->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_notes = new QLabel(this);
    m_notes->setWordWrap(true);
    m_notes->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_notes->setTextInteractionFlags(Qt::TextSelectableByMouse);
    bottom->addWidget(m_bones, 1);
    bottom->addWidget(m_notes, 1);
    root->addLayout(bottom);

    auto* buttons = new QHBoxLayout;
    m_openButton = new QPushButton(tr("Open as Document"), this);
    m_openButton->setToolTip(tr("Open the posed frames as a new animation document"));
    m_openButton->setEnabled(false);
    buttons->addStretch(1);
    buttons->addWidget(m_openButton);
    root->addLayout(buttons);

    m_timer = new QTimer(this);
    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(120);

    connect(load, &QPushButton::clicked, this, &MotionStudioPanel::loadMotion);
    connect(m_presets, &QComboBox::currentIndexChanged, this, [this] { libraryChosen(); });
    connect(m_savePreset, &QPushButton::clicked, this, &MotionStudioPanel::saveAsPreset);
    connect(m_removePreset, &QPushButton::clicked, this, &MotionStudioPanel::removeSavedPreset);
    refreshPresets();
    connect(m_view, &QComboBox::currentIndexChanged, this, [this] { presetChosen(); });
    for (QSlider* s : {m_turn, m_tilt, m_roll}) connect(s, &QSlider::valueChanged, this, [this] { cameraEdited(); });
    connect(m_perspective, &QCheckBox::toggled, this, [this] { cameraEdited(); });
    for (QDoubleSpinBox* s : {m_start, m_length, m_amount}) connect(s, &QDoubleSpinBox::valueChanged, this, [this] { scheduleRetarget(); });
    connect(m_body, &QComboBox::currentIndexChanged, this, [this] { scheduleRetarget(); });
    connect(m_fps, &QSpinBox::valueChanged, this, [this] { scheduleRetarget(); });
    connect(m_inPlace, &QCheckBox::toggled, this, [this] { scheduleRetarget(); });
    connect(m_source, &SkeletonView::cameraDragged, this, [this](double turn, double tilt) {
        m_syncing = true;
        m_view->setCurrentIndex(m_view->findData(QStringLiteral("custom")));
        m_turn->setValue(static_cast<int>(std::lround(turn)));
        m_tilt->setValue(static_cast<int>(std::lround(tilt)));
        m_syncing = false;
        cameraEdited();
    });
    connect(m_debounce, &QTimer::timeout, this, &MotionStudioPanel::retarget);
    connect(m_timer, &QTimer::timeout, this, &MotionStudioPanel::tick);
    connect(m_play, &QPushButton::clicked, this, [this] {
        if (m_timer->isActive()) {
            m_timer->stop();
            m_play->setText(tr("Play"));
        } else {
            m_timer->start();
            m_play->setText(tr("Pause"));
        }
    });
    connect(m_scrub, &QSlider::valueChanged, this, [this](int v) {
        m_frame = v;
        showFrame();
    });
    connect(m_openButton, &QPushButton::clicked, this, &MotionStudioPanel::openDocument);
    connect(useSprite, &QPushButton::clicked, this, &MotionStudioPanel::currentSpriteRequested);

    presetChosen();
}

void MotionStudioPanel::setSprite(const Image& sprite, const QString& name) {
    m_sprite = sprite;
    m_name = name;
    m_fourLegged = !m_sprite.isNull() && ::acestudio::motion::analyzeSubject(m_sprite).body == ::acestudio::motion::Body::Quadruped;
    m_spriteLabel->setText(tr("Sprite: <b>%1</b> (%2 x %3)%4")
                               .arg(name.toHtmlEscaped())
                               .arg(m_sprite.width())
                               .arg(m_sprite.height())
                               .arg(m_fourLegged ? tr(", four-legged") : QString()));
    // "As drawn" follows the new sprite's facing; other views keep the camera.
    if (m_view->currentData().toString().isEmpty()) presetChosen();
    else scheduleRetarget();
}

void MotionStudioPanel::hideEvent(QHideEvent* e) {
    // A closed panel does not keep playing.
    m_timer->stop();
    QWidget::hideEvent(e);
}

void MotionStudioPanel::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    if (m_result && !m_result->clip.frames.empty() && m_play->text() == tr("Pause")) m_timer->start();
}

MotionStudioPanel::~MotionStudioPanel() = default;

mc::Camera MotionStudioPanel::camera() const {
    mc::Camera c;
    c.turn = m_turn->value();
    c.tilt = m_tilt->value();
    c.roll = m_roll->value();
    c.perspective = m_perspective->isChecked();
    return c;
}

void MotionStudioPanel::presetChosen() {
    if (m_syncing) return;
    const QString key = m_view->currentData().toString();
    mc::Camera c = camera();
    if (key.isEmpty()) {
        // As drawn: the side the sprite faces, or the front.
        c = mc::Camera{};
        if (hasSprite()) {
            const ::acestudio::motion::Subject s = ::acestudio::motion::analyzeSubject(m_sprite);
            c.turn = s.frontView ? 0 : (s.facing < 0 ? -90 : 90);
        }
    } else if (key != QStringLiteral("custom")) {
        if (auto p = mc::cameraPreset(key.toStdString())) c = *p;
    }
    m_syncing = true;
    m_turn->setValue(static_cast<int>(std::lround(c.turn)));
    m_tilt->setValue(static_cast<int>(std::lround(c.tilt)));
    m_roll->setValue(static_cast<int>(std::lround(c.roll)));
    m_perspective->setChecked(c.perspective);
    m_syncing = false;
    cameraEdited();
}

void MotionStudioPanel::cameraEdited() {
    if (m_syncing) return;
    m_turnValue->setText(tr("%1 deg").arg(m_turn->value()));
    m_tiltValue->setText(tr("%1 deg").arg(m_tilt->value()));
    m_rollValue->setText(tr("%1 deg").arg(m_roll->value()));
    m_source->setCamera(camera());
    scheduleRetarget();
}

void MotionStudioPanel::scheduleRetarget() {
    if (m_motion) m_debounce->start();
}

void MotionStudioPanel::loadMotion() {
    const QString path = QFileDialog::getOpenFileName(this, tr("Load Motion"), QString(), tr("Motion (*.bvh *.glb *.gltf);;BVH motion (*.bvh);;glTF animation (*.glb *.gltf);;All files (*)"));
    if (!path.isEmpty()) loadMotionFile(path);
}

void MotionStudioPanel::loadMotionFile(const QString& path) {
    const std::filesystem::path file = path.toStdWString();
    // A glTF file can hold several animations: let the user pick one.
    mc::GltfOptions gopt;
    QString animName;
    auto names = mc::motionAnimationNames(file);
    if (!names) {
        QMessageBox::warning(this, tr("Load Motion"), tr("This motion could not be read:\n%1").arg(QString::fromStdString(names.error().message)));
        return;
    }
    if (names.value().size() > 1) {
        QStringList items;
        for (std::size_t i = 0; i < names.value().size(); ++i)
            items << QStringLiteral("%1: %2").arg(i).arg(QString::fromStdString(names.value()[i]));
        bool ok = false;
        const QString chosen = QInputDialog::getItem(this, tr("Load Motion"), tr("This file holds several animations. Which one?"), items, 0, false, &ok);
        if (!ok) return;
        const qsizetype index = items.indexOf(chosen);
        if (index < 0) return;
        gopt.animationIndex = static_cast<int>(index);
        animName = QString::fromStdString(names.value()[static_cast<std::size_t>(index)]);
    }
    auto m = mc::loadMotion(file, gopt);
    if (!m) {
        QMessageBox::warning(this, tr("Load Motion"), tr("This motion could not be read:\n%1").arg(QString::fromStdString(m.error().message)));
        return;
    }
    QString motionName = QFileInfo(path).completeBaseName();
    if (!animName.isEmpty()) motionName += QStringLiteral(" (%1)").arg(animName);
    {
        const QSignalBlocker b(m_presets);
        m_presets->setCurrentIndex(0);
    }
    useMotion(std::move(m).value(), motionName, false);
}

void MotionStudioPanel::refreshPresets(const QString& select) {
    const QSignalBlocker b(m_presets);
    m_presets->clear();
    m_presets->addItem(tr("Choose a preset…"), QString());
    for (const mc::PresetInfo& p : mc::builtInPresets())
        m_presets->addItem(tr("%1 (built in)").arg(QString::fromStdString(p.name)), QStringLiteral("b:") + QString::fromStdString(p.name));
    if (auto dir = mc::defaultPresetDir()) {
        if (auto saved = mc::savedPresets(dir.value())) {
            for (const mc::PresetInfo& p : saved.value()) {
                if (!p.error.empty()) continue;   // unreadable files are not offered
                m_presets->addItem(QString::fromStdString(p.name), QStringLiteral("s:") + QString::fromStdString(p.name));
            }
        }
    }
    const int at = select.isEmpty() ? 0 : m_presets->findData(select);
    m_presets->setCurrentIndex(std::max(0, at));
    m_removePreset->setEnabled(m_presets->currentData().toString().startsWith(QStringLiteral("s:")));
}

void MotionStudioPanel::libraryChosen() {
    const QString key = m_presets->currentData().toString();
    m_removePreset->setEnabled(key.startsWith(QStringLiteral("s:")));
    if (key.isEmpty()) return;
    const std::string name = key.mid(2).toStdString();
    auto dir = mc::defaultPresetDir();
    if (!dir) {
        QMessageBox::warning(this, tr("Motion Presets"), QString::fromStdString(dir.error().message));
        return;
    }
    auto m = mc::loadPreset(name, dir.value());
    if (!m) {
        QMessageBox::warning(this, tr("Motion Presets"), tr("This preset could not be loaded:\n%1").arg(QString::fromStdString(m.error().message)));
        return;
    }
    bool oneShot = false;
    for (const mc::PresetInfo& p : mc::builtInPresets())
        if (key.startsWith(QStringLiteral("b:")) && p.name == name) oneShot = !p.loops;
    useMotion(std::move(m).value(), QString::fromStdString(name), oneShot);
}

void MotionStudioPanel::saveAsPreset() {
    if (!m_motion) return;
    auto dir = mc::defaultPresetDir();
    if (!dir) {
        QMessageBox::warning(this, tr("Save as Preset"), QString::fromStdString(dir.error().message));
        return;
    }
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Save as Preset"), tr("Preset name (letters, digits, spaces, - and _):"), QLineEdit::Normal,
                                               m_motionName, &ok)
                             .trimmed();
    if (!ok || name.isEmpty()) return;
    if (auto st = mc::checkPresetName(name.toStdString()); !st) {
        QMessageBox::warning(this, tr("Save as Preset"), QString::fromStdString(st.error().message));
        return;
    }
    const std::filesystem::path file = dir.value() / (name.toStdString() + ".bvh");
    std::error_code ec;
    bool overwrite = false;
    if (std::filesystem::exists(file, ec)) {
        if (QMessageBox::question(this, tr("Save as Preset"), tr("A preset named “%1” is already saved. Replace it?").arg(name)) != QMessageBox::Yes)
            return;
        overwrite = true;
    }
    if (auto st = mc::savePreset(dir.value(), name.toStdString(), *m_motion, overwrite); !st) {
        QMessageBox::warning(this, tr("Save as Preset"), tr("The preset could not be saved:\n%1").arg(QString::fromStdString(st.error().message)));
        return;
    }
    refreshPresets(QStringLiteral("s:") + name);
    m_motionName = name;
}

void MotionStudioPanel::removeSavedPreset() {
    const QString key = m_presets->currentData().toString();
    if (!key.startsWith(QStringLiteral("s:"))) return;
    const QString name = key.mid(2);
    if (QMessageBox::question(this, tr("Remove Preset"), tr("Delete the saved preset “%1”? This cannot be undone.").arg(name)) != QMessageBox::Yes)
        return;
    auto dir = mc::defaultPresetDir();
    if (!dir) return;
    if (auto st = mc::removePreset(dir.value(), name.toStdString()); !st)
        QMessageBox::warning(this, tr("Remove Preset"), QString::fromStdString(st.error().message));
    refreshPresets();
}

void MotionStudioPanel::useMotion(mc::Motion motion, const QString& name, bool oneShot) {
    m_motion = std::move(motion);
    m_oneShot = oneShot;
    m_map = mc::autoMap(*m_motion);
    m_motionName = name;
    m_savePreset->setEnabled(true);
    m_motionLabel->setText(tr("%1: %2 joints, %3 frames, %4 s")
                               .arg(m_motionName)
                               .arg(m_motion->joints.size())
                               .arg(m_motion->frameCount)
                               .arg(m_motion->duration(), 0, 'f', 2));
    m_start->setMaximum(m_motion->duration());
    m_length->setMaximum(m_motion->duration());
    m_bones->clear();
    for (int b = 0; b < mc::kBoneCount; ++b) {
        const int j = m_map.joint[static_cast<std::size_t>(b)];
        auto* it = new QTreeWidgetItem(m_bones);
        it->setText(0, QString::fromUtf8(mc::toString(static_cast<mc::Bone>(b)).data(),
                                         static_cast<int>(mc::toString(static_cast<mc::Bone>(b)).size())));
        it->setText(1, j >= 0 ? QString::fromStdString(m_motion->joints[static_cast<std::size_t>(j)].name) : tr("(not found)"));
    }
    m_source->setMotion(&*m_motion, &m_map);
    retarget();
}

void MotionStudioPanel::retarget() {
    if (!m_motion || !hasSprite()) return;
    mc::RetargetOptions o;
    o.camera = camera();
    o.start = m_start->value();
    o.length = m_length->value();
    o.fps = m_fps->value();
    o.amount = m_amount->value();
    o.inPlace = m_inPlace->isChecked();
    o.loop = !m_oneShot;   // jump, attack and hurt play once
    const QString body = m_body->currentData().toString();
    if (body == QLatin1String("biped")) o.body = ::acestudio::motion::Body::Biped;
    else if (body == QLatin1String("quadruped")) o.body = ::acestudio::motion::Body::Quadruped;
    else if (body == QLatin1String("flyer")) o.body = ::acestudio::motion::Body::Flyer;
    else o.body = m_fourLegged ? ::acestudio::motion::Body::Quadruped : ::acestudio::motion::Body::Biped;
    auto r = mc::retarget(m_sprite, *m_motion, o);
    if (!r) {
        m_result.reset();
        m_openButton->setEnabled(false);
        m_preview->clear();
        m_notes->setText(tr("Cannot retarget: %1").arg(QString::fromStdString(r.error().message)));
        return;
    }
    m_result = std::move(r).value();
    QStringList notes;
    if (m_result->approximateView)
        notes << tr("<b>Approximate view.</b> The camera is far from the view the sprite is drawn in; the art is not turned.");
    for (const std::string& n : m_result->notes) notes << QString::fromStdString(n).toHtmlEscaped();
    for (const std::string& i : ::acestudio::motion::checkClip(m_result->clip).issues) notes << tr("Check: %1").arg(QString::fromStdString(i).toHtmlEscaped());
    if (notes.isEmpty()) notes << tr("No problems found.");
    m_notes->setText(notes.join(QStringLiteral("<br>")));
    const int n = static_cast<int>(m_result->clip.frames.size());
    m_scrub->blockSignals(true);
    m_scrub->setRange(0, std::max(0, n - 1));
    m_scrub->blockSignals(false);
    m_frame = std::min(m_frame, n - 1);
    m_timer->setInterval(std::max(16, m_result->clip.durations.empty() ? 83 : m_result->clip.durations[0]));
    if (!m_timer->isActive() && m_play->text() == tr("Pause") && isVisible()) m_timer->start();
    m_openButton->setEnabled(true);
    showFrame();
}

void MotionStudioPanel::tick() {
    if (!m_result || m_result->clip.frames.empty()) return;
    m_frame = (m_frame + 1) % static_cast<int>(m_result->clip.frames.size());
    m_scrub->blockSignals(true);
    m_scrub->setValue(m_frame);
    m_scrub->blockSignals(false);
    showFrame();
}

void MotionStudioPanel::showFrame() {
    if (!m_result || m_frame < 0 || m_frame >= static_cast<int>(m_result->clip.frames.size())) return;
    const Image& f = m_result->clip.frames[static_cast<std::size_t>(m_frame)];
    const QSize area = m_preview->size() - QSize(8, 8);
    const int k = std::max(1, std::min(area.width() / std::max(1, f.width()), area.height() / std::max(1, f.height())));
    // Whole-pixel zoom on a checkerboard: pixel art stays crisp.
    QImage canvas(f.width() * k, f.height() * k, QImage::Format_ARGB32);
    QPainter p(&canvas);
    const int cell = std::max(4, k * 2);
    for (int y = 0; y < canvas.height(); y += cell)
        for (int x = 0; x < canvas.width(); x += cell)
            p.fillRect(x, y, cell, cell, ((x / cell + y / cell) % 2) ? QColor("#2A2A31") : QColor("#34343C"));
    p.drawImage(QRect(0, 0, canvas.width(), canvas.height()), toQImage(f));
    p.end();
    m_preview->setPixmap(QPixmap::fromImage(canvas));
    const double fps = m_fps->value();
    const double span = m_motion ? (m_length->value() > 0 ? m_length->value() : m_motion->duration() - m_start->value()) : 0;
    const int n = static_cast<int>(m_result->clip.frames.size());
    m_source->setTime(m_start->value() + (n > 0 ? span * m_frame / n : m_frame / fps), m_inPlace->isChecked());
}

void MotionStudioPanel::openDocument() {
    if (!m_result || !m_open) return;
    const int n = static_cast<int>(m_result->clip.frames.size());
    std::vector<AnimationTag> tags;
    AnimationTag t;
    t.name = m_motionName.isEmpty() ? std::string("motion") : m_motionName.toStdString();
    t.fromFrame = 0;
    t.toFrame = n - 1;
    tags.push_back(t);
    m_open(m_result->clip.frames, m_result->clip.durations, tags, m_name + QStringLiteral("_") + m_motionName);
}

} // namespace acestudio::ui
