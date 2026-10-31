#include "LayerDialogs.h"
#include "QtBridge.h"

#include "acestudio/core/Editor.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QVBoxLayout>

namespace acestudio::ui {

namespace {
struct Param {
    const char* label;
    int min, max, def;
};
// Up to three parameters per type (empty label = unused).
const Param kParams[5][3] = {
    {{"Brightness", -100, 100, 0}, {"Contrast", -100, 100, 0}, {nullptr, 0, 0, 0}},
    {{"Hue", -180, 180, 0}, {"Saturation", -100, 100, 0}, {"Lightness", -100, 100, 0}},
    {{nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}},
    {{"Levels", 2, 64, 4}, {nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}},
    {{"Level", 0, 255, 128}, {nullptr, 0, 0, 0}, {nullptr, 0, 0, 0}},
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
    m_syncing = was;
}

Adjustment AdjustmentDialog::current() const {
    Adjustment a;
    a.type = static_cast<AdjustmentType>(m_type->currentData().toInt());
    const int t = static_cast<int>(a.type);
    int* fields[3] = {&a.a, &a.b, &a.c};
    for (int i = 0; i < 3; ++i) *fields[i] = kParams[t][i].label ? m_sliders[i]->value() : 0;
    return a;
}

void AdjustmentDialog::apply() { (void)m_editor.setLayerAdjustment(m_layer, current(), /*mergeable=*/true); }

void AdjustmentDialog::reject() {
    while (m_editor.history().index() > m_historyIndex && m_editor.history().canUndo()) (void)m_editor.undo();
    QDialog::reject();
}

} // namespace acestudio::ui
