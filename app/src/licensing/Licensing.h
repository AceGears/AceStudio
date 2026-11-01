#pragma once

// The app's side of licensing (core/Licence.h): the Help › Licence dialog,
// the status-bar indicator, and the check before anything is saved or
// exported. After the 14-day trial, documents still open and can be edited;
// saving and exporting ask for a key. Autosave and recovery keep working.

#include "acestudio/core/Licence.h"

#include <QDialog>

#include <filesystem>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QWidget;

namespace acestudio::ui {

// Set once at startup with the app's data folder.
void initLicensing(const std::filesystem::path& dataDir);
const LicenceStatus& currentLicence();
void refreshLicence();

// True when `action` ("Saving", "Exporting") may write files. Otherwise shows
// why, with a button to enter a key, and returns whether a key was entered.
bool licenceAllows(QWidget* parent, const QString& action);
// For scripts: empty, or the reason writes are refused.
std::string licenceReadOnlyReason();

class LicenceDialog : public QDialog {
    Q_OBJECT
public:
    explicit LicenceDialog(QWidget* parent = nullptr);

Q_SIGNALS:
    void licenceChanged();

private:
    void updateStatus();
    void install();

    QLabel* m_status = nullptr;
    QPlainTextEdit* m_key = nullptr;
    QPushButton* m_install = nullptr;
    QPushButton* m_remove = nullptr;
};

} // namespace acestudio::ui
