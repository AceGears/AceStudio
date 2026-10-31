#include "LayerDialogs.h"
#include "QtBridge.h"

#include "acestudio/core/Adjust.h"
#include "acestudio/core/Editor.h"

#include <algorithm>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSlider>
#include <QVBoxLayout>

namespace acestudio::ui {

namespace {
struct Param {
    const char* label;
    int min, max, def;
};
// Up to three parameters per type (empty label = unused).
constexpr int kTypes = 7;
const Param kParams[kTypes][3] = {
    {{"Brightness", -100, 100, 0}, {"Contrast", -100, 100, 0}, {nullptr, 0, 0, 0}},
    {{"Hue", -180, 180, 0}, {"Saturation", -100, 100, 0}, {"Lightness", -100, 100, 0}},
    {{nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}},
    {{"Levels", 2, 64, 4}, {nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}},
    {{"Level", 0, 255, 128}, {nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}},
    {{"Input black", 0, 254, 0}, {"Input white", 1, 255, 255}, {"Gamma ×100", 10, 999, 100}},
    {{nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}},   // Curves: CurveWidget
};
} // namespace

AdjustmentDialog::AdjustmentDialog(Editor& editor, LayerId layer, QWidget* parent)
    : QDialog(parent), m_editor(editor), m_layer(layer), m_historyIndex(editor.history().index()) {
    setWindowTitle(tr("Adjustment Layer"));
    m_type = new QComboBox(this);
    m_type->addItem(tr("Brightness / Contrast"), static_cast<int>(AdjustmentType::BrightnessContrast));
    m_type->addItem(tr("Hue / Saturation"), static_cast<int>(AdjustmentType::HueSaturation));
    m_type->addItem(tr("Invert"), static_cast<int>(AdjustmentType::Invert));
    m_type->addItem(tr("Posterize"), static_cast<int>(AdjustmentType::Posterize));
    m_type->addItem(tr("Threshold"), static_cast<int>(AdjustmentType::Threshold));
    m_type->addItem(tr("Levels"), static_cast<int>(AdjustmentType::Levels));
    m_type->addItem(tr("Curves"), static_cast<int>(AdjustmentType::Curves));
    auto* form = new QFormLayout;
    form->addRow(tr("Type"), m_type);
    for (int i = 0; i < 3; ++i) {
        m_labels[i] = new QLabel(this);
        m_sliders[i] = new QSlider(Qt::Horizontal, this);
        m_sliders[i]->setMinimumWidth(220);
        m_values[i] = new QLabel(this);
        m_values[i]->setMinimumWidth(36);
        auto* row = new QHBoxLayout;
        row->addWidget(m_sliders[i], 1);
        row->addWidget(m_values[i]);
        form->addRow(m_labels[i], row);
        connect(m_sliders[i], &QSlider::valueChanged, this, [this, i](int v) {
            m_values[i]->setText(QString::number(v));
            if (!m_syncing) apply();
        });
    }
    m_curve = new CurveWidget(this);
    connect(m_curve, &CurveWidget::changed, this, [this] { if (!m_syncing) apply(); });
    form->addRow(m_curve);
    auto* note = new QLabel(tr("Affects the layers below it in the same group. Paint on its mask to limit it to an area."), this);
    note->setWordWrap(true);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(note);
    layout->addWidget(buttons);

    const Layer* l = editor.document().layer(layer);
    const Adjustment adj = l ? l->adjustment : Adjustment{};
    m_syncing = true;
    m_type->setCurrentIndex(m_type->findData(static_cast<int>(adj.type)));
    typeChanged();
    const int vals[3] = {adj.a, adj.b, adj.c};
    for (int i = 0; i < 3; ++i) if (m_sliders[i]->isVisible() || kParams[static_cast<int>(adj.type)][i].label) m_sliders[i]->setValue(vals[i]);
    if (adj.type == AdjustmentType::Curves && adj.points.size() >= 2) m_curve->setPoints(adj.points);
    m_syncing = false;
    connect(m_type, &QComboBox::currentIndexChanged, this, [this] {
        typeChanged();
        if (!m_syncing) apply();
    });
}

void AdjustmentDialog::typeChanged() {
    const bool was = m_syncing;
    m_syncing = true;
    const int t = m_type->currentData().toInt();
    for (int i = 0; i < 3; ++i) {
        const Param& p = kParams[t][i];
        const bool used = p.label != nullptr;
        m_labels[i]->setVisible(used);
        m_sliders[i]->setVisible(used);
        m_values[i]->setVisible(used);
        if (!used) continue;
        m_labels[i]->setText(tr(p.label));
        m_sliders[i]->setRange(p.min, p.max);
        m_sliders[i]->setValue(p.def);
        m_values[i]->setText(QString::number(p.def));
    }
    m_curve->setVisible(t == static_cast<int>(AdjustmentType::Curves));
    adjustSize();
    m_syncing = was;
}

Adjustment AdjustmentDialog::current() const {
    Adjustment a;
    a.type = static_cast<AdjustmentType>(m_type->currentData().toInt());
    const int t = static_cast<int>(a.type);
    int* fields[3] = {&a.a, &a.b, &a.c};
    for (int i = 0; i < 3; ++i) *fields[i] = kParams[t][i].label ? m_sliders[i]->value() : 0;
    if (a.type == AdjustmentType::Levels) a.b = std::max(a.b, a.a + 1);   // white stays above black
    if (a.type == AdjustmentType::Curves) a.points = m_curve->points();
    return a;
}

void AdjustmentDialog::apply() { (void)m_editor.setLayerAdjustment(m_layer, current(), /*mergeable=*/true); }

void AdjustmentDialog::reject() {
    while (m_editor.history().index() > m_historyIndex && m_editor.history().canUndo()) (void)m_editor.undo();
    QDialog::reject();
}

// ------------------------------------------------------------------ curve widget

CurveWidget::CurveWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(200, 200);
    setMouseTracking(true);
    setToolTip(tr("Drag points · click to add · double-click or right-click to remove"));
}

void CurveWidget::setPoints(std::vector<CurvePoint> pts) {
    m_points = std::move(pts);
    update();
}

QRectF CurveWidget::plot() const {
    const double side = std::min(width(), height()) - 12.0;
    return {(width() - side) / 2.0, (height() - side) / 2.0, side, side};
}

QPointF CurveWidget::toWidget(CurvePoint p) const {
    const QRectF r = plot();
    return {r.left() + p.x / 255.0 * r.width(), r.bottom() - p.y / 255.0 * r.height()};
}

CurvePoint CurveWidget::fromWidget(QPointF w) const {
    const QRectF r = plot();
    const int x = std::clamp(static_cast<int>(std::lround((w.x() - r.left()) / r.width() * 255.0)), 0, 255);
    const int y = std::clamp(static_cast<int>(std::lround((r.bottom() - w.y()) / r.height() * 255.0)), 0, 255);
    return {static_cast<std::uint8_t>(x), static_cast<std::uint8_t>(y)};
}

int CurveWidget::hit(QPointF w) const {
    for (std::size_t i = 0; i < m_points.size(); ++i)
        if (QLineF(toWidget(m_points[i]), w).length() <= 7.0) return static_cast<int>(i);
    return -1;
}

void CurveWidget::removeAt(int i) {
    if (i <= 0 || i >= static_cast<int>(m_points.size()) - 1) return;   // keep the end points
    m_points.erase(m_points.begin() + i);
    update();
    Q_EMIT changed();
}

void CurveWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = plot();
    p.fillRect(r, palette().color(QPalette::Base));
    p.setPen(QPen(palette().color(QPalette::Mid), 1, Qt::DotLine));
    for (int i = 1; i < 4; ++i) {
        p.drawLine(QPointF(r.left() + r.width() * i / 4, r.top()), QPointF(r.left() + r.width() * i / 4, r.bottom()));
        p.drawLine(QPointF(r.left(), r.top() + r.height() * i / 4), QPointF(r.right(), r.top() + r.height() * i / 4));
    }
    p.drawLine(r.bottomLeft(), r.topRight());
    const auto table = curveTable(m_points);
    QPainterPath path;
    for (int v = 0; v < 256; ++v) {
        const QPointF w = toWidget({static_cast<std::uint8_t>(v), table[static_cast<std::size_t>(v)]});
        if (v == 0) path.moveTo(w); else path.lineTo(w);
    }
    p.setPen(QPen(palette().color(QPalette::WindowText), 1.6));
    p.drawPath(path);
    p.setBrush(palette().color(QPalette::Highlight));
    for (std::size_t i = 0; i < m_points.size(); ++i)
        p.drawEllipse(toWidget(m_points[i]), static_cast<int>(i) == m_drag ? 5.0 : 4.0, static_cast<int>(i) == m_drag ? 5.0 : 4.0);
    p.setBrush(Qt::NoBrush);
    p.setPen(palette().color(QPalette::Mid));
    p.drawRect(r);
}

void CurveWidget::mousePressEvent(QMouseEvent* e) {
    const QPointF pos = e->position();
    const int i = hit(pos);
    if (e->button() == Qt::RightButton) { removeAt(i); return; }
    if (e->button() != Qt::LeftButton) return;
    if (i >= 0) { m_drag = i; update(); return; }
    if (m_points.size() >= 16) return;
    const CurvePoint np = fromWidget(pos);
    auto it = std::lower_bound(m_points.begin(), m_points.end(), np, [](CurvePoint a, CurvePoint b) { return a.x < b.x; });
    if (it != m_points.end() && it->x == np.x) return;   // x already taken
    m_drag = static_cast<int>(m_points.insert(it, np) - m_points.begin());
    update();
    Q_EMIT changed();
}

void CurveWidget::mouseMoveEvent(QMouseEvent* e) {
    if (m_drag < 0) return;
    CurvePoint np = fromWidget(e->position());
    const auto n = static_cast<int>(m_points.size());
    // End points slide vertically only; inner points stay between their neighbours.
    if (m_drag == 0) np.x = m_points.front().x;
    else if (m_drag == n - 1) np.x = m_points.back().x;
    else {
        const auto d = static_cast<std::size_t>(m_drag);
        np.x = static_cast<std::uint8_t>(std::clamp<int>(np.x, m_points[d - 1].x + 1, m_points[d + 1].x - 1));
    }
    if (np == m_points[static_cast<std::size_t>(m_drag)]) return;
    m_points[static_cast<std::size_t>(m_drag)] = np;
    update();
    Q_EMIT changed();
}

void CurveWidget::mouseReleaseEvent(QMouseEvent*) {
    m_drag = -1;
    update();
}

void CurveWidget::mouseDoubleClickEvent(QMouseEvent* e) { removeAt(hit(e->position())); }

} // namespace acestudio::ui
