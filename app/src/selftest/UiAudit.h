#pragma once

// UI audit (Phase 8: accessibility and the UI regression check). Started with
//   AceStudio --ui-audit report.json [project.acestudio]
// it opens the project, triggers every menu command, and for every window
// that appears (dialogs, message boxes, file dialogs) records what a screen
// reader announces for each control and whether the keyboard can reach it,
// then closes the window again. tests/ui_audit.cmake runs it offscreen.

#include <QString>

namespace acestudio::ui {

class MainWindow;

// Writes the JSON report; returns the process exit code (0 unless the audit
// itself failed).
int runUiAudit(MainWindow& window, const QString& reportPath, const QString& project);

} // namespace acestudio::ui
