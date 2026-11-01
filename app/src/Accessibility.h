#pragma once

// Accessibility helpers (Phase 8). installAccessibilityHelper() watches every
// window as it is shown and gives each control without an accessible name the
// one a sighted user reads next to it: its label (buddy, form row, grid cell
// or the label just before it), else its tooltip or placeholder. Screen
// readers then announce "Tile size, spin box, 16" instead of "spin box, 16".
// The UI audit (selftest/UiAudit) checks that nothing is left unnamed.

class QApplication;
class QWidget;

namespace acestudio::ui {

void installAccessibilityHelper(QApplication& app);
// Names the controls of `root` now (also used by the audit).
void nameControls(QWidget* root);

} // namespace acestudio::ui
