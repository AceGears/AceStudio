#include "CharacterDialog.h"

#include "../DocumentView.h"
#include "../QtBridge.h"

#include "acestudio/core/Log.h"
#include "acestudio/core/PaletteIO.h"
#include "acestudio/core/PngCodec.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QVBoxLayout>

namespace acestudio::ui {

namespace fs = std::filesystem;

namespace {
QIcon thumbnail(const Image& img, int size = 64) {
    QImage q = toQImage(img);
    QPixmap pm(size, size);
    pm.fill(QColor(40, 40, 46));
    QPainter p(&pm);
    const QImage s = q.scaled(size, size, Qt::KeepAspectRatio, Qt::FastTransformation);
    p.drawImage((size - s.width()) / 2, (size - s.height()) / 2, s);
    return QIcon(pm);
}
} // namespace

fs::path CharacterDialog::pathFor(const fs::path& dir, const CharacterProfile& c) { return dir / (c.id + ".acechar"); }

CharacterDialog::CharacterDialog(fs::path libraryDir, DocumentView* view, QWidget* parent)
    : QDialog(parent), m_dir(std::move(libraryDir)), m_view(view) {
    setWindowTitle(tr("Characters"));
    std::error_code ec;
    fs::create_directories(m_dir, ec);

    m_list = new QListWidget(this);
    m_list->setAccessibleName(tr("Characters"));
    m_list->setMinimumWidth(170);
    auto* newBtn = new QPushButton(tr("&New"), this);
    auto* dupBtn = new QPushButton(tr("D&uplicate"), this);
    auto* delBtn = new QPushButton(tr("&Delete"), this);
    auto* listButtons = new QHBoxLayout;
    listButtons->addWidget(newBtn);
    listButtons->addWidget(dupBtn);
    listButtons->addWidget(delBtn);
    auto* left = new QWidget(this);
    auto* lv = new QVBoxLayout(left);
    lv->setContentsMargins(0, 0, 0, 0);
    lv->addWidget(m_list, 1);
    lv->addLayout(listButtons);

    m_form = new QWidget(this);
    m_name = new QLineEdit(m_form);
    m_name->setMaxLength(100);
    m_prompt = new QPlainTextEdit(m_form);
    m_prompt->setPlaceholderText(tr("What the character looks like, e.g. “knight in red armour with a round shield”"));
    m_prompt->setFixedHeight(60);
    m_prompt->setTabChangesFocus(true);
    m_negative = new QLineEdit(m_form);
    m_negative->setPlaceholderText(tr("Avoid (optional)"));
    m_width = new QSpinBox(m_form);
    m_height = new QSpinBox(m_form);
    for (QSpinBox* s : {m_width, m_height}) { s->setRange(8, 1024); s->setSuffix(tr(" px")); }
    auto* size = new QHBoxLayout;
    size->addWidget(m_width);
    size->addWidget(new QLabel(QStringLiteral("×"), m_form));
    size->addWidget(m_height);
    size->addStretch();
    m_fixedSeed = new QCheckBox(tr("Fixed seed"), m_form);
    m_fixedSeed->setToolTip(tr("The same seed every time keeps the character more consistent"));
    m_seed = new QSpinBox(m_form);
    m_seed->setRange(0, 2147483646);
    auto* seedRow = new QHBoxLayout;
    seedRow->addWidget(m_fixedSeed);
    seedRow->addWidget(m_seed, 1);
    m_style = new QSpinBox(m_form);
    m_style->setRange(0, 200);
    m_style->setSuffix(QStringLiteral(" %"));
    m_style->setToolTip(tr("Pixel Art XL style strength"));
    m_refStrength = new QSpinBox(m_form);
    m_refStrength->setRange(0, 100);
    m_refStrength->setSuffix(QStringLiteral(" %"));
    m_refStrength->setToolTip(tr("How closely results follow the reference images"));
    m_facing = new QComboBox(m_form);
    m_facing->addItem(tr("Right"), true);
    m_facing->addItem(tr("Left"), false);
    m_palette = new QLabel(m_form);
    m_palette->setMinimumHeight(20);
    auto* palBtn = new QPushButton(tr("From &Document"), m_form);
    palBtn->setToolTip(tr("Use the current document's palette"));
    auto* palRefs = new QPushButton(tr("From &References"), m_form);
    palRefs->setToolTip(tr("Use the colours of the reference images (up to 32)"));
    auto* palClear = new QPushButton(tr("Clear"), m_form);
    auto* palRow = new QHBoxLayout;
    palRow->addWidget(m_palette, 1);
    palRow->addWidget(palRefs);
    palRow->addWidget(palBtn);
    palRow->addWidget(palClear);
    m_refs = new QListWidget(m_form);
    m_refs->setAccessibleName(tr("Reference images"));
    m_refs->setViewMode(QListView::IconMode);
    m_refs->setIconSize(QSize(64, 64));
    m_refs->setFixedHeight(92);
    m_refs->setMovement(QListView::Static);
    auto* addLayer = new QPushButton(tr("Add Current &Layer"), m_form);
    auto* addFile = new QPushButton(tr("Add &Image…"), m_form);
    auto* removeRef = new QPushButton(tr("&Remove"), m_form);
    auto* refRow = new QHBoxLayout;
    refRow->addWidget(addLayer);
    refRow->addWidget(addFile);
    refRow->addWidget(removeRef);
    refRow->addStretch();
    m_notes = new QPlainTextEdit(m_form);
    m_notes->setFixedHeight(48);
    m_notes->setTabChangesFocus(true);

    auto* f = new QFormLayout(m_form);
    f->addRow(tr("Name:"), m_name);
    f->addRow(tr("Description:"), m_prompt);
    f->addRow(tr("Avoid:"), m_negative);
    f->addRow(tr("Sprite size:"), size);
    f->addRow(tr("Seed:"), seedRow);
    f->addRow(tr("Style strength:"), m_style);
    f->addRow(tr("Reference strength:"), m_refStrength);
    f->addRow(tr("Facing:"), m_facing);
    f->addRow(tr("Palette:"), palRow);
    f->addRow(tr("References:"), m_refs);
    f->addRow(QString(), refRow);
    f->addRow(tr("Notes:"), m_notes);

    auto* split = new QSplitter(this);
    split->addWidget(left);
    split->addWidget(m_form);
    split->setStretchFactor(1, 1);
    m_save = new QPushButton(tr("&Save"), this);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->addButton(m_save, QDialogButtonBox::ActionRole);
    connect(buttons, &QDialogButtonBox::rejected, this, [this] { if (maybeSave()) accept(); });
    connect(m_save, &QPushButton::clicked, this, [this] { saveCurrent(); });
    auto* root = new QVBoxLayout(this);
    root->addWidget(split, 1);
    root->addWidget(buttons);
    resize(760, 560);

    auto markDirty = [this] { if (!m_loading) { m_dirty = true; m_save->setEnabled(true); } };
    for (QLineEdit* e : {m_name, m_negative}) connect(e, &QLineEdit::textChanged, this, markDirty);
    for (QPlainTextEdit* e : {m_prompt, m_notes}) connect(e, &QPlainTextEdit::textChanged, this, markDirty);
    for (QSpinBox* s : {m_width, m_height, m_seed, m_style, m_refStrength}) connect(s, &QSpinBox::valueChanged, this, markDirty);
    connect(m_fixedSeed, &QCheckBox::toggled, this, [this, markDirty](bool on) { m_seed->setEnabled(on); markDirty(); });
    connect(m_facing, &QComboBox::currentIndexChanged, this, markDirty);

    connect(m_list, &QListWidget::currentRowChanged, this, [this](int row) {
        if (m_loading || row < 0) return;
        if (!maybeSave()) return;
        const fs::path p = toPath(m_list->item(row)->data(Qt::UserRole).toString());
        auto c = loadCharacter(p);
        if (!c) { QMessageBox::warning(this, windowTitle(), qs(c.error().message)); return; }
        showProfile(c.value());
    });
    connect(newBtn, &QPushButton::clicked, this, [this] {
        if (!maybeSave()) return;
        CharacterProfile c;
        c.name = tr("New character").toStdString();
        c.id = newCharacterId(c.name);
        if (m_view) {
            c.width = std::clamp(m_view->document().width(), 8, 1024);
            c.height = std::clamp(m_view->document().height(), 8, 1024);
            c.palette = m_view->document().palette();
        }
        if (auto st = saveCharacter(c, pathFor(m_dir, c)); !st) { QMessageBox::warning(this, windowTitle(), qs(st.error().message)); return; }
        reloadList(qs(c.id));
        m_name->setFocus();
        m_name->selectAll();
    });
    connect(dupBtn, &QPushButton::clicked, this, [this] {
        if (!m_current || !maybeSave()) return;
        CharacterProfile c = *m_current;
        c.name = (qs(c.name) + tr(" copy")).left(100).toStdString();
        c.id = newCharacterId(c.name);
        if (auto st = saveCharacter(c, pathFor(m_dir, c)); !st) { QMessageBox::warning(this, windowTitle(), qs(st.error().message)); return; }
        reloadList(qs(c.id));
    });
    connect(delBtn, &QPushButton::clicked, this, [this] {
        if (!m_current) return;
        if (QMessageBox::question(this, windowTitle(), tr("Delete the character “%1”? This cannot be undone.").arg(qs(m_current->name)))
            != QMessageBox::Yes)
            return;
        std::error_code err;
        fs::remove(pathFor(m_dir, *m_current), err);
        if (err) { QMessageBox::warning(this, windowTitle(), tr("Could not delete: %1").arg(qs(err.message()))); return; }
        m_current.reset();
        m_dirty = false;
        reloadList();
    });
    connect(palBtn, &QPushButton::clicked, this, [this] {
        if (!m_current || !m_view) return;
        m_current->palette = m_view->document().palette();
        refreshPalette();
        m_dirty = true;
        m_save->setEnabled(true);
    });
    connect(palRefs, &QPushButton::clicked, this, [this] {
        if (!m_current) return;
        if (m_current->references.empty()) {
            QMessageBox::information(this, windowTitle(), tr("Add a reference image first."));
            return;
        }
        // One strip of all references, so shared colours are counted together.
        int w = 0, h = 0;
        for (const Image& r : m_current->references) { w += r.width(); h = std::max(h, r.height()); }
        Image strip(w, h);
        int x = 0;
        for (const Image& r : m_current->references) { strip.blit(r, x, 0); x += r.width(); }
        Palette pal = extractPalette(strip, 32);
        if (pal.size() == 0) {
            QMessageBox::information(this, windowTitle(), tr("The reference images have no opaque colours."));
            return;
        }
        m_current->palette = std::move(pal);
        refreshPalette();
        m_dirty = true;
        m_save->setEnabled(true);
    });
    connect(palClear, &QPushButton::clicked, this, [this] {
        if (!m_current) return;
        m_current->palette = Palette();
        refreshPalette();
        m_dirty = true;
        m_save->setEnabled(true);
    });
    connect(addLayer, &QPushButton::clicked, this, [this] {
        if (!m_current || !m_view) return;
        if (m_current->references.size() >= static_cast<std::size_t>(CharacterProfile::kMaxReferences)) {
            QMessageBox::information(this, windowTitle(), tr("A character holds up to %1 reference images.").arg(CharacterProfile::kMaxReferences));
            return;
        }
        Image img = m_view->document().renderLayer(m_view->currentLayer(), m_view->currentFrame());
        const IntRect b = img.opaqueBounds();
        if (b.empty()) { QMessageBox::information(this, windowTitle(), tr("The current layer is empty on this frame.")); return; }
        m_current->references.push_back(img.copy(b));
        refreshReferences();
        m_dirty = true;
        m_save->setEnabled(true);
    });
    connect(addFile, &QPushButton::clicked, this, [this] {
        if (!m_current) return;
        const QString path = QFileDialog::getOpenFileName(this, tr("Reference Image"), QString(), tr("PNG images (*.png)"));
        if (path.isEmpty()) return;
        if (m_current->references.size() >= static_cast<std::size_t>(CharacterProfile::kMaxReferences)) {
            QMessageBox::information(this, windowTitle(), tr("A character holds up to %1 reference images.").arg(CharacterProfile::kMaxReferences));
            return;
        }
        auto img = readPng(toPath(path));
        if (!img) { QMessageBox::warning(this, windowTitle(), qs(img.error().message)); return; }
        if (img.value().width() > 2048 || img.value().height() > 2048) {
            QMessageBox::warning(this, windowTitle(), tr("Reference images can be at most 2048 × 2048 px."));
            return;
        }
        m_current->references.push_back(std::move(img).value());
        refreshReferences();
        m_dirty = true;
        m_save->setEnabled(true);
    });
    connect(removeRef, &QPushButton::clicked, this, [this] {
        const int r = m_refs->currentRow();
        if (!m_current || r < 0 || r >= static_cast<int>(m_current->references.size())) return;
        m_current->references.erase(m_current->references.begin() + r);
        refreshReferences();
        m_dirty = true;
        m_save->setEnabled(true);
    });

    reloadList();
}

void CharacterDialog::reloadList(const QString& selectId) {
    m_loading = true;
    m_list->clear();
    int select = -1;
    for (const auto& e : listCharacters(m_dir)) {
        auto* item = new QListWidgetItem(qs(e.name), m_list);
        item->setData(Qt::UserRole, fromPath(e.path));
        if (qs(e.id) == selectId || (selectId.isEmpty() && m_current && e.id == m_current->id)) select = m_list->count() - 1;
    }
    m_loading = false;
    m_form->setEnabled(m_list->count() > 0);
    if (m_list->count() == 0) { m_current.reset(); m_save->setEnabled(false); return; }
    m_list->setCurrentRow(select >= 0 ? select : 0);
    if (m_list->currentRow() == (select >= 0 ? select : 0)) {
        auto c = loadCharacter(toPath(m_list->currentItem()->data(Qt::UserRole).toString()));
        if (c) showProfile(c.value());
    }
}

void CharacterDialog::showProfile(const CharacterProfile& c) {
    m_loading = true;
    m_current = c;
    m_name->setText(qs(c.name));
    m_prompt->setPlainText(qs(c.prompt));
    m_negative->setText(qs(c.negative));
    m_width->setValue(c.width);
    m_height->setValue(c.height);
    m_fixedSeed->setChecked(c.seed >= 0);
    m_seed->setEnabled(c.seed >= 0);
    m_seed->setValue(c.seed >= 0 ? static_cast<int>(std::min<std::int64_t>(c.seed, 2147483646)) : 12345);
    m_style->setValue(c.loraStrength);
    m_refStrength->setValue(c.referenceStrength);
    m_facing->setCurrentIndex(c.facingRight ? 0 : 1);
    m_notes->setPlainText(qs(c.notes));
    refreshPalette();
    refreshReferences();
    m_loading = false;
    m_dirty = false;
    m_save->setEnabled(false);
}

CharacterProfile CharacterDialog::collect() const {
    CharacterProfile c = m_current.value_or(CharacterProfile{});
    c.name = m_name->text().trimmed().toStdString();
    c.prompt = m_prompt->toPlainText().trimmed().toStdString();
    c.negative = m_negative->text().trimmed().toStdString();
    c.width = m_width->value();
    c.height = m_height->value();
    c.seed = m_fixedSeed->isChecked() ? m_seed->value() : -1;
    c.loraStrength = m_style->value();
    c.referenceStrength = m_refStrength->value();
    c.facingRight = m_facing->currentData().toBool();
    c.notes = m_notes->toPlainText().toStdString();
    return c;
}

bool CharacterDialog::saveCurrent() {
    if (!m_current) return true;
    CharacterProfile c = collect();
    if (auto st = validateCharacter(c); !st) { QMessageBox::warning(this, windowTitle(), qs(st.error().message)); return false; }
    if (auto st = saveCharacter(c, pathFor(m_dir, c)); !st) { QMessageBox::warning(this, windowTitle(), qs(st.error().message)); return false; }
    m_current = c;
    m_dirty = false;
    m_save->setEnabled(false);
    // Keep the list's name in sync.
    if (QListWidgetItem* it = m_list->currentItem()) {
        m_loading = true;
        it->setText(qs(c.name));
        m_loading = false;
    }
    return true;
}

bool CharacterDialog::maybeSave() {
    if (!m_dirty || !m_current) return true;
    const auto r = QMessageBox::question(this, windowTitle(), tr("Save changes to “%1”?").arg(m_name->text()),
                                         QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (r == QMessageBox::Cancel) return false;
    if (r == QMessageBox::Save) return saveCurrent();
    m_dirty = false;
    return true;
}

void CharacterDialog::refreshReferences() {
    m_refs->clear();
    if (!m_current) return;
    for (std::size_t i = 0; i < m_current->references.size(); ++i) {
        const Image& r = m_current->references[i];
        auto* item = new QListWidgetItem(thumbnail(r), QStringLiteral("%1×%2").arg(r.width()).arg(r.height()), m_refs);
        item->setToolTip(tr("Reference %1").arg(i + 1));
    }
}

void CharacterDialog::refreshPalette() {
    if (!m_current || m_current->palette.size() == 0) {
        m_palette->setText(tr("(none: results keep their own colours)"));
        m_palette->setPixmap(QPixmap());
        return;
    }
    const auto& cols = m_current->palette.colors();
    const int n = static_cast<int>(cols.size()), cell = 12, perRow = 20;
    const int rows = (n + perRow - 1) / perRow;
    QPixmap pm(std::min(n, perRow) * cell, rows * cell);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    for (int i = 0; i < n; ++i) p.fillRect((i % perRow) * cell, (i / perRow) * cell, cell - 1, cell - 1, toQColor(cols[static_cast<std::size_t>(i)]));
    p.end();
    m_palette->setPixmap(pm);
    m_palette->setToolTip(tr("%n colour(s)", nullptr, n));
}

std::optional<fs::path> CharacterDialog::selectedPath() const {
    if (!m_current) return std::nullopt;
    return pathFor(m_dir, *m_current);
}

} // namespace acestudio::ui
