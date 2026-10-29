#include "ToolsPanel.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

namespace acestudio::ui {

namespace {

// Minimal monochrome tool glyphs drawn in code (24×24 logical px).
QIcon toolIcon(Tool t, const QColor& fg) {
    QPixmap pm(48, 48);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(2, 2);
    QPen pen(fg, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    switch (t) {
    case Tool::Pencil:
        p.drawLine(QPointF(5, 19), QPointF(17, 7));
        p.drawLine(QPointF(15, 5), QPointF(19, 9));
        p.drawLine(QPointF(5, 19), QPointF(4, 20));
        p.drawLine(QPointF(7, 21), QPointF(3, 17));
        break;
    case Tool::Eraser:
        p.drawRoundedRect(QRectF(4, 9, 16, 8), 2, 2);
        p.drawLine(QPointF(11, 9), QPointF(11, 17));
        break;
    case Tool::Line: p.drawLine(QPointF(5, 19), QPointF(19, 5)); break;
    case Tool::Rectangle: p.drawRect(QRectF(5, 6, 14, 12)); break;
    case Tool::Ellipse: p.drawEllipse(QRectF(4, 6, 16, 12)); break;
    case Tool::Fill: {
        QPainterPath bucket;
        bucket.moveTo(6, 10); bucket.lineTo(12, 4); bucket.lineTo(18, 10); bucket.lineTo(12, 16); bucket.closeSubpath();
        p.drawPath(bucket);
        p.setBrush(fg);
        p.drawEllipse(QRectF(17, 15, 4, 5));
        break;
    }
    case Tool::Gradient: {
        QLinearGradient g(QPointF(4, 0), QPointF(20, 0));
        g.setColorAt(0, fg);
        g.setColorAt(1, Qt::transparent);
        p.setBrush(g);
        p.drawRect(QRectF(4, 6, 16, 12));
        break;
    }
    case Tool::Lasso: {
        QPen dash = pen;
        dash.setStyle(Qt::DashLine);
        dash.setWidthF(1.4);
        p.setPen(dash);
        QPainterPath path;
        path.moveTo(6, 16);
        path.cubicTo(1, 8, 12, 2, 19, 6);
        path.cubicTo(23, 9, 16, 17, 10, 16);
        p.drawPath(path);
        p.setPen(pen);
        p.drawLine(QPointF(10, 16), QPointF(8, 21));
        break;
    }
    case Tool::MagicWand:
        p.drawLine(QPointF(4, 20), QPointF(15, 9));
        p.drawLine(QPointF(18, 3), QPointF(18, 7));
        p.drawLine(QPointF(16, 5), QPointF(20, 5));
        p.drawLine(QPointF(11, 4), QPointF(12, 6));
        p.drawLine(QPointF(19, 11), QPointF(21, 12));
        break;
    case Tool::Eyedropper:
        p.drawLine(QPointF(5, 19), QPointF(14, 10));
        p.drawEllipse(QRectF(13, 5, 6, 6));
        break;
    case Tool::Select: {
        QPen dash = pen;
        dash.setStyle(Qt::DashLine);
        dash.setWidthF(1.4);
        p.setPen(dash);
        p.drawRect(QRectF(4.5, 5.5, 15, 13));
        break;
    }
    case Tool::Hand:
        p.drawLine(QPointF(8, 12), QPointF(8, 6));
        p.drawLine(QPointF(11, 11), QPointF(11, 4));
        p.drawLine(QPointF(14, 11), QPointF(14, 5));
        p.drawLine(QPointF(17, 12), QPointF(17, 7));
        p.drawArc(QRectF(7, 8, 11, 12), 180 * 16, 180 * 16);
        break;
    }
    return QIcon(pm);
}

} // namespace

// Overlapping primary/secondary swatches. Click: edit colour. Swap arrow
// in the corner (or X) swaps them.
class ColorPairWidget : public QWidget {
public:
    explicit ColorPairWidget(ToolState* s, QWidget* parent) : QWidget(parent), m_s(s) {
        setFixedSize(64, 56);
        setToolTip(tr("Primary / secondary colour. Click to edit; click the arrow (or press X) to swap."));
        connect(s, &ToolState::changed, this, qOverload<>(&QWidget::update));
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        auto swatch = [&](QRect r, const QColor& c) {
            // checker behind translucent colours
            p.fillRect(r, QColor("#999"));
            for (int y = 0; y < r.height(); y += 6)
                for (int x = (y / 6) % 2 * 6; x < r.width(); x += 12) p.fillRect(r.x() + x, r.y() + y, 6, 6, QColor("#666"));
            p.fillRect(r, c);
            p.setPen(palette().color(QPalette::Mid));
            p.drawRect(r.adjusted(0, 0, -1, -1));
        };
        swatch(secondaryRect(), m_s->secondary());
        swatch(primaryRect(), m_s->primary());
        p.setPen(palette().color(QPalette::WindowText));
        p.drawText(swapRect(), Qt::AlignCenter, QStringLiteral("⇄"));
    }
    void mousePressEvent(QMouseEvent* e) override {
        const QPoint pt = e->position().toPoint();
        if (swapRect().contains(pt)) { m_s->swapColors(); return; }
        const bool primary = primaryRect().contains(pt);
        if (!primary && !secondaryRect().contains(pt)) return;
        const QColor start = primary ? m_s->primary() : m_s->secondary();
        const QColor c = QColorDialog::getColor(start, this, primary ? tr("Primary Colour") : tr("Secondary Colour"),
                                                QColorDialog::ShowAlphaChannel);
        if (!c.isValid()) return;
        if (primary) m_s->setPrimary(c);
        else m_s->setSecondary(c);
    }

private:
    QRect primaryRect() const { return {0, 0, 36, 36}; }
    QRect secondaryRect() const { return {20, 18, 36, 36}; }
    QRect swapRect() const { return {42, 0, 20, 16}; }
    ToolState* m_s;
};

ToolsPanel::ToolsPanel(ToolState* state, QWidget* parent) : QWidget(parent), m_state(state) {
    m_group = new QButtonGroup(this);
    m_group->setExclusive(true);
    auto* grid = new QGridLayout;
    grid->setSpacing(2);
    const QColor fg = palette().color(QPalette::WindowText);
    int i = 0;
    for (Tool t : kAllTools) {
        auto* b = new QToolButton(this);
        b->setIcon(toolIcon(t, fg));
        b->setIconSize(QSize(22, 22));
        b->setCheckable(true);
        b->setAutoRaise(true);
        b->setFixedSize(34, 34);
        b->setToolTip(QStringLiteral("%1 (%2)").arg(toolName(t), toolShortcut(t)));
        m_group->addButton(b, static_cast<int>(t));
        grid->addWidget(b, i / 3, i % 3);
        ++i;
    }
    connect(m_group, &QButtonGroup::idClicked, this, [this](int id) { m_state->setTool(static_cast<Tool>(id)); });

    m_colors = new ColorPairWidget(state, this);
    m_toolLabel = new QLabel(this);
    QFont bold = m_toolLabel->font();
    bold.setBold(true);
    m_toolLabel->setFont(bold);

    // Brush options
    m_brushBox = new QWidget(this);
    auto* bf = new QFormLayout(m_brushBox);
    bf->setContentsMargins(0, 0, 0, 0);
    m_size = new QSpinBox(m_brushBox);
    m_size->setRange(1, 64);
    m_size->setSuffix(tr(" px"));
    m_shape = new QComboBox(m_brushBox);
    m_shape->addItem(tr("Square"), static_cast<int>(BrushShape::Square));
    m_shape->addItem(tr("Circle"), static_cast<int>(BrushShape::Circle));
    m_mirror = new QComboBox(m_brushBox);
    m_mirror->addItem(tr("Off"), static_cast<int>(MirrorMode::None));
    m_mirror->addItem(tr("Left ↔ Right"), static_cast<int>(MirrorMode::Horizontal));
    m_mirror->addItem(tr("Top ↕ Bottom"), static_cast<int>(MirrorMode::Vertical));
    m_mirror->addItem(tr("Both"), static_cast<int>(MirrorMode::Both));
    m_dither = new QSpinBox(m_brushBox);
    m_dither->setRange(1, 16);
    m_dither->setSuffix(tr(" / 16"));
    m_dither->setToolTip(tr("Ordered-dither density: 16 paints solid, 8 paints a checkerboard"));
    m_pixelPerfect = new QCheckBox(tr("Pixel-perfect"), m_brushBox);
    m_pixelPerfect->setToolTip(tr("Removes doubled corner pixels from 1px freehand strokes"));
    m_filled = new QCheckBox(tr("Filled shape"), m_brushBox);
    bf->addRow(tr("Size"), m_size);
    bf->addRow(tr("Shape"), m_shape);
    bf->addRow(tr("Mirror"), m_mirror);
    bf->addRow(tr("Density"), m_dither);
    bf->addRow(m_pixelPerfect);
    bf->addRow(m_filled);

    // Fill / sampling options
    m_fillBox = new QWidget(this);
    auto* ff = new QFormLayout(m_fillBox);
    ff->setContentsMargins(0, 0, 0, 0);
    m_contiguous = new QCheckBox(tr("Contiguous"), m_fillBox);
    m_tolerance = new QSpinBox(m_fillBox);
    m_tolerance->setRange(0, 255);
    ff->addRow(m_contiguous);
    ff->addRow(tr("Tolerance"), m_tolerance);
    m_sampleMerged = new QCheckBox(tr("Sample all layers"), this);

    // Selection mode
    m_selBox = new QWidget(this);
    auto* sf = new QFormLayout(m_selBox);
    sf->setContentsMargins(0, 0, 0, 0);
    m_selOp = new QComboBox(m_selBox);
    m_selOp->addItem(tr("Replace"), static_cast<int>(SelectionOp::Replace));
    m_selOp->addItem(tr("Add (Shift)"), static_cast<int>(SelectionOp::Add));
    m_selOp->addItem(tr("Subtract (Alt)"), static_cast<int>(SelectionOp::Subtract));
    m_selOp->addItem(tr("Intersect (Shift+Alt)"), static_cast<int>(SelectionOp::Intersect));
    sf->addRow(tr("Mode"), m_selOp);

    // Gradient
    m_gradBox = new QWidget(this);
    auto* gf = new QFormLayout(m_gradBox);
    gf->setContentsMargins(0, 0, 0, 0);
    m_gradShape = new QComboBox(m_gradBox);
    m_gradShape->addItem(tr("Linear"), static_cast<int>(GradientShape::Linear));
    m_gradShape->addItem(tr("Radial"), static_cast<int>(GradientShape::Radial));
    m_gradDither = new QComboBox(m_gradBox);
    m_gradDither->addItem(tr("Dithered (2 colours)"), static_cast<int>(GradientDither::Ordered));
    m_gradDither->addItem(tr("Smooth"), static_cast<int>(GradientDither::Smooth));
    gf->addRow(tr("Shape"), m_gradShape);
    gf->addRow(tr("Style"), m_gradDither);
    connect(m_selOp, &QComboBox::currentIndexChanged, this,
            [this] { if (!m_syncing) m_state->setSelectionOp(static_cast<SelectionOp>(m_selOp->currentData().toInt())); });
    connect(m_gradShape, &QComboBox::currentIndexChanged, this,
            [this] { if (!m_syncing) m_state->setGradientShape(static_cast<GradientShape>(m_gradShape->currentData().toInt())); });
    connect(m_gradDither, &QComboBox::currentIndexChanged, this,
            [this] { if (!m_syncing) m_state->setGradientDither(static_cast<GradientDither>(m_gradDither->currentData().toInt())); });
    connect(m_dither, &QSpinBox::valueChanged, this, [this](int v) { if (!m_syncing) m_state->setDitherDensity(v); });

    connect(m_size, &QSpinBox::valueChanged, this, [this](int v) { if (!m_syncing) m_state->setBrushSize(v); });
    connect(m_shape, &QComboBox::currentIndexChanged, this,
            [this] { if (!m_syncing) m_state->setBrushShape(static_cast<BrushShape>(m_shape->currentData().toInt())); });
    connect(m_mirror, &QComboBox::currentIndexChanged, this,
            [this] { if (!m_syncing) m_state->setMirror(static_cast<MirrorMode>(m_mirror->currentData().toInt())); });
    connect(m_pixelPerfect, &QCheckBox::toggled, this, [this](bool on) { if (!m_syncing) m_state->setPixelPerfect(on); });
    connect(m_filled, &QCheckBox::toggled, this, [this](bool on) { if (!m_syncing) m_state->setFillShapes(on); });
    connect(m_contiguous, &QCheckBox::toggled, this, [this](bool on) { if (!m_syncing) m_state->setFillContiguous(on); });
    connect(m_tolerance, &QSpinBox::valueChanged, this, [this](int v) { if (!m_syncing) m_state->setFillTolerance(v); });
    connect(m_sampleMerged, &QCheckBox::toggled, this, [this](bool on) { if (!m_syncing) m_state->setSampleMerged(on); });
    connect(m_state, &ToolState::changed, this, &ToolsPanel::syncFromState);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->addLayout(grid);
    root->addSpacing(8);
    root->addWidget(m_colors);
    root->addSpacing(8);
    root->addWidget(m_toolLabel);
    root->addWidget(m_brushBox);
    root->addWidget(m_fillBox);
    root->addWidget(m_selBox);
    root->addWidget(m_gradBox);
    root->addWidget(m_sampleMerged);
    root->addStretch();
    syncFromState();
}

void ToolsPanel::syncFromState() {
    m_syncing = true;
    const Tool t = m_state->tool();
    if (auto* b = m_group->button(static_cast<int>(t))) b->setChecked(true);
    m_toolLabel->setText(toolName(t));
    const BrushSettings br = m_state->brush();
    m_size->setValue(br.size);
    m_shape->setCurrentIndex(m_shape->findData(static_cast<int>(br.shape)));
    m_mirror->setCurrentIndex(m_mirror->findData(static_cast<int>(br.mirror)));
    m_pixelPerfect->setChecked(m_state->pixelPerfect());
    m_filled->setChecked(m_state->fillShapes());
    m_contiguous->setChecked(m_state->fillContiguous());
    m_tolerance->setValue(m_state->fillTolerance());
    m_sampleMerged->setChecked(m_state->sampleMerged());
    m_dither->setValue(br.ditherDensity);
    m_selOp->setCurrentIndex(m_selOp->findData(static_cast<int>(m_state->selectionOp())));
    m_gradShape->setCurrentIndex(m_gradShape->findData(static_cast<int>(m_state->gradientShape())));
    m_gradDither->setCurrentIndex(m_gradDither->findData(static_cast<int>(m_state->gradientDither())));

    const bool brushTool = t == Tool::Pencil || t == Tool::Eraser || t == Tool::Line || t == Tool::Rectangle || t == Tool::Ellipse;
    m_brushBox->setVisible(brushTool);
    m_pixelPerfect->setVisible(t == Tool::Pencil || t == Tool::Eraser);
    m_filled->setVisible(t == Tool::Rectangle || t == Tool::Ellipse);
    m_fillBox->setVisible(t == Tool::Fill || t == Tool::MagicWand);
    m_sampleMerged->setVisible(t == Tool::Eyedropper || t == Tool::MagicWand);
    m_selBox->setVisible(isSelectionTool(t));
    m_gradBox->setVisible(t == Tool::Gradient);
    m_dither->setVisible(t == Tool::Pencil);
    if (auto* lbl = qobject_cast<QFormLayout*>(m_brushBox->layout())->labelForField(m_dither)) lbl->setVisible(t == Tool::Pencil);
    m_syncing = false;
}

} // namespace acestudio::ui
