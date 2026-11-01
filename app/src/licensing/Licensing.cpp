#include "Licensing.h"

#include "../QtBridge.h"

#include "acestudio/core/Log.h"

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFontDatabase>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <fstream>

namespace acestudio::ui {

namespace fs = std::filesystem;

namespace {
fs::path s_dataDir;
LicenceStatus s_status;
bool useRegistry() { return !qEnvironmentVariableIsSet("ACESTUDIO_DATA_DIR"); }   // tests keep the registry out
} // namespace

void initLicensing(const fs::path& dataDir) {
    s_dataDir = dataDir;
    refreshLicence();
    Logger::instance().log(LogLevel::Info, "licence", describeLicence(s_status));
}

void refreshLicence() { s_status = licenceStatus(s_dataDir, std::chrono::system_clock::now(), useRegistry()); }

const LicenceStatus& currentLicence() { return s_status; }

std::string licenceReadOnlyReason() { return writesAllowed(s_status) ? std::string() : licenceRequiredMessage("Writing files"); }

bool licenceAllows(QWidget* parent, const QString& action) {
    if (writesAllowed(s_status)) return true;
    refreshLicence();   // a key may have been installed by the CLI meanwhile
    if (writesAllowed(s_status)) return true;
    QMessageBox box(parent);
    box.setIcon(QMessageBox::Information);
    box.setWindowTitle(QObject::tr("Licence Needed"));
    box.setText(QObject::tr("%1 needs a licence key: the %2-day trial has ended.").arg(action).arg(kTrialDays));
    box.setInformativeText(QObject::tr("You can still open, view and edit your work, and nothing is lost: autosave and crash "
                                       "recovery keep running. Enter a key to save and export again."));
    QPushButton* enter = box.addButton(QObject::tr("Enter Licence Key…"), QMessageBox::AcceptRole);
    box.addButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != enter) return false;
    LicenceDialog dlg(parent);
    dlg.exec();
    return writesAllowed(s_status);
}

LicenceDialog::LicenceDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Licence"));
    resize(560, 360);
    auto* lay = new QVBoxLayout(this);
    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lay->addWidget(m_status);
    auto* label = new QLabel(tr("&Licence key (paste it, or load the file you received):"), this);
    lay->addWidget(label);
    m_key = new QPlainTextEdit(this);
    m_key->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_key->setPlaceholderText(QStringLiteral("ACESTUDIO-1.…"));
    m_key->setAccessibleName(tr("Licence key"));
    m_key->setTabChangesFocus(true);
    label->setBuddy(m_key);
    lay->addWidget(m_key, 1);
    auto* buttons = new QDialogButtonBox(this);
    QPushButton* load = buttons->addButton(tr("Load &File…"), QDialogButtonBox::ActionRole);
    m_install = buttons->addButton(tr("&Install Key"), QDialogButtonBox::ActionRole);
    m_remove = buttons->addButton(tr("&Remove Key"), QDialogButtonBox::ActionRole);
    buttons->addButton(QDialogButtonBox::Close);
    lay->addWidget(buttons);
    connect(load, &QPushButton::clicked, this, [this] {
        const QString f = QFileDialog::getOpenFileName(this, tr("Load Licence Key"), QString(), tr("Licence keys (*.key *.txt);;All files (*)"));
        if (f.isEmpty()) return;
        std::ifstream in(toPath(f), std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), {});
        if (text.size() > 65536) text.clear();
        m_key->setPlainText(qs(text));
    });
    connect(m_install, &QPushButton::clicked, this, &LicenceDialog::install);
    connect(m_remove, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, windowTitle(), tr("Remove the licence key from this computer? You can enter it again later.")) !=
            QMessageBox::Yes)
            return;
        if (auto s = removeLicenceKey(s_dataDir); !s) QMessageBox::warning(this, windowTitle(), qs(s.error().message));
        refreshLicence();
        updateStatus();
        Q_EMIT licenceChanged();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    updateStatus();
}

void LicenceDialog::updateStatus() {
    const LicenceStatus& s = currentLicence();
    QString text = QStringLiteral("<b>%1</b>").arg(qs(describeLicence(s)).toHtmlEscaped());
    if (s.mode == LicenceMode::Licensed && !s.key->id.empty()) text += tr("<br>Key %1, issued %2.").arg(qs(s.key->id), qs(s.key->issued));
    if (s.mode != LicenceMode::Licensed)
        text += tr("<br>The trial started on %1. After %2 days, saving and exporting need a key; your work always stays readable.")
                    .arg(qs(s.trialStarted))
                    .arg(kTrialDays);
    if (!s.problem.empty()) text += QStringLiteral("<br><span style='color:#d9534f'>%1</span>").arg(tr("The saved key was not accepted: %1").arg(qs(s.problem).toHtmlEscaped()));
    if (usingDevelopmentLicenceKey())
        text += tr("<br><i>Development build: keys are checked against the public development key, so anyone can make "
                   "one. Release builds use the publisher's key.</i>");
    m_status->setText(text);
    m_remove->setEnabled(s.mode == LicenceMode::Licensed || !s.problem.empty());
}

void LicenceDialog::install() {
    auto k = installLicenceKey(s_dataDir, m_key->toPlainText().toStdString());
    if (!k) {
        QMessageBox::warning(this, windowTitle(), qs(k.error().message));
        return;
    }
    refreshLicence();
    Logger::instance().log(LogLevel::Info, "licence", "Licence key installed: " + describeLicence(currentLicence()));
    m_key->clear();
    updateStatus();
    Q_EMIT licenceChanged();
    QMessageBox::information(this, windowTitle(), tr("Thank you! AceStudio is licensed to %1.").arg(qs(k.value().name)));
}

} // namespace acestudio::ui
