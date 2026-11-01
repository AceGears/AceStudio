#pragma once

#include <QColor>
#include <QString>

class QApplication;

namespace acestudio::ui {

// HighContrast: white on black, yellow selection and focus (also chosen
// automatically when Windows runs a high-contrast theme).
enum class ThemeKind { Dark, Light, HighContrast };

ThemeKind themeFromString(const QString& s);
QString toString(ThemeKind t);

// Applies palette + stylesheet application-wide. `accent` tints selection,
// focus rings and primary buttons.
void applyTheme(QApplication& app, ThemeKind kind, const QColor& accent);
// Windows "Contrast themes" (Settings › Accessibility) is on.
bool systemHighContrast();
// Interface text size in percent of the system font (100 = unchanged).
void applyTextScale(QApplication& app, int percent);

} // namespace acestudio::ui
