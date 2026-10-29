#pragma once

#include <QColor>
#include <QString>

class QApplication;

namespace acestudio::ui {

enum class ThemeKind { Dark, Light };

ThemeKind themeFromString(const QString& s);
QString toString(ThemeKind t);

// Applies palette + stylesheet application-wide. `accent` tints selection,
// focus rings and primary buttons.
void applyTheme(QApplication& app, ThemeKind kind, const QColor& accent);

} // namespace acestudio::ui
