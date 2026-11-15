#include "MotionStudioDialog.h"

#include "QtBridge.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

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
        p.drawText(rect(), Qt::AlignCenter, tr("Load a BVH motion"));
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

// ---- MotionStudioDialog ----------------------------------------------------------------

namespace {

QSlider* angleSlider(QWidget* parent, int lo, int hi, const QString& name) {
    auto* s = new QSlider(Qt::Horizontal, parent);
    s->setRange(lo, hi);
    s->setAccessibleName(name);
    s->setMinimumWidth(180);
    return s;
}

} // namespace

MotionStudioDialog::MotionStudioDialog(const Image& sprite, const QString& name, OpenFn open, QWidget* parent)
    : QDialog(parent), m_sprite(sprite), m_name(name), m_open(std::move(open)) {
    setWindowTitle(tr("Motion Studio: 3D Motion onto a Sprite"));
    setSizeGripEnabled(true);
    auto* root = new QVBoxLayout(this);

    // Motion and camera controls.
    auto* top = new QHBoxLayout;
    root->addLayout(top);
    auto* motionBox = new QGroupBox(tr("Motion"), this);
    auto* mf = new QFormLayout(motionBox);
    auto* load = new QPushButton(tr("&Load BVH…"), motionBox);
    load->setAutoDefault(false);
    load->setToolTip(tr("A BVH motion file (Blender, Mixamo via Blender, CMU mocap, Rokoko). FBX and glTF: export to BVH from Blender first."));
    m_motionLabel = new QLabel(tr("No motion loaded"), motionBox);
    m_motionLabel->setWordWrap(true);
    mf->addRow(load, m_motionLabel);
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
    mf->addRow(tr("&Start:"), m_start);
    mf->addRow(tr("&Length:"), m_length);
    mf->addRow(tr("&Frame rate:"), m_fps);
    mf->addRow(tr("&Amount:"), m_amount);
    mf->addRow(QString(), m_inPlace);
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
    cf->addRow(tr("&View:"), m_view);
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
    angleRow(m_turn, m_turnValue, -180, 180, tr("&Turn:"), tr("Camera turn"));
    angleRow(m_tilt, m_tiltValue, -90, 90, tr("T&ilt:"), tr("Camera tilt"));
    angleRow(m_roll, m_rollValue, -180, 180, tr("&Roll:"), tr("Camera roll"));
    m_perspective = new QCheckBox(tr("&Perspective"), camBox);
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

    auto* buttons = new QDialogButtonBox(this);
    m_openButton = buttons->addButton(tr("&Open as Document"), QDialogButtonBox::AcceptRole);
    buttons->addButton(QDialogButtonBox::Close);
    m_openButton->setEnabled(false);
    root->addWidget(buttons);

    m_timer = new QTimer(this);
    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(120);

    connect(load, &QPushButton::clicked, this, &MotionStudioDialog::loadMotion);
    connect(m_view, &QComboBox::currentIndexChanged, this, [this] { presetChosen(); });
    for (QSlider* s : {m_turn, m_tilt, m_roll}) connect(s, &QSlider::valueChanged, this, [this] { cameraEdited(); });
    connect(m_perspective, &QCheckBox::toggled, this, [this] { cameraEdited(); });
    for (QDoubleSpinBox* s : {m_start, m_length, m_amount}) connect(s, &QDoubleSpinBox::valueChanged, this, [this] { scheduleRetarget(); });
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
    connect(m_debounce, &QTimer::timeout, this, &MotionStudioDialog::retarget);
    connect(m_timer, &QTimer::timeout, this, &MotionStudioDialog::tick);
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
    connect(m_openButton, &QPushButton::clicked, this, &MotionStudioDialog::openDocument);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    presetChosen();
    resize(980, 760);
}

MotionStudioDialog::~MotionStudioDialog() = default;

mc::Camera MotionStudioDialog::camera() const {
    mc::Camera c;
    c.turn = m_turn->value();
    c.tilt = m_tilt->value();
    c.roll = m_roll->value();
    c.perspective = m_perspective->isChecked();
    return c;
}

void MotionStudioDialog::presetChosen() {
    if (m_syncing) return;
    const QString key = m_view->currentData().toString();
    mc::Camera c = camera();
    if (key.isEmpty()) {
        // As drawn: the side the sprite faces, or the front.
        const ::acestudio::motion::Subject s = ::acestudio::motion::analyzeSubject(m_sprite);
        c = mc::Camera{};
        c.turn = s.frontView ? 0 : (s.facing < 0 ? -90 : 90);
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

void MotionStudioDialog::cameraEdited() {
    if (m_syncing) return;
    m_turnValue->setText(tr("%1 deg").arg(m_turn->value()));
    m_tiltValue->setText(tr("%1 deg").arg(m_tilt->value()));
    m_rollValue->setText(tr("%1 deg").arg(m_roll->value()));
    m_source->setCamera(camera());
    scheduleRetarget();
}

void MotionStudioDialog::scheduleRetarget() {
    if (m_motion) m_debounce->start();
}

void MotionStudioDialog::loadMotion() {
    const QString path = QFileDialog::getOpenFileName(this, tr("Load Motion"), QString(), tr("BVH motion (*.bvh);;All files (*)"));
    if (!path.isEmpty()) loadMotionFile(path);
}

void MotionStudioDialog::loadMotionFile(const QString& path) {
    auto m = mc::loadBvh(path.toStdWString());
    if (!m) {
        QMessageBox::warning(this, tr("Load Motion"), tr("This motion could not be read:\n%1").arg(QString::fromStdString(m.error().message)));
        return;
    }
    m_motion = std::move(m).value();
    m_map = mc::autoMap(*m_motion);
    m_motionName = QFileInfo(path).completeBaseName();
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

void MotionStudioDialog::retarget() {
    if (!m_motion) return;
    mc::RetargetOptions o;
    o.camera = camera();
    o.start = m_start->value();
    o.length = m_length->value();
    o.fps = m_fps->value();
    o.amount = m_amount->value();
    o.inPlace = m_inPlace->isChecked();
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
    if (!m_timer->isActive() && m_play->text() == tr("Pause")) m_timer->start();
    m_openButton->setEnabled(true);
    showFrame();
}

void MotionStudioDialog::tick() {
    if (!m_result || m_result->clip.frames.empty()) return;
    m_frame = (m_frame + 1) % static_cast<int>(m_result->clip.frames.size());
    m_scrub->blockSignals(true);
    m_scrub->setValue(m_frame);
    m_scrub->blockSignals(false);
    showFrame();
}

void MotionStudioDialog::showFrame() {
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

void MotionStudioDialog::openDocument() {
    if (!m_result || !m_open) return;
    const int n = static_cast<int>(m_result->clip.frames.size());
    std::vector<AnimationTag> tags;
    AnimationTag t;
    t.name = m_motionName.isEmpty() ? std::string("motion") : m_motionName.toStdString();
    t.fromFrame = 0;
    t.toFrame = n - 1;
    tags.push_back(t);
    m_open(m_result->clip.frames, m_result->clip.durations, tags, m_name + QStringLiteral("_") + m_motionName);
    accept();
}

} // namespace acestudio::ui
