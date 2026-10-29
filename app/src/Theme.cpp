#include "Theme.h"

#include <QApplication>
#include <QPalette>
#include <QStyleFactory>

namespace acestudio::ui {

namespace {
struct Tokens {
    QColor window, base, alternate, panel, border, text, textMuted, disabled, hover;
};

Tokens tokensFor(ThemeKind k) {
    if (k == ThemeKind::Light)
        return {QColor("#EEEEF2"), QColor("#FFFFFF"), QColor("#F5F5F8"), QColor("#E4E4EA"),
                QColor("#CACAD3"), QColor("#1D1D24"), QColor("#5C5C6B"), QColor("#A3A3B0"), QColor("#DCDCE4")};
    return {QColor("#1E1E23"), QColor("#18181C"), QColor("#222228"), QColor("#26262D"),
            QColor("#34343D"), QColor("#E6E6EC"), QColor("#9A9AA8"), QColor("#5C5C68"), QColor("#30303A")};
}
} // namespace

ThemeKind themeFromString(const QString& s) { return s == QLatin1String("light") ? ThemeKind::Light : ThemeKind::Dark; }
QString toString(ThemeKind t) { return t == ThemeKind::Light ? QStringLiteral("light") : QStringLiteral("dark"); }

void applyTheme(QApplication& app, ThemeKind kind, const QColor& accentIn) {
    const QColor accent = accentIn.isValid() ? accentIn : QColor("#4F8CFF");
    const Tokens t = tokensFor(kind);
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    QPalette p;
    p.setColor(QPalette::Window, t.window);
    p.setColor(QPalette::WindowText, t.text);
    p.setColor(QPalette::Base, t.base);
    p.setColor(QPalette::AlternateBase, t.alternate);
    p.setColor(QPalette::ToolTipBase, t.panel);
    p.setColor(QPalette::ToolTipText, t.text);
    p.setColor(QPalette::PlaceholderText, t.textMuted);
    p.setColor(QPalette::Text, t.text);
    p.setColor(QPalette::Button, t.panel);
    p.setColor(QPalette::ButtonText, t.text);
    p.setColor(QPalette::BrightText, QColor("#FF5A5A"));
    p.setColor(QPalette::Highlight, accent);
    p.setColor(QPalette::HighlightedText, QColor("#FFFFFF"));
    p.setColor(QPalette::Link, accent);
    p.setColor(QPalette::Mid, t.border);
    p.setColor(QPalette::Dark, t.base.darker(130));
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        p.setColor(QPalette::Disabled, role, t.disabled);
    app.setPalette(p);

    // Restrained styling: flat surfaces, 1px borders, small radii, accent
    // used only for state. Sizes are in px; Qt scales them for high DPI.
    const QString qss = QStringLiteral(R"(
        QMainWindow::separator { background: %1; width: 1px; height: 1px; }
        QDockWidget { titlebar-close-icon: none; }
        QDockWidget::title { background: %2; padding: 6px 8px; border-bottom: 1px solid %1;
                             font-weight: 600; color: %4; }
        QMenuBar { background: %3; border-bottom: 1px solid %1; padding: 2px; }
        QMenuBar::item { padding: 4px 10px; border-radius: 4px; background: transparent; }
        QMenuBar::item:selected { background: %6; }
        QMenu { background: %2; border: 1px solid %1; padding: 4px; border-radius: 6px; }
        QMenu::item { padding: 5px 24px 5px 12px; border-radius: 4px; }
        QMenu::item:selected { background: %5; color: white; }
        QMenu::item:disabled { color: %7; }
        QMenu::separator { height: 1px; background: %1; margin: 4px 6px; }
        QTabWidget::pane { border: none; }
        QTabBar::tab { background: %3; color: %4; padding: 6px 14px; border: 1px solid %1;
                       border-bottom: none; margin-right: 1px;
                       border-top-left-radius: 5px; border-top-right-radius: 5px; }
        QTabBar::tab:selected { background: %2; color: %8; border-top: 2px solid %5; }
        QTabBar::tab:hover:!selected { background: %6; }
        QStatusBar { background: %3; border-top: 1px solid %1; color: %4; }
        QListWidget, QTreeWidget, QTableWidget, QPlainTextEdit { border: 1px solid %1; border-radius: 4px; }
        QListWidget::item:selected, QTreeWidget::item:selected, QTableWidget::item:selected { background: %5; color: white; }
        QPushButton { padding: 5px 14px; border: 1px solid %1; border-radius: 5px; background: %2; }
        QPushButton:hover { background: %6; }
        QPushButton:default { border-color: %5; }
        QLineEdit, QSpinBox, QComboBox { padding: 3px 6px; border: 1px solid %1; border-radius: 4px; background: %9; }
        QLineEdit:focus, QSpinBox:focus, QComboBox:focus { border-color: %5; }
        QToolTip { border: 1px solid %1; padding: 4px; }
        QScrollBar:vertical { width: 10px; background: transparent; }
        QScrollBar:horizontal { height: 10px; background: transparent; }
        QScrollBar::handle { background: %1; border-radius: 4px; min-height: 24px; min-width: 24px; }
        QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
    )")
        .arg(t.border.name(), t.panel.name(), t.window.name(), t.textMuted.name(), accent.name(),
             t.hover.name(), t.disabled.name(), t.text.name(), t.base.name());
    app.setStyleSheet(qss);
}

} // namespace acestudio::ui
