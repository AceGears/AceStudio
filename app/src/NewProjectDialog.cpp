#include "NewProjectDialog.h"

#include "acestudio/core/Image.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace acestudio::ui {

NewProjectDialog::NewProjectDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("New Project"));
    setModal(true);

    m_preset = new QComboBox(this);
    const QList<QPair<QString, int>> presets = {
        {tr("Custom"), 0},          {QStringLiteral("16 × 16"), 16},   {QStringLiteral("32 × 32"), 32},
        {QStringLiteral("48 × 48"), 48}, {QStringLiteral("64 × 64"), 64}, {QStringLiteral("96 × 96"), 96},
        {QStringLiteral("128 × 128"), 128}, {QStringLiteral("256 × 256"), 256}};
    for (const auto& [label, size] : presets) m_preset->addItem(label, size);
    m_preset->setCurrentIndex(4);

    auto makeSpin = [this](int value, int max) {
        auto* s = new QSpinBox(this);
        s->setRange(1, max);
        s->setValue(value);
        s->setSuffix(tr(" px"));
        return s;
    };
    m_width = makeSpin(64, Image::kMaxDimension);
    m_height = makeSpin(64, Image::kMaxDimension);
    m_frames = new QSpinBox(this);
    m_frames->setRange(1, Document::kMaxFrames);
    m_frames->setValue(1);

    m_mode = new QComboBox(this);
    m_mode->addItem(tr("RGBA (full color)"), static_cast<int>(ColorMode::Rgba));
    m_mode->addItem(tr("Indexed (palette, up to 256)"), static_cast<int>(ColorMode::Indexed));
    m_mode->addItem(tr("Grayscale"), static_cast<int>(ColorMode::Grayscale));

    connect(m_preset, &QComboBox::currentIndexChanged, this, [this](int) {
        const int size = m_preset->currentData().toInt();
        if (size > 0) { m_width->setValue(size); m_height->setValue(size); }
    });
    auto markCustom = [this] {
        const int size = m_preset->currentData().toInt();
        if (size > 0 && (m_width->value() != size || m_height->value() != size)) {
            const QSignalBlocker block(m_preset);
            m_preset->setCurrentIndex(0);
        }
    };
    connect(m_width, &QSpinBox::valueChanged, this, markCustom);
    connect(m_height, &QSpinBox::valueChanged, this, markCustom);

    auto* form = new QFormLayout;
    form->addRow(tr("Size preset"), m_preset);
    form->addRow(tr("Width"), m_width);
    form->addRow(tr("Height"), m_height);
    form->addRow(tr("Frames"), m_frames);
    form->addRow(tr("Color mode"), m_mode);

    auto* note = new QLabel(tr("Color mode is stored with the project. Indexed and grayscale painting "
                               "constraints arrive with the drawing tools."), this);
    note->setWordWrap(true);
    note->setEnabled(false);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Create"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* root = new QVBoxLayout(this);
    root->addLayout(form);
    root->addWidget(note);
    root->addWidget(buttons);
}

int NewProjectDialog::canvasWidth() const { return m_width->value(); }
int NewProjectDialog::canvasHeight() const { return m_height->value(); }
int NewProjectDialog::frameCount() const { return m_frames->value(); }
ColorMode NewProjectDialog::colorMode() const { return static_cast<ColorMode>(m_mode->currentData().toInt()); }

} // namespace acestudio::ui
