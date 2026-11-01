#include "ExportDialog.h"
#include "QtBridge.h"
#include "assets/AssetCommon.h"

#include "acestudio/core/Export.h"
#include "acestudio/core/Log.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <filesystem>

namespace acestudio::ui {

namespace fs = std::filesystem;

namespace {
QString extensionFor(ExportKind k) {
    switch (k) {
    case ExportKind::SpriteSheet: return QStringLiteral(".png");
    case ExportKind::Gif: return QStringLiteral(".gif");
    case ExportKind::Apng: return QStringLiteral(".png");
    case ExportKind::Webp: return QStringLiteral(".webp");
    case ExportKind::PngSequence: return QString();
    }
    return {};
}
constexpr int kAllFrames = -1;
constexpr int kCustom = -2;
} // namespace

ExportDialog::ExportDialog(ExportKind kind, const Document& doc, int currentFrame, QString suggestedPath,
                           const nlohmann::json& preset, QWidget* parent)
    : QDialog(parent), m_kind(kind), m_doc(doc) {
    setWindowTitle(tr("Export %1").arg(kindTitle()));
    const bool animated = kind == ExportKind::Gif || kind == ExportKind::Apng || kind == ExportKind::Webp;
    const bool sheet = kind == ExportKind::SpriteSheet;

    // Frames
    m_range = new QComboBox(this);
    m_range->addItem(tr("All frames"), kAllFrames);
    for (std::size_t i = 0; i < doc.tags().size(); ++i)
        m_range->addItem(tr("Tag: %1").arg(qs(doc.tags()[i].name)), static_cast<int>(i));
    m_range->addItem(tr("Custom range"), kCustom);
    m_from = new QSpinBox(this);
    m_to = new QSpinBox(this);
    for (QSpinBox* s : {m_from, m_to}) s->setRange(1, doc.frameCount());
    m_from->setValue(1);
    m_to->setValue(doc.frameCount());
    (void)currentFrame;
    connect(m_range, &QComboBox::currentIndexChanged, this, &ExportDialog::updateRangeControls);
    connect(m_from, &QSpinBox::valueChanged, this, [this](int v) { if (m_to->value() < v) m_to->setValue(v); updateSummary(); });
    connect(m_to, &QSpinBox::valueChanged, this, [this](int v) { if (m_from->value() > v) m_from->setValue(v); updateSummary(); });
    auto* rangeRow = new QHBoxLayout;
    rangeRow->addWidget(m_from);
    rangeRow->addWidget(new QLabel(tr("to"), this));
    rangeRow->addWidget(m_to);
    rangeRow->addStretch();

    m_scale = new QSpinBox(this);
    m_scale->setRange(1, 32);
    m_scale->setSuffix(QStringLiteral("×"));
    connect(m_scale, &QSpinBox::valueChanged, this, &ExportDialog::updateSummary);

    auto* general = new QFormLayout;
    general->addRow(tr("&Frames:"), m_range);
    general->addRow(QString(), rangeRow);
    general->addRow(tr("&Scale:"), m_scale);
    if (animated) {
        m_loop = new QSpinBox(this);
        m_loop->setRange(0, 65535);
        m_loop->setSpecialValueText(tr("Forever"));
        m_loop->setSuffix(tr(" times"));
        general->addRow(tr("&Loop:"), m_loop);
    }

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(general);

    if (sheet) {
        auto* box = new QGroupBox(tr("Sheet"), this);
        auto* form = new QFormLayout(box);
        m_layout = new QComboBox(box);
        m_layout->addItem(tr("Horizontal strip"), static_cast<int>(SheetLayout::Horizontal));
        m_layout->addItem(tr("Vertical strip"), static_cast<int>(SheetLayout::Vertical));
        m_layout->addItem(tr("Grid"), static_cast<int>(SheetLayout::Grid));
        m_layout->addItem(tr("Packed"), static_cast<int>(SheetLayout::Packed));
        m_columns = new QSpinBox(box);
        m_columns->setRange(0, 1024);
        m_columns->setSpecialValueText(tr("Automatic"));
        m_border = new QSpinBox(box);
        m_spacing = new QSpinBox(box);
        m_inner = new QSpinBox(box);
        for (QSpinBox* s : {m_border, m_spacing, m_inner}) { s->setRange(0, 64); s->setSuffix(tr(" px")); }
        m_trim = new QCheckBox(tr("Trim transparent edges of each frame"), box);
        m_merge = new QCheckBox(tr("Merge identical frames"), box);
        m_pot = new QCheckBox(tr("Power-of-two sheet size"), box);
        m_json = new QComboBox(box);
        m_json->addItem(tr("JSON (hash) — Aseprite / Phaser"), 0);
        m_json->addItem(tr("JSON (array) — Aseprite / Phaser"), 1);
        m_json->addItem(tr("No data file"), 2);
        form->addRow(tr("La&yout:"), m_layout);
        form->addRow(tr("&Columns:"), m_columns);
        form->addRow(tr("&Border padding:"), m_border);
        form->addRow(tr("S&pacing:"), m_spacing);
        form->addRow(tr("&Inner padding:"), m_inner);
        form->addRow(QString(), m_trim);
        form->addRow(QString(), m_merge);
        form->addRow(QString(), m_pot);
        form->addRow(tr("&Data:"), m_json);
        m_engines = new EngineChoice(kEngineGodot | kEngineUnity, box);
        form->addRow(tr("&Engines:"), m_engines);
        connect(m_layout, &QComboBox::currentIndexChanged, this, [this] {
            m_columns->setEnabled(m_layout->currentData().toInt() == static_cast<int>(SheetLayout::Grid));
            updateSummary();
        });
        m_columns->setEnabled(false);
        layout->addWidget(box);
    }

    // Output
    m_path = new QLineEdit(this);
    if (!suggestedPath.isEmpty()) {
        const QFileInfo fi(suggestedPath);
        const QString base = fi.absolutePath() + QLatin1Char('/') + fi.completeBaseName();
        m_path->setText(QDir::toNativeSeparators(kind == ExportKind::PngSequence ? base + QStringLiteral("_frames")
                                                 : kind == ExportKind::Apng      ? base + QStringLiteral(".apng.png")
                                                                                 : base + extensionFor(kind)));
    }
    auto* browseBtn = new QPushButton(tr("&Browse…"), this);
    connect(browseBtn, &QPushButton::clicked, this, &ExportDialog::browse);
    auto* pathRow = new QHBoxLayout;
    pathRow->addWidget(m_path, 1);
    pathRow->addWidget(browseBtn);
    auto* out = new QFormLayout;
    auto* pathLabel = new QLabel(kind == ExportKind::PngSequence ? tr("&Folder:") : tr("&File:"), this);
    pathLabel->setBuddy(m_path);
    out->addRow(pathLabel, pathRow);
    layout->addLayout(out);

    m_summary = new QLabel(this);
    m_summary->setWordWrap(true);
    layout->addWidget(m_summary);
    if (kind == ExportKind::Gif) {
        auto* note = new QLabel(tr("GIF stores 1-bit transparency, at most 256 colours per frame and durations in 10 ms steps. "
                                   "Use APNG or WebP for exact colours and alpha."), this);
        note->setWordWrap(true);
        layout->addWidget(note);
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    buttons->addButton(tr("&Export"), QDialogButtonBox::AcceptRole);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] { if (runExport()) accept(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    // Default to the tag under the playhead, which is what people usually export.
    for (std::size_t i = 0; i < doc.tags().size(); ++i)
        if (currentFrame >= doc.tags()[i].fromFrame && currentFrame <= doc.tags()[i].toFrame) {
            m_range->setCurrentIndex(static_cast<int>(i) + 1);
            break;
        }
    applyPreset(preset);
    updateRangeControls();
    setMinimumWidth(460);
}

QString ExportDialog::kindTitle() const {
    switch (m_kind) {
    case ExportKind::SpriteSheet: return tr("Sprite Sheet");
    case ExportKind::Gif: return tr("Animated GIF");
    case ExportKind::Apng: return tr("Animated PNG");
    case ExportKind::Webp: return tr("Animated WebP");
    case ExportKind::PngSequence: return tr("PNG Sequence");
    }
    return {};
}

QString ExportDialog::outputPath() const { return QDir::fromNativeSeparators(m_path->text().trimmed()); }

void ExportDialog::browse() {
    QString path;
    if (m_kind == ExportKind::PngSequence) {
        path = QFileDialog::getExistingDirectory(this, tr("Export Folder"), outputPath());
    } else {
        const QString filter = m_kind == ExportKind::Gif    ? tr("GIF Images (*.gif)")
                             : m_kind == ExportKind::Webp   ? tr("WebP Images (*.webp)")
                                                            : tr("PNG Images (*.png)");
        path = QFileDialog::getSaveFileName(this, windowTitle(), outputPath(), filter, nullptr,
                                            QFileDialog::DontConfirmOverwrite);   // we ask ourselves, for JSON too
    }
    if (!path.isEmpty()) m_path->setText(QDir::toNativeSeparators(path));
}

void ExportDialog::updateRangeControls() {
    const int sel = m_range->currentData().toInt();
    m_from->setEnabled(sel == kCustom);
    m_to->setEnabled(sel == kCustom);
    if (sel >= 0) {
        const auto& t = m_doc.tags()[static_cast<std::size_t>(sel)];
        m_from->setValue(t.fromFrame + 1);
        m_to->setValue(t.toFrame + 1);
    } else if (sel == kAllFrames) {
        m_from->setValue(1);
        m_to->setValue(m_doc.frameCount());
    }
    updateSummary();
}

void ExportDialog::updateSummary() {
    const int n = m_to->value() - m_from->value() + 1;
    const int s = m_scale->value();
    QString text = tr("%n frame(s) at %1 × %2 px", nullptr, n).arg(m_doc.width() * s).arg(m_doc.height() * s);
    const int sel = m_range->currentData().toInt();
    if (sel >= 0 && m_kind != ExportKind::SpriteSheet && m_kind != ExportKind::PngSequence)
        text += tr(" — played in the tag's direction");
    m_summary->setText(text);
}

QString ExportDialog::kindKey(ExportKind k) {
    switch (k) {
    case ExportKind::SpriteSheet: return QStringLiteral("sheet");
    case ExportKind::Gif: return QStringLiteral("gif");
    case ExportKind::Apng: return QStringLiteral("apng");
    case ExportKind::Webp: return QStringLiteral("webp");
    case ExportKind::PngSequence: return QStringLiteral("sequence");
    }
    return {};
}

nlohmann::json ExportDialog::preset() const {
    nlohmann::json p = {{"scale", m_scale->value()}};
    if (m_loop) p["loop"] = m_loop->value();
    if (m_layout) {
        p["layout"] = m_layout->currentData().toInt();
        p["columns"] = m_columns->value();
        p["border"] = m_border->value();
        p["spacing"] = m_spacing->value();
        p["inner"] = m_inner->value();
        p["trim"] = m_trim->isChecked();
        p["merge"] = m_merge->isChecked();
        p["pot"] = m_pot->isChecked();
        p["data"] = m_json->currentData().toInt();
    }
    const QFileInfo fi(outputPath());
    if (!outputPath().isEmpty()) p["folder"] = fi.absolutePath().toStdString();   // parent of the file or sequence folder
    return p;
}

void ExportDialog::applyPreset(const nlohmann::json& p) {
    if (!p.is_object()) return;
    // Tolerant: a missing or mistyped value keeps the default.
    auto num = [&](const char* k, QSpinBox* box) {
        if (box && p.contains(k) && p[k].is_number_integer()) box->setValue(p[k].get<int>());
    };
    auto flag = [&](const char* k, QCheckBox* box) {
        if (box && p.contains(k) && p[k].is_boolean()) box->setChecked(p[k].get<bool>());
    };
    auto pick = [&](const char* k, QComboBox* box) {
        if (!box || !p.contains(k) || !p[k].is_number_integer()) return;
        const int i = box->findData(p[k].get<int>());
        if (i >= 0) box->setCurrentIndex(i);
    };
    num("scale", m_scale);
    num("loop", m_loop);
    pick("layout", m_layout);
    num("columns", m_columns);
    num("border", m_border);
    num("spacing", m_spacing);
    num("inner", m_inner);
    flag("trim", m_trim);
    flag("merge", m_merge);
    flag("pot", m_pot);
    pick("data", m_json);
}

bool ExportDialog::runExport() {
    const QString path = outputPath();
    if (path.isEmpty()) {
        QMessageBox::warning(this, windowTitle(), tr("Choose where to export to."));
        return false;
    }
    FrameRange range;
    const int sel = m_range->currentData().toInt();
    if (sel >= 0) range.tag = m_doc.tags()[static_cast<std::size_t>(sel)].name;
    else { range.from = m_from->value() - 1; range.to = m_to->value() - 1; }
    const int scale = m_scale->value();
    const fs::path target = toPath(path);

    // Work out every file that would be replaced and ask once.
    QStringList existing;
    std::error_code ec;
    SheetOptions so;
    std::optional<JsonStyle> json;
    fs::path sheetPng = target;
    if (m_kind == ExportKind::SpriteSheet) {
        so.layout = static_cast<SheetLayout>(m_layout->currentData().toInt());
        so.columns = m_columns->value();
        so.borderPadding = m_border->value();
        so.spacing = m_spacing->value();
        so.innerPadding = m_inner->value();
        so.trim = m_trim->isChecked();
        so.mergeDuplicates = m_merge->isChecked();
        so.powerOfTwo = m_pot->isChecked();
        so.scale = scale;
        const int j = m_json->currentData().toInt();
        if (j == 0) json = JsonStyle::Hash;
        else if (j == 1) json = JsonStyle::Array;
        if (sheetPng.extension() != ".png") sheetPng += ".png";
        if (fs::exists(sheetPng, ec)) existing << fromPath(sheetPng);
        if (json) {
            fs::path j2 = sheetPng;
            j2.replace_extension(".json");
            if (fs::exists(j2, ec)) existing << fromPath(j2);
        }
        if (m_engines->engines() != kEngineNone) {
            AssetWriteOptions dry;
            dry.engines = m_engines->engines();
            dry.engineFilesOnly = true;
            dry.dryRun = true;
            auto plan = writeSheetFiles(m_doc, range, so, sheetPng, dry);
            if (!plan) { QMessageBox::critical(this, windowTitle(), qs(plan.error().message)); return false; }
            for (const auto& p : plan.value().existing) existing << fromPath(p);
        }
    } else if (m_kind == ExportKind::PngSequence) {
        if (fs::exists(target, ec) && !fs::is_directory(target, ec)) {
            QMessageBox::warning(this, windowTitle(), tr("%1 is a file, not a folder.").arg(path));
            return false;
        }
        auto paths = pngSequencePaths(m_doc, range, target, target.filename().string());
        if (!paths) { QMessageBox::critical(this, windowTitle(), qs(paths.error().message)); return false; }
        for (const auto& p : paths.value())
            if (fs::exists(p, ec)) existing << fromPath(p);
    } else if (fs::exists(target, ec)) {
        existing << fromPath(target);
    }
    bool overwrite = false;
    if (!existing.isEmpty()) {
        const QString list = existing.size() > 4 ? tr("%n file(s)", nullptr, static_cast<int>(existing.size()))
                                                 : existing.join(QLatin1Char('\n'));
        if (QMessageBox::question(this, windowTitle(), tr("Replace existing files?\n\n%1").arg(list),
                                  QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
            return false;
        overwrite = true;
    }

    auto fail = [this](const Error& e) {
        ACE_LOG_WARN("export", e.message);
        QMessageBox::critical(this, windowTitle(), qs(e.message));
        return false;
    };
    switch (m_kind) {
    case ExportKind::SpriteSheet: {
        auto r = writeSpriteSheet(m_doc, range, so, target, json, overwrite);
        if (!r) return fail(r.error());
        m_result = tr("Exported %n frame(s) to %1 (%2 × %3)", nullptr, static_cast<int>(r.value().frames))
                       .arg(QDir::toNativeSeparators(fromPath(r.value().image))).arg(r.value().width).arg(r.value().height);
        if (m_engines->engines() != kEngineNone) {
            AssetWriteOptions eo;
            eo.engines = m_engines->engines();
            eo.engineFilesOnly = true;
            eo.overwrite = true;   // confirmed above
            auto e = writeSheetFiles(m_doc, range, so, sheetPng, eo);
            if (!e) return fail(e.error());
            m_result += tr(", plus %n engine file(s)", nullptr, static_cast<int>(e.value().files.size()));
        }
        break;
    }
    case ExportKind::PngSequence: {
        fs::create_directories(target, ec);
        if (ec) return fail(Error{ErrorCode::IoError, "Cannot create " + target.string() + ": " + ec.message()});
        auto r = exportPngSequence(m_doc, range, target, target.filename().string(), scale, overwrite);
        if (!r) return fail(r.error());
        m_result = tr("Exported %n PNG file(s) to %1", nullptr, static_cast<int>(r.value().size())).arg(QDir::toNativeSeparators(path));
        break;
    }
    default: {
        const AnimFormat f = m_kind == ExportKind::Gif ? AnimFormat::Gif : m_kind == ExportKind::Apng ? AnimFormat::Apng : AnimFormat::Webp;
        auto r = writeAnimation(m_doc, range, f, target, scale, m_loop->value(), overwrite);
        if (!r) return fail(r.error());
        m_result = tr("Exported %n frame(s) to %1", nullptr, static_cast<int>(r.value())).arg(QDir::toNativeSeparators(path));
        break;
    }
    }
    ACE_LOG_INFO("export", m_result.toStdString());
    return true;
}

} // namespace acestudio::ui
