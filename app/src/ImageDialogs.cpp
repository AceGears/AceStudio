#include "ImageDialogs.h"

#include "acestudio/core/Image.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QLabel>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

namespace acestudio::ui {

namespace {
QDialogButtonBox* okCancel(QDialog* d) {
    auto* b = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, d);
    QObject::connect(b, &QDialogButtonBox::accepted, d, &QDialog::accept);
    QObject::connect(b, &QDialogButtonBox::rejected, d, &QDialog::reject);
    return b;
}
QCheckBox* allFramesBox(QDialog* d) {
    auto* c = new QCheckBox(QObject::tr("Apply to all frames"), d);
    c->setToolTip(QObject::tr("Otherwise only the current frame of the current layer is changed"));
    return c;
}
} // namespace

// ------------------------------------------------------------------ canvas size

CanvasSizeDialog::CanvasSizeDialog(int width, int height, QWidget* parent)
    : QDialog(parent), m_oldW(width), m_oldH(height) {
    setWindowTitle(tr("Canvas Size"));
    m_w = new QSpinBox(this);
    m_h = new QSpinBox(this);
    for (QSpinBox* s : {m_w, m_h}) {
        s->setRange(1, Image::kMaxDimension);
        s->setSuffix(tr(" px"));
    }
    m_w->setValue(width);
    m_h->setValue(height);

    m_anchor = new QButtonGroup(this);
    auto* grid = new QGridLayout;
    grid->setSpacing(2);
    const QString arrows[9] = {QStringLiteral("↖"), QStringLiteral("↑"), QStringLiteral("↗"),
                               QStringLiteral("←"), QStringLiteral("•"), QStringLiteral("→"),
                               QStringLiteral("↙"), QStringLiteral("↓"), QStringLiteral("↘")};
    for (int i = 0; i < 9; ++i) {
        auto* b = new QToolButton(this);
        b->setCheckable(true);
        b->setText(arrows[i]);
        b->setFixedSize(28, 28);
        m_anchor->addButton(b, i);
        grid->addWidget(b, i / 3, i % 3);
    }
    m_anchor->button(4)->setChecked(true);

    auto* form = new QFormLayout;
    form->addRow(tr("Width"), m_w);
    form->addRow(tr("Height"), m_h);
    form->addRow(tr("Anchor"), grid);
    auto* note = new QLabel(tr("Current size: %1 × %2. Content outside a smaller canvas is kept in the layers "
                               "and reappears if the canvas grows again.").arg(width).arg(height), this);
    note->setWordWrap(true);
    note->setEnabled(false);
    auto* root = new QVBoxLayout(this);
    root->addLayout(form);
    root->addWidget(note);
    root->addWidget(okCancel(this));
}

int CanvasSizeDialog::newWidth() const { return m_w->value(); }
int CanvasSizeDialog::newHeight() const { return m_h->value(); }

int CanvasSizeDialog::offsetX() const {
    const int col = m_anchor->checkedId() % 3;
    const int dw = newWidth() - m_oldW;
    return col == 0 ? 0 : col == 1 ? dw / 2 : dw;
}

int CanvasSizeDialog::offsetY() const {
    const int row = m_anchor->checkedId() / 3;
    const int dh = newHeight() - m_oldH;
    return row == 0 ? 0 : row == 1 ? dh / 2 : dh;
}

// ------------------------------------------------------------------ rotate

RotateDialog::RotateDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Rotate Layer (RotSprite)"));
    m_angle = new QDoubleSpinBox(this);
    m_angle->setRange(-360.0, 360.0);
    m_angle->setDecimals(1);
    m_angle->setSuffix(QStringLiteral("°"));
    m_angle->setValue(45.0);
    m_allFrames = allFramesBox(this);
    m_expand = new QCheckBox(tr("Expand to fit (don't clip corners)"), this);
    m_expand->setChecked(true);
    m_expand->setToolTip(tr("Keep the whole rotated image. Pixels that land past the canvas edge stay on the layer "
                            "and show if the canvas is enlarged."));
    auto* form = new QFormLayout;
    form->addRow(tr("Angle (clockwise)"), m_angle);
    form->addRow(m_expand);
    form->addRow(m_allFrames);
    auto* note = new QLabel(tr("RotSprite keeps pixel-art edges clean and adds no new colours. "
                               "Applies to the selection, or the whole layer if nothing is selected."), this);
    note->setWordWrap(true);
    note->setEnabled(false);
    auto* root = new QVBoxLayout(this);
    root->addLayout(form);
    root->addWidget(note);
    root->addWidget(okCancel(this));
}

double RotateDialog::degrees() const { return m_angle->value(); }
bool RotateDialog::allFrames() const { return m_allFrames->isChecked(); }
bool RotateDialog::expand() const { return m_expand->isChecked(); }

// ------------------------------------------------------------------ scale

ScaleDialog::ScaleDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Scale Layer"));
    m_x = new QDoubleSpinBox(this);
    m_y = new QDoubleSpinBox(this);
    for (QDoubleSpinBox* s : {m_x, m_y}) {
        s->setRange(1.0, 3200.0);
        s->setDecimals(1);
        s->setSuffix(QStringLiteral(" %"));
        s->setValue(200.0);
    }
    m_lock = new QCheckBox(tr("Keep proportions"), this);
    m_lock->setChecked(true);
    connect(m_x, &QDoubleSpinBox::valueChanged, this, [this](double v) { if (m_lock->isChecked()) m_y->setValue(v); });
    m_allFrames = allFramesBox(this);
    auto* form = new QFormLayout;
    form->addRow(tr("Width"), m_x);
    form->addRow(tr("Height"), m_y);
    form->addRow(m_lock);
    form->addRow(m_allFrames);
    auto* note = new QLabel(tr("Nearest-neighbour: pixels stay sharp. Use multiples of 100 % for exact results."), this);
    note->setWordWrap(true);
    note->setEnabled(false);
    auto* root = new QVBoxLayout(this);
    root->addLayout(form);
    root->addWidget(note);
    root->addWidget(okCancel(this));
}

double ScaleDialog::percentX() const { return m_x->value(); }
double ScaleDialog::percentY() const { return m_y->value(); }
bool ScaleDialog::allFrames() const { return m_allFrames->isChecked(); }

// ------------------------------------------------------------------ outline

OutlineDialog::OutlineDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Outline"));
    m_mode = new QComboBox(this);
    m_mode->addItem(tr("Outside the shape"), static_cast<int>(OutlineMode::Outside));
    m_mode->addItem(tr("Inside the shape"), static_cast<int>(OutlineMode::Inside));
    m_diagonal = new QCheckBox(tr("Include diagonal neighbours (thicker corners)"), this);
    m_allFrames = allFramesBox(this);
    auto* form = new QFormLayout;
    form->addRow(tr("Placement"), m_mode);
    form->addRow(m_diagonal);
    form->addRow(m_allFrames);
    auto* note = new QLabel(tr("Uses the primary colour."), this);
    note->setEnabled(false);
    auto* root = new QVBoxLayout(this);
    root->addLayout(form);
    root->addWidget(note);
    root->addWidget(okCancel(this));
}

OutlineMode OutlineDialog::mode() const { return static_cast<OutlineMode>(m_mode->currentData().toInt()); }
bool OutlineDialog::diagonal() const { return m_diagonal->isChecked(); }
bool OutlineDialog::allFrames() const { return m_allFrames->isChecked(); }

} // namespace acestudio::ui
