#include "Theme.h"

#include <QApplication>
#include <QFont>
#include <QPalette>
#include <QProxyStyle>
#include <QStyleFactory>

#include <algorithm>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace acestudio::ui {

namespace {
ThemeTokens g_tokens;

ThemeTokens tokensFor(ThemeKind k, const QColor& accentIn) {
    ThemeTokens t;
    if (k == ThemeKind::HighContrast) {
        t = {QColor("#000000"), QColor("#000000"), QColor("#101010"), QColor("#000000"), QColor("#1A1A1A"),
             QColor("#FFFFFF"), QColor("#FFFFFF"), QColor("#FFFFFF"), QColor("#8FD3FF"), QColor("#1A1A1A"),
             QColor("#FFFF00"), QColor("#FFFF00"), QColor("#FFFF00"), QColor("#000000"), QColor("#000000"),
             QColor("#000000"), QColor("#00FF66"), QColor("#FFFF00"), QColor("#FF6060")};
        t.dark = true;
        t.highContrast = true;
        return t;
    }
    const QColor accent = accentIn.isValid() ? accentIn : QColor(kBrandAccent);
    if (k == ThemeKind::Light) {
        // Balanced light: cool greys, never pure white backgrounds behind the canvas.
        t = {QColor("#ECEFF4"), QColor("#FFFFFF"), QColor("#F5F7FA"), QColor("#F7F8FB"), QColor("#FFFFFF"),
             QColor("#D3D8E2"), QColor("#161A22"), QColor("#5B6475"), QColor("#A7AEBC"), QColor("#E3E8F1"),
             accent, accent.lighter(125), accent.darker(125), QColor("#FFFFFF"), QColor("#FFFFFF"),
             QColor("#E9EDF3"), QColor("#1F9D55"), QColor("#B7791F"), QColor("#D64545")};
        t.dark = false;
        return t;
    }
    // Deep charcoal and graphite.
    t = {QColor("#0E1014"), QColor("#0B0D10"), QColor("#14171D"), QColor("#161A20"), QColor("#1D222A"),
         QColor("#262C36"), QColor("#E8ECF3"), QColor("#8C95A7"), QColor("#4E5666"), QColor("#202631"),
         accent, accent.lighter(140), accent.darker(140), QColor("#FFFFFF"), QColor("#232934"),
         QColor("#171B22"), QColor("#3DD68C"), QColor("#F5B94A"), QColor("#FF6B6B")};
    t.dark = true;
    return t;
}
// Fusion, except that clicking a slider's track jumps straight to the click
// point (the platform default moves by a page step, which feels broken for
// opacity and other value sliders).
class AceStyle final : public QProxyStyle {
public:
    using QProxyStyle::QProxyStyle;
    int styleHint(StyleHint hint, const QStyleOption* opt, const QWidget* w, QStyleHintReturn* ret) const override {
        if (hint == QStyle::SH_Slider_AbsoluteSetButtons) return Qt::LeftButton;
        if (hint == QStyle::SH_Slider_PageSetButtons) return Qt::MiddleButton;
        return QProxyStyle::styleHint(hint, opt, w, ret);
    }
};
} // namespace

ThemeKind themeFromString(const QString& s) {
    if (s == QLatin1String("light")) return ThemeKind::Light;
    if (s == QLatin1String("highContrast")) return ThemeKind::HighContrast;
    return ThemeKind::Dark;
}
QString toString(ThemeKind t) {
    return t == ThemeKind::Light ? QStringLiteral("light") : t == ThemeKind::HighContrast ? QStringLiteral("highContrast") : QStringLiteral("dark");
}

bool systemHighContrast() {
#ifdef _WIN32
    HIGHCONTRASTW hc{};
    hc.cbSize = sizeof hc;
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof hc, &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
#else
    return false;
#endif
}

void applyTextScale(QApplication& app, int percent) {
    static const QFont base = app.font();   // the system font, before any scaling
    QFont f = base;
    const int p = std::clamp(percent, 75, 250);
    if (base.pointSizeF() > 0) f.setPointSizeF(base.pointSizeF() * p / 100.0);
    else if (base.pixelSize() > 0) f.setPixelSize(std::max(1, base.pixelSize() * p / 100));
    app.setFont(f);
}

const ThemeTokens& themeTokens() { return g_tokens; }

void applyTheme(QApplication& app, ThemeKind kind, const QColor& accentIn) {
    const ThemeTokens t = tokensFor(kind, accentIn);
    g_tokens = t;
    const bool hc = t.highContrast;
    app.setStyle(new AceStyle(QStyleFactory::create(QStringLiteral("Fusion"))));   // QApplication takes ownership

    QPalette p;
    p.setColor(QPalette::Window, t.window);
    p.setColor(QPalette::WindowText, t.text);
    p.setColor(QPalette::Base, t.base);
    p.setColor(QPalette::AlternateBase, t.alternate);
    p.setColor(QPalette::ToolTipBase, t.raised);
    p.setColor(QPalette::ToolTipText, t.text);
    p.setColor(QPalette::PlaceholderText, t.textMuted);
    p.setColor(QPalette::Text, t.text);
    p.setColor(QPalette::Button, t.panel);
    p.setColor(QPalette::ButtonText, t.text);
    p.setColor(QPalette::BrightText, t.danger);
    p.setColor(QPalette::Highlight, t.accent);
    p.setColor(QPalette::HighlightedText, t.onAccent);
    p.setColor(QPalette::Link, hc ? t.accent : t.accentHi);
    p.setColor(QPalette::Mid, t.border);
    p.setColor(QPalette::Dark, t.base.darker(130));
    p.setColor(QPalette::Light, t.raised);
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        p.setColor(QPalette::Disabled, role, t.disabled);
    app.setPalette(p);

    // Restrained, layered surfaces: charcoal window, graphite panels with a
    // faint metallic gradient, 1px hairlines, 6-10px radii, and the electric
    // accent kept for state (selection, focus, primary actions, progress).
    // Sizes are in px; Qt scales them for high DPI.
    QString qss = QStringLiteral(R"(
        QMainWindow, QDialog { background: {window}; }
        QMainWindow::separator { background: {border}; width: 1px; height: 1px; }
        QMainWindow::separator:hover { background: {accent}; }
        QDockWidget { titlebar-close-icon: none; }
        QDockWidget::title { background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 {metalHi}, stop:1 {metalLo});
                             padding: 6px 10px; border-bottom: 1px solid {border};
                             font-weight: 600; color: {muted}; text-transform: uppercase; }
        QMenuBar { background: {window}; border-bottom: 1px solid {border}; padding: 2px 4px; }
        QMenuBar::item { padding: 5px 10px; border-radius: 6px; background: transparent; color: {muted}; }
        QMenuBar::item:selected { background: {hover}; color: {text}; }
        QMenu { background: {raised}; border: 1px solid {border}; padding: 6px; border-radius: 10px; }
        QMenu::item { padding: 6px 28px 6px 14px; border-radius: 6px; }
        QMenu::item:selected { background: qlineargradient(x1:0,y1:0,x2:1,y2:0, stop:0 {accent}, stop:1 {accentHi}); color: {onAccent}; }
        QMenu::item:disabled { color: {disabled}; }
        QMenu::separator { height: 1px; background: {border}; margin: 5px 8px; }
        QTabWidget::pane { border: none; }
        QTabBar::tab { background: transparent; color: {muted}; padding: 7px 16px; border: none;
                       border-bottom: 2px solid transparent; margin-right: 2px; }
        QTabBar::tab:selected { color: {text}; border-bottom: 2px solid {accent}; }
        QTabBar::tab:hover:!selected { color: {text}; background: {hover}; border-top-left-radius: 6px; border-top-right-radius: 6px; }
        QStatusBar { background: {window}; border-top: 1px solid {border}; color: {muted}; }
        QStatusBar QPushButton { border: none; background: transparent; color: {muted}; padding: 2px 8px; }
        QToolTip { background: {raised}; color: {text}; border: 1px solid {border}; padding: 5px 8px; border-radius: 6px; }
        QListWidget, QListView, QTreeWidget, QTreeView, QTableWidget, QTableView, QPlainTextEdit, QTextEdit, QTextBrowser {
            background: {base}; border: 1px solid {border}; border-radius: 8px; }
        QListWidget::item, QTreeWidget::item { padding: 3px; border-radius: 5px; }
        QListWidget::item:hover, QTreeWidget::item:hover { background: {hover}; }
        QListWidget::item:selected, QTreeWidget::item:selected, QTableWidget::item:selected { background: {accent}; color: {onAccent}; }
        QHeaderView::section { background: {panel}; color: {muted}; border: none; border-bottom: 1px solid {border}; padding: 4px 6px; }
        QPushButton { padding: 6px 16px; border: 1px solid {border}; border-radius: 7px;
                      background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 {metalHi}, stop:1 {metalLo}); color: {text}; }
        QPushButton:hover { border-color: {accentLo}; background: {hover}; }
        QPushButton:pressed { background: {base}; }
        QPushButton:disabled { color: {disabled}; }
        QPushButton:default { border-color: {accent}; }
        QPushButton[accent="true"] { border: 1px solid {accentLo}; color: {onAccent}; font-weight: 600;
                      background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 {accentHi}, stop:1 {accent}); }
        QPushButton[accent="true"]:hover { background: {accentHi}; }
        QPushButton[accent="true"]:disabled { background: {hover}; color: {disabled}; border-color: {border}; }
        QToolButton { border: 1px solid transparent; border-radius: 6px; padding: 4px; }
        QToolButton:hover { background: {hover}; border-color: {border}; }
        QToolButton:checked { background: {hover}; border-color: {accent}; }
        QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox { padding: 4px 8px; border: 1px solid {border}; border-radius: 6px;
                      background: {base}; selection-background-color: {accent}; }
        QLineEdit:hover, QSpinBox:hover, QDoubleSpinBox:hover, QComboBox:hover { border-color: {accentLo}; }
        QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus { border-color: {accent}; }
        QComboBox QAbstractItemView { background: {raised}; border: 1px solid {border}; border-radius: 8px; padding: 4px;
                      selection-background-color: {accent}; selection-color: {onAccent}; outline: none; }
        QPushButton:focus, QToolButton:focus, QListWidget:focus, QTreeWidget:focus, QTableWidget:focus,
        QPlainTextEdit:focus, QTextBrowser:focus { border: {focusW}px solid {accent}; }
        QCheckBox:focus, QRadioButton:focus { border: 1px dashed {accent}; }
        QGroupBox { border: 1px solid {border}; border-radius: 10px; margin-top: 14px; padding: 10px 8px 8px 8px; }
        QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 4px; color: {muted}; font-weight: 600; }
        QProgressBar { border: 1px solid {border}; border-radius: 6px; background: {base}; text-align: center; color: {text}; min-height: 12px; }
        QProgressBar::chunk { border-radius: 5px;
                      background: qlineargradient(x1:0,y1:0,x2:1,y2:0, stop:0 {accentLo}, stop:0.6 {accent}, stop:1 {accentHi}); }
        QSlider::groove:horizontal { height: 4px; background: {border}; border-radius: 2px; }
        QSlider::sub-page:horizontal { background: {accent}; border-radius: 2px; }
        QSlider::handle:horizontal { width: 14px; height: 14px; margin: -6px 0; border-radius: 7px;
                      background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #FFFFFF, stop:1 #C9D3E3); border: 1px solid {accentLo}; }
        QScrollBar:vertical { width: 10px; background: transparent; margin: 2px; }
        QScrollBar:horizontal { height: 10px; background: transparent; margin: 2px; }
        QScrollBar::handle { background: {border}; border-radius: 4px; min-height: 28px; min-width: 28px; }
        QScrollBar::handle:hover { background: {muted}; }
        QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
        QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
        QFrame[card="true"] { background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 {metalHi}, stop:1 {panel});
                      border: 1px solid {border}; border-radius: 12px; }
        QFrame[card="true"]:hover { border-color: {accentLo}; }
        QLabel[role="h1"] { font-size: 20pt; font-weight: 700; color: {text}; }
        QLabel[role="h2"] { font-size: 13pt; font-weight: 600; color: {text}; }
        QLabel[role="muted"] { color: {muted}; }
        QLabel[role="eyebrow"] { color: {accentHi}; font-weight: 700; letter-spacing: 1px; }
        QScrollArea { background: transparent; border: none; }
        QScrollArea > QWidget > QWidget { background: transparent; }
    )");
    const std::pair<const char*, QString> vars[] = {
        {"{window}", t.window.name()},   {"{base}", t.base.name()},       {"{panel}", t.panel.name()},
        {"{raised}", t.raised.name()},   {"{border}", t.border.name()},   {"{text}", t.text.name()},
        {"{muted}", t.textMuted.name()}, {"{disabled}", t.disabled.name()}, {"{hover}", t.hover.name()},
        {"{accentHi}", t.accentHi.name()}, {"{accentLo}", t.accentLo.name()}, {"{accent}", t.accent.name()},
        {"{onAccent}", t.onAccent.name()}, {"{metalHi}", t.metalHi.name()}, {"{metalLo}", t.metalLo.name()},
        {"{focusW}", hc ? QStringLiteral("2") : QStringLiteral("1")}};
    for (const auto& [k, v] : vars) qss.replace(QLatin1String(k), v);
    app.setStyleSheet(qss);
}

} // namespace acestudio::ui
