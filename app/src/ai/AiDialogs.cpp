#include "AiDialogs.h"

#include "AiService.h"
#include "AppContext.h"
#include "QtBridge.h"

#include "acestudio/core/AiWorker.h"

#include <QCheckBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace acestudio::ui {

using namespace acestudio::ai;

QString formatBytes(std::uint64_t bytes) {
    if (bytes == 0) return QObject::tr("unknown size");
    const double v = static_cast<double>(bytes);
    if (v >= 1e9) return QObject::tr("%1 GB").arg(v / 1e9, 0, 'f', 2);
    if (v >= 1e6) return QObject::tr("%1 MB").arg(v / 1e6, 0, 'f', 1);
    return QObject::tr("%1 KB").arg(v / 1e3, 0, 'f', 0);
}

// ------------------------------------------------------------------ setup

AiSetupDialog::AiSetupDialog(AiService& ai, AppContext& ctx, QWidget* parent) : QDialog(parent), m_ai(ai), m_ctx(ctx) {
    setWindowTitle(tr("Set Up AI Runtime"));
    auto* intro = new QLabel(tr("AceStudio runs AI models locally in a separate Python process. Setup creates a private "
                                "Python environment and installs the packages below from the Python Package Index "
                                "(and PyTorch from download.pytorch.org). Nothing is installed system-wide, and no "
                                "models are downloaded: you choose those later in Models…"), this);
    intro->setWordWrap(true);

    m_python = new QLabel(this);
    m_python->setWordWrap(true);
    m_pythonPath = new QLineEdit(qs(ctx.settings().get<std::string>("ai.pythonPath")), this);
    m_pythonPath->setPlaceholderText(tr("Automatic (Python 3.10 or newer on PATH)"));
    auto* browse = new QPushButton(tr("Browse…"), this);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString f = QFileDialog::getOpenFileName(this, tr("Python Interpreter"));
        if (!f.isEmpty()) { m_pythonPath->setText(QDir::toNativeSeparators(f)); detectPython(); }
    });
    connect(m_pythonPath, &QLineEdit::editingFinished, this, &AiSetupDialog::detectPython);
    auto* pyRow = new QHBoxLayout;
    pyRow->addWidget(m_pythonPath, 1);
    pyRow->addWidget(browse);

    auto* box = new QGroupBox(tr("Components"), this);
    m_onnx = new QCheckBox(tr("Background removal (BiRefNet) — ONNX Runtime, roughly 50 MB"), box);
    m_onnx->setChecked(true);
    m_diffusion = new QCheckBox(tr("Image generation (Stable Diffusion XL) and AI upscaling — PyTorch + Diffusers"), box);
    m_cuda = new QRadioButton(tr("NVIDIA GPU (CUDA) — roughly 3 GB download, needs a recent NVIDIA driver"), box);
    m_cpu = new QRadioButton(tr("CPU only — smaller download; generation is very slow"), box);
    m_cuda->setChecked(true);
    for (QRadioButton* r : {m_cuda, m_cpu}) r->setEnabled(false);
    connect(m_diffusion, &QCheckBox::toggled, this, [this](bool on) { m_cuda->setEnabled(on); m_cpu->setEnabled(on); });
    auto* bl = new QVBoxLayout(box);
    bl->addWidget(m_onnx);
    bl->addWidget(m_diffusion);
    auto* sub = new QVBoxLayout;
    sub->setContentsMargins(24, 0, 0, 0);
    sub->addWidget(m_cuda);
    sub->addWidget(m_cpu);
    bl->addLayout(sub);

    auto* where = new QLabel(this);
    where->setWordWrap(true);
    auto updateWhere = [this, where] {
        if (!m_ai.environment()) return where->setText(m_ai.environmentError());
        if (m_pythonPath->text().trimmed().isEmpty())
            where->setText(tr("Installed into a private environment: %1").arg(QDir::toNativeSeparators(fromPath(m_ai.environment()->venvDir))));
        else
            where->setText(tr("<b>Packages will be installed into the Python you chose above</b>, changing that "
                              "environment. Leave the field empty to use a private environment instead."));
    };
    connect(m_pythonPath, &QLineEdit::textChanged, this, updateWhere);
    updateWhere();
    where->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_install = buttons->addButton(tr("&Install"), QDialogButtonBox::AcceptRole);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (profiles().isEmpty()) return;
        (void)m_ctx.settings().set("ai.pythonPath", QDir::fromNativeSeparators(m_pythonPath->text().trimmed()).toStdString());
        m_ctx.saveSettings();
        accept();
    });

    auto* form = new QFormLayout;
    form->addRow(tr("Python:"), pyRow);
    form->addRow(QString(), m_python);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addLayout(form);
    layout->addWidget(box);
    layout->addWidget(where);
    layout->addWidget(buttons);
    setMinimumWidth(560);
    detectPython();
}

void AiSetupDialog::detectPython() {
    const QString configured = m_pythonPath->text().trimmed();
    auto py = findPython(configured.isEmpty() ? std::filesystem::path() : toPath(configured));
    if (py) {
        m_python->setText(tr("Found Python %1 at %2").arg(qs(py.value().version), QDir::toNativeSeparators(fromPath(py.value().executable))));
        m_install->setEnabled(true);
    } else {
        m_python->setText(tr("<b>No Python 3.10+ found.</b> Install Python from "
                             "<a href=\"https://www.python.org/downloads/\">python.org</a> (tick “Add to PATH”), "
                             "or choose an interpreter above.<br><small>%1</small>").arg(qs(py.error().message).toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"))));
        m_python->setOpenExternalLinks(true);
        m_install->setEnabled(false);
    }
}

QStringList AiSetupDialog::profiles() const {
    QStringList p;
    if (m_onnx->isChecked()) p << AiService::defaultProfiles(false, false);
    if (m_diffusion->isChecked()) p << (m_cuda->isChecked() ? QStringLiteral("diffusion-cuda") : QStringLiteral("diffusion-cpu"));
    return p;
}

// ---------------------------------------------------------------- licence

LicenseDialog::LicenseDialog(const ModelEntry& m, QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Licence — %1").arg(qs(m.name)));
    QString html = tr("<h3>%1</h3><p>%2</p>").arg(qs(m.name).toHtmlEscaped(), qs(m.description).toHtmlEscaped());
    html += tr("<p><b>Licence:</b> <a href=\"%1\">%2</a><br><b>Commercial use:</b> %3<br><b>Download:</b> %4<br>"
               "<b>Source:</b> <a href=\"%5\">%5</a><br><b>Copyright:</b> %6</p>")
                .arg(qs(m.license.url).toHtmlEscaped(), qs(m.license.name).toHtmlEscaped(),
                     m.license.commercialUse ? tr("allowed") : tr("<b>not allowed</b>"), formatBytes(m.knownSize()),
                     qs(m.source).toHtmlEscaped(), qs(m.copyright).toHtmlEscaped());
    html += tr("<p>%1</p><p><b>Your obligations:</b> %2</p>").arg(qs(m.license.summary).toHtmlEscaped(), qs(m.license.obligations).toHtmlEscaped());
    if (!m.pinned)
        html += tr("<p><i>AceStudio could not pre-verify these files. Their SHA-256 checksums are recorded on this "
                   "first download and checked on every later download.</i></p>");
    if (!m.fileListVerified)
        html += tr("<p><i>The file list for this model has not yet been confirmed against the repository; if a file "
                   "is missing, the download reports it.</i></p>");
    auto* text = new QLabel(html, this);
    text->setWordWrap(true);
    text->setOpenExternalLinks(true);
    text->setTextInteractionFlags(Qt::TextBrowserInteraction);
    auto* accept = new QCheckBox(tr("I have read the licence and accept its terms"), this);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    QPushButton* go = buttons->addButton(tr("Accept and &Download"), QDialogButtonBox::AcceptRole);
    go->setEnabled(false);
    connect(accept, &QCheckBox::toggled, go, &QPushButton::setEnabled);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(text);
    layout->addWidget(accept);
    layout->addWidget(buttons);
    setMinimumWidth(520);
}

// ----------------------------------------------------------------- models

ModelsDialog::ModelsDialog(AiService& ai, QWidget* parent) : QDialog(parent), m_ai(ai) {
    setWindowTitle(tr("AI Models"));
    resize(820, 420);
    m_table = new QTableWidget(0, 5, this);
    m_table->setHorizontalHeaderLabels({tr("Model"), tr("Used for"), tr("Licence"), tr("Size"), tr("Status")});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->hide();
    m_details = new QLabel(this);
    m_details->setWordWrap(true);
    m_details->setMinimumHeight(48);
    m_download = new QPushButton(tr("&Download…"), this);
    m_remove = new QPushButton(tr("&Remove"), this);
    m_download->setEnabled(false);
    m_remove->setEnabled(false);
    auto* folder = new QPushButton(tr("Open &Folder"), this);
    connect(m_download, &QPushButton::clicked, this, &ModelsDialog::download);
    connect(m_remove, &QPushButton::clicked, this, &ModelsDialog::remove);
    connect(folder, &QPushButton::clicked, this, [this] {
        if (m_ai.store()) {
            std::error_code ec;
            std::filesystem::create_directories(m_ai.store()->root(), ec);
            QDesktopServices::openUrl(QUrl::fromLocalFile(fromPath(m_ai.store()->root())));
        }
    });
    connect(m_table, &QTableWidget::itemSelectionChanged, this, [this] {
        const ModelEntry* m = m_table->selectedItems().isEmpty() ? nullptr : selected();
        m_details->setText(m ? qs(m->description) + (m->vramMb ? tr("  Estimated GPU memory: ~%1 GB (not yet measured).").arg(m->vramMb / 1024.0, 0, 'f', 1) : QString()) : QString());
        const InstallState st = m && m_ai.store() ? m_ai.store()->state(*m) : InstallState::NotInstalled;
        m_download->setEnabled(m && st != InstallState::Installed);
        m_download->setText(st == InstallState::Partial ? tr("&Resume Download") : tr("&Download…"));
        m_remove->setEnabled(m && st != InstallState::NotInstalled);
    });
    connect(&m_ai, &AiService::modelsChanged, this, &ModelsDialog::rebuild);
    connect(&m_ai, &AiService::jobChanged, this, &ModelsDialog::rebuild);
    auto* side = new QVBoxLayout;
    side->addWidget(m_download);
    side->addWidget(m_remove);
    side->addWidget(folder);
    side->addStretch();
    auto* row = new QHBoxLayout;
    row->addWidget(m_table, 1);
    row->addLayout(side);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    auto* note = new QLabel(tr("Only models whose licences allow commercial use are listed. Downloads run in the "
                               "background (see the AI Studio queue) and can be resumed."), this);
    note->setWordWrap(true);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(note);
    layout->addLayout(row);
    layout->addWidget(m_details);
    layout->addWidget(buttons);
    rebuild();
}

void ModelsDialog::rebuild() {
    const int keep = m_table->currentRow();
    m_table->setRowCount(0);
    if (!m_ai.catalog() || !m_ai.store()) return;
    // A running download for a model shows its progress in the Status column.
    std::map<std::string, const Job*> active;
    if (auto* sup = m_ai.supervisor())
        for (const Job* j : sup->jobs())
            if (j->task == "model.download" && !isFinished(j->state)) active[j->params.value("model_id", std::string())] = j;
    for (const auto& m : m_ai.catalog()->models()) {
        const int r = m_table->rowCount();
        m_table->insertRow(r);
        QString status = qs(toString(m_ai.store()->state(m)));
        if (auto it = active.find(m.id); it != active.end())
            status = it->second->progress >= 0 ? tr("downloading %1%").arg(qRound(it->second->progress * 100)) : tr("downloading…");
        const QString used = m.task == "background.remove" ? tr("Remove background")
                           : m.task == "upscale.esrgan"    ? tr("Upscale")
                           : m.kind == "lora"              ? tr("Generation style")
                                                           : tr("Generate");
        const QStringList cells{qs(m.name), used, qs(m.license.name), formatBytes(m.knownSize()), status};
        for (int c = 0; c < cells.size(); ++c) m_table->setItem(r, c, new QTableWidgetItem(cells[c]));
    }
    m_table->resizeColumnsToContents();
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    if (keep >= 0 && keep < m_table->rowCount()) m_table->selectRow(keep);
    else m_table->clearSelection();
    Q_EMIT m_table->itemSelectionChanged();   // refresh button states
}

const ModelEntry* ModelsDialog::selected() const {
    const int r = m_table->currentRow();
    if (!m_ai.catalog() || r < 0 || r >= static_cast<int>(m_ai.catalog()->models().size())) return nullptr;
    return &m_ai.catalog()->models()[static_cast<std::size_t>(r)];
}

void ModelsDialog::download() {
    const ModelEntry* m = selected();
    if (!m || !m_ai.store()) return;
    if (!m_ai.store()->isApproved(*m)) {
        LicenseDialog dlg(*m, this);
        if (dlg.exec() != QDialog::Accepted) return;
        if (auto st = m_ai.store()->approve(*m); !st) return void(QMessageBox::warning(this, windowTitle(), qs(st.error().message)));
    }
    auto id = m_ai.download(*m);
    if (!id) QMessageBox::warning(this, windowTitle(), qs(id.error().message));
    rebuild();
}

void ModelsDialog::remove() {
    const ModelEntry* m = selected();
    if (!m || !m_ai.store()) return;
    if (QMessageBox::question(this, windowTitle(), tr("Delete the files of %1 (%2)?").arg(qs(m->name), formatBytes(m_ai.store()->bytesOnDisk(*m))),
                              QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    if (auto st = m_ai.store()->remove(*m); !st) QMessageBox::warning(this, windowTitle(), qs(st.error().message));
    rebuild();
}

} // namespace acestudio::ui
