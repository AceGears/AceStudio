#include "PoseEditor.h"

#include "../QtBridge.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>

#include <cmath>

namespace acestudio::ui {

PoseCanvas::PoseCanvas(QWidget* parent) : QWidget(parent) {
    setMinimumSize(240, 240);
    setMouseTracking(true);
    setToolTip(tr("Drag a joint to move it · right-click a joint to hide or show it"));
    m_pose = standingPose();
}

void PoseCanvas::setPose(const Pose& p) {
    m_pose = p;
    update();
}

void PoseCanvas::setUnderlay(const QImage& img) {
    m_underlay = img;
    update();
}

QRectF PoseCanvas::area() const {
    const double side = std::min(width(), height()) - 8.0;
    return {(width() - side) / 2.0, (height() - side) / 2.0, side, side};
}

QPointF PoseCanvas::toWidget(const Keypoint& k) const {
    const QRectF a = area();
    return {a.left() + k.x * a.width(), a.top() + k.y * a.height()};
}

int PoseCanvas::hit(QPointF p) const {
    int best = -1;
    double bestD = 9.0;
    for (int i = 0; i < kJointCount; ++i) {
        const double d = QLineF(toWidget(m_pose.points[static_cast<std::size_t>(i)]), p).length();
        if (d < bestD) { bestD = d; best = i; }
    }
    return best;
}

void PoseCanvas::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF a = area();
    p.fillRect(a, QColor(16, 16, 20));
    if (!m_underlay.isNull()) {
        p.setOpacity(0.45);
        p.drawImage(a, m_underlay);   // nearest-neighbour scaling (no smooth hint)
        p.setOpacity(1.0);
    }
    // Ground line at the standing figure's feet.
    p.setPen(QPen(QColor(90, 90, 100), 1, Qt::DashLine));
    p.drawLine(QPointF(a.left(), a.top() + 0.9 * a.height()), QPointF(a.right(), a.top() + 0.9 * a.height()));
    const auto& limbs = poseLimbs();
    for (std::size_t i = 0; i < limbs.size(); ++i) {
        const Keypoint &k1 = m_pose[limbs[i].first], &k2 = m_pose[limbs[i].second];
        if (!k1.visible || !k2.visible) continue;
        p.setPen(QPen(toQColor(limbColor(i)), 3, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(toWidget(k1), toWidget(k2));
    }
    for (int j = 0; j < kJointCount; ++j) {
        const Keypoint& k = m_pose.points[static_cast<std::size_t>(j)];
        const QPointF c = toWidget(k);
        const double r = (j == m_hover || j == m_drag) ? 6.0 : 4.5;
        p.setPen(QPen(Qt::white, 1));
        p.setBrush(k.visible ? toQColor(jointColor(static_cast<Joint>(j))) : QColor(0, 0, 0, 0));
        p.drawEllipse(c, r, r);
    }
    if (m_hover >= 0) {
        p.setPen(Qt::white);
        p.drawText(a.adjusted(6, 4, -6, -4), Qt::AlignTop | Qt::AlignLeft,
                   QString::fromLatin1(toString(static_cast<Joint>(m_hover)).data()));
    }
}

void PoseCanvas::mousePressEvent(QMouseEvent* e) {
    const int j = hit(e->position());
    if (j < 0) return;
    if (e->button() == Qt::RightButton) {
        Keypoint& k = m_pose.points[static_cast<std::size_t>(j)];
        if (static_cast<Joint>(j) != Joint::Neck) k.visible = !k.visible;   // the neck anchors the body
        update();
        Q_EMIT poseEdited();
        return;
    }
    if (e->button() == Qt::LeftButton) m_drag = j;
}

void PoseCanvas::mouseMoveEvent(QMouseEvent* e) {
    if (m_drag < 0) {
        const int h = hit(e->position());
        if (h != m_hover) { m_hover = h; update(); }
        return;
    }
    const QRectF a = area();
    Keypoint& k = m_pose.points[static_cast<std::size_t>(m_drag)];
    k.x = std::clamp((e->position().x() - a.left()) / a.width(), 0.0, 1.0);
    k.y = std::clamp((e->position().y() - a.top()) / a.height(), 0.0, 1.0);
    update();
    Q_EMIT poseEdited();
}

void PoseCanvas::mouseReleaseEvent(QMouseEvent*) { m_drag = -1; }

// ------------------------------------------------------------------ dialog

PoseDialog::PoseDialog(const Pose& initial, const QImage& underlay, QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Edit Pose"));
    m_canvas = new PoseCanvas(this);
    m_canvas->setPose(initial);
    m_canvas->setUnderlay(underlay);
    m_preset = new QComboBox(this);
    m_preset->addItem(tr("Standing"), QStringLiteral("stand"));
    for (PoseCycle c : {PoseCycle::Idle, PoseCycle::Walk, PoseCycle::Run, PoseCycle::Jump})
        m_preset->addItem(tr("Cycle: %1").arg(QString::fromLatin1(toString(c).data())), QString::fromLatin1(toString(c).data()));
    m_phase = new QSlider(Qt::Horizontal, this);
    m_phase->setRange(0, 99);
    m_phase->setToolTip(tr("Phase of the cycle"));
    auto* apply = new QPushButton(tr("&Apply Preset"), this);
    auto* mirror = new QPushButton(tr("&Mirror"), this);
    connect(apply, &QPushButton::clicked, this, &PoseDialog::applyPreset);
    connect(m_phase, &QSlider::valueChanged, this, &PoseDialog::applyPreset);
    connect(mirror, &QPushButton::clicked, this, [this] { m_canvas->setPose(m_canvas->pose().mirrored()); });
    auto* row = new QHBoxLayout;
    row->addWidget(new QLabel(tr("Preset"), this));
    row->addWidget(m_preset);
    row->addWidget(m_phase, 1);
    row->addWidget(apply);
    row->addWidget(mirror);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* root = new QVBoxLayout(this);
    root->addWidget(m_canvas, 1);
    root->addLayout(row);
    root->addWidget(buttons);
    resize(460, 520);
}

void PoseDialog::applyPreset() {
    const QString id = m_preset->currentData().toString();
    if (id == QLatin1String("stand")) { m_canvas->setPose(standingPose()); return; }
    if (auto c = poseCycleFromString(id.toStdString())) m_canvas->setPose(cyclePose(*c, m_phase->value() / 100.0));
}

Pose PoseDialog::pose() const { return m_canvas->pose(); }

} // namespace acestudio::ui
