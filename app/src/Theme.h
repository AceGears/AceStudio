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

// The design tokens of the current theme. Widgets that paint themselves
// (the workspace rail, cards, progress rings, icons) use these instead of
// hard-coded colours, so every theme and accent applies everywhere.
struct ThemeTokens {
    QColor window;      // app background (deep charcoal in dark)
    QColor base;        // inputs, lists, canvas surround
    QColor alternate;   // alternate rows
    QColor panel;       // docks, cards (graphite)
    QColor raised;      // hovered / raised surfaces
    QColor border;      // 1px hairlines
    QColor text;
    QColor textMuted;
    QColor disabled;
    QColor hover;
    QColor accent;      // electric blue (from the logo), or the user's accent
    QColor accentHi;    // the bright end of accent gradients
    QColor accentLo;    // the deep end of accent gradients
    QColor onAccent;    // text on accent
    QColor metalHi;     // top of the subtle metallic gradients
    QColor metalLo;     // bottom of the subtle metallic gradients
    QColor success, warning, danger;
    bool dark = true;
    bool highContrast = false;
};

// Electric blue sampled from the AceStudio logo (its blues run #0050F0–#00A0F0);
// dark enough for white text.
inline constexpr const char* kBrandAccent = "#0A6CF5";

// Applies palette + stylesheet application-wide. `accent` tints selection,
// focus rings and primary buttons (invalid = the brand accent).
void applyTheme(QApplication& app, ThemeKind kind, const QColor& accent);
const ThemeTokens& themeTokens();
// Windows "Contrast themes" (Settings › Accessibility) is on.
bool systemHighContrast();
// Interface text size in percent of the system font (100 = unchanged).
void applyTextScale(QApplication& app, int percent);

} // namespace acestudio::ui
