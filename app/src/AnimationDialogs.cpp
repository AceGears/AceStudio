#include "AnimationDialogs.h"
#include "DocumentView.h"
#include "QtBridge.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <iterator>

namespace acestudio::ui {

QString directionName(PlaybackDirection d) {
    switch (d) {
    case PlaybackDirection::Forward: return QObject::tr("Forward");
    case PlaybackDirection::Reverse: return QObject::tr("Reverse");
    case PlaybackDirection::PingPong: return QObject::tr("Ping-pong");
    }
    return {};
}

// ------------------------------------------------------------- TagEditDialog

TagEditDialog::TagEditDialog(const AnimationTag& tag, int frameCount, QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Tag"));
    m_name = new QLineEdit(qs(tag.name), this);
    m_from = new QSpinBox(this);
    m_to = new QSpinBox(this);
    for (QSpinBox* s : {m_from, m_to}) s->setRange(1, std::max(1, frameCount));
    m_from->setValue(tag.fromFrame + 1);
    m_to->setValue(tag.toFrame + 1);
    connect(m_from, &QSpinBox::valueChanged, this, [this](int v) { if (m_to->value() < v) m_to->setValue(v); });
    connect(m_to, &QSpinBox::valueChanged, this, [this](int v) { if (m_from->value() > v) m_from->setValue(v); });
    m_direction = new QComboBox(this);
    for (auto d : {PlaybackDirection::Forward, PlaybackDirection::Reverse, PlaybackDirection::PingPong})
        m_direction->addItem(directionName(d), static_cast<int>(d));
    m_direction->setCurrentIndex(m_direction->findData(static_cast<int>(tag.direction)));
    m_repeat = new QSpinBox(this);
    m_repeat->setRange(0, 9999);
    m_repeat->setSpecialValueText(tr("Loop forever"));
    m_repeat->setValue(tag.repeat);
    m_colorValue = qs(tag.color).left(7);
    m_color = new QPushButton(this);
    connect(m_color, &QPushButton::clicked, this, [this] {
        const QColor c = QColorDialog::getColor(QColor(m_colorValue), this, tr("Tag Colour"));
        if (c.isValid()) { m_colorValue = c.name(QColor::HexRgb).toUpper(); updateColorButton(); }
    });
    updateColorButton();

    auto* form = new QFormLayout;
    form->addRow(tr("&Name:"), m_name);
    form->addRow(tr("&From frame:"), m_from);
    form->addRow(tr("&To frame:"), m_to);
    form->addRow(tr("&Direction:"), m_direction);
    form->addRow(tr("&Repeat:"), m_repeat);
    form->addRow(tr("&Colour:"), m_color);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (m_name->text().trimmed().isEmpty()) {
            QMessageBox::warning(this, windowTitle(), tr("A tag needs a name."));
            return;
        }
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(buttons);
}

void TagEditDialog::updateColorButton() {
    m_color->setText(m_colorValue);
    m_color->setStyleSheet(QStringLiteral("QPushButton { border-left: 18px solid %1; padding-left: 6px; }").arg(m_colorValue));
}

AnimationTag TagEditDialog::tag() const {
    AnimationTag t;
    t.name = m_name->text().trimmed().toStdString();
    t.fromFrame = m_from->value() - 1;
    t.toFrame = m_to->value() - 1;
    t.direction = static_cast<PlaybackDirection>(m_direction->currentData().toInt());
    t.repeat = m_repeat->value();
    t.color = m_colorValue.toStdString();
    return t;
}

// ---------------------------------------------------------------- TagsDialog

TagsDialog::TagsDialog(DocumentView* view, QWidget* parent) : QDialog(parent), m_view(view) {
    setWindowTitle(tr("Animation Tags"));
    resize(420, 300);
    m_list = new QListWidget(this);
    connect(m_list, &QListWidget::itemDoubleClicked, this, &TagsDialog::editTag);
    connect(m_list, &QListWidget::currentRowChanged, this, [this](int row) {
        m_edit->setEnabled(row >= 0);
        m_remove->setEnabled(row >= 0);
    });
    auto* add = new QPushButton(tr("&Add…"), this);
    m_edit = new QPushButton(tr("&Edit…"), this);
    m_remove = new QPushButton(tr("&Remove"), this);
    connect(add, &QPushButton::clicked, this, &TagsDialog::addTag);
    connect(m_edit, &QPushButton::clicked, this, &TagsDialog::editTag);
    connect(m_remove, &QPushButton::clicked, this, &TagsDialog::removeTag);
    auto* side = new QVBoxLayout;
    side->addWidget(add);
    side->addWidget(m_edit);
    side->addWidget(m_remove);
    side->addStretch();
    auto* row = new QHBoxLayout;
    row->addWidget(m_list, 1);
    row->addLayout(side);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(row);
    layout->addWidget(buttons);
    if (m_view) connect(m_view, &DocumentView::documentChanged, this, &TagsDialog::rebuild);
    rebuild();
}

void TagsDialog::rebuild() {
    const int keep = m_list->currentRow();
    m_list->clear();
    if (!m_view) return;
    for (const auto& t : m_view->document().tags()) {
        auto* item = new QListWidgetItem(tr("%1   frames %2–%3, %4, %5")
                                             .arg(qs(t.name)).arg(t.fromFrame + 1).arg(t.toFrame + 1)
                                             .arg(directionName(t.direction))
                                             .arg(t.repeat == 0 ? tr("loops") : tr("×%1").arg(t.repeat)));
        QPixmap swatch(12, 12);
        swatch.fill(QColor(qs(t.color).left(7)));
        item->setIcon(QIcon(swatch));
        m_list->addItem(item);
    }
    m_list->setCurrentRow(std::min(keep, m_list->count() - 1));
    m_edit->setEnabled(m_list->currentRow() >= 0);
    m_remove->setEnabled(m_list->currentRow() >= 0);
}

void TagsDialog::addTag() {
    if (!m_view) return;
    AnimationTag t;
    t.name = "tag" + std::to_string(m_view->document().tags().size() + 1);
    t.fromFrame = t.toFrame = m_view->currentFrame();
    // Distinct colours so overlapping tags stay readable in the timeline.
    static const char* const kTagColors[] = {"#4F8CFF", "#E5484D", "#30A46C", "#F5A524", "#8E4EC6", "#12A594", "#E54D9E"};
    t.color = kTagColors[m_view->document().tags().size() % std::size(kTagColors)];
    TagEditDialog dlg(t, m_view->document().frameCount(), this);
    if (dlg.exec() != QDialog::Accepted) return;
    auto r = m_view->editor().addTag(dlg.tag());
    if (!r) return void(QMessageBox::warning(this, windowTitle(), qs(r.error().message)));
    m_list->setCurrentRow(static_cast<int>(r.value()));
}

void TagsDialog::editTag() {
    const int row = m_list->currentRow();
    if (!m_view || row < 0) return;
    TagEditDialog dlg(m_view->document().tags()[static_cast<std::size_t>(row)], m_view->document().frameCount(), this);
    if (dlg.exec() != QDialog::Accepted) return;
    auto st = m_view->editor().updateTag(static_cast<std::size_t>(row), dlg.tag());
    if (!st) QMessageBox::warning(this, windowTitle(), qs(st.error().message));
}

void TagsDialog::removeTag() {
    const int row = m_list->currentRow();
    if (!m_view || row < 0) return;
    auto st = m_view->editor().removeTag(static_cast<std::size_t>(row));
    if (!st) QMessageBox::warning(this, windowTitle(), qs(st.error().message));
}

// ----------------------------------------------------------- OnionSkinDialog

OnionSkinDialog::OnionSkinDialog(const OnionSkin& o, QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Onion Skin"));
    m_before = new QSpinBox(this);
    m_after = new QSpinBox(this);
    for (QSpinBox* s : {m_before, m_after}) s->setRange(0, 8);
    m_before->setValue(o.before);
    m_after->setValue(o.after);
    m_opacity = new QSpinBox(this);
    m_opacity->setRange(5, 100);
    m_opacity->setSuffix(QStringLiteral(" %"));
    m_opacity->setValue(o.opacity);
    m_tint = new QCheckBox(tr("Tint earlier frames red, later frames blue"), this);
    m_tint->setChecked(o.tint);
    m_wrap = new QCheckBox(tr("Wrap around (looping animation)"), this);
    m_wrap->setChecked(o.wrap);
    auto* form = new QFormLayout;
    form->addRow(tr("Frames &before:"), m_before);
    form->addRow(tr("Frames &after:"), m_after);
    form->addRow(tr("&Opacity (nearest):"), m_opacity);
    form->addRow(QString(), m_tint);
    form->addRow(QString(), m_wrap);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(buttons);
}

OnionSkin OnionSkinDialog::options() const {
    OnionSkin o;
    o.before = m_before->value();
    o.after = m_after->value();
    o.opacity = m_opacity->value();
    o.tint = m_tint->isChecked();
    o.wrap = m_wrap->isChecked();
    return o;
}

} // namespace acestudio::ui
