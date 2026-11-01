#include "UiAudit.h"

#include "../Accessibility.h"
#include "../DocumentView.h"
#include "../MainWindow.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QAccessible>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFile>
#include <QHeaderView>
#include <QScrollBar>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QPointer>
#include <QSet>
#include <QTabBar>
#include <QTextEdit>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>

#include <cstdio>
#include <functional>

namespace acestudio::ui {

namespace {

struct Audit {
    QJsonArray unnamed, unreachable, windows, actions, skipped;
    QSet<QString> seen;   // window|class|name, so repeated windows are counted once
    int controls = 0;
};

bool interactive(QWidget* w) {
    // Scroll bars and table headers are announced with their view, and tab
    // close buttons with their tab.
    if (qobject_cast<QScrollBar*>(w) || qobject_cast<QHeaderView*>(w) || qstrcmp(w->metaObject()->className(), "CloseButton") == 0 ||
        w->objectName().startsWith(QLatin1String("qt_")))   // Qt's own (the menu bar's overflow button)
        return false;
    return qobject_cast<QAbstractButton*>(w) || qobject_cast<QComboBox*>(w) || qobject_cast<QAbstractSpinBox*>(w) ||
           qobject_cast<QAbstractSlider*>(w) || qobject_cast<QLineEdit*>(w) || qobject_cast<QPlainTextEdit*>(w) ||
           qobject_cast<QTextEdit*>(w) || qobject_cast<QAbstractItemView*>(w) || qobject_cast<QTabBar*>(w);
}

QString windowName(QWidget* top) {
    const QString t = top->windowTitle();
    return t.isEmpty() ? QString::fromLatin1(top->metaObject()->className()) : t;
}

// `pageName` audits a page of the main window (a workspace) as if it were a
// window of its own, keyboard reachability included.
void auditWindow(QWidget* top, Audit& a, const QString& context, const QString& pageName = {}) {
    nameControls(top);   // what the app does when the window is shown
    const QString win = pageName.isEmpty() ? windowName(top) : pageName;
    a.windows.append(QJsonObject{{"window", win}, {"class", top->metaObject()->className()}, {"from", context}});
    QList<QWidget*> all = top->findChildren<QWidget*>();
    all.prepend(top);
    for (QWidget* w : all) {
        if (!w->isVisibleTo(top) || !interactive(w)) continue;
        // Spin boxes and combo boxes own an inner line edit: the outer control is what is announced.
        if (qobject_cast<QLineEdit*>(w) && (qobject_cast<QAbstractSpinBox*>(w->parentWidget()) || qobject_cast<QComboBox*>(w->parentWidget())))
            continue;
        // Inside a QTabWidget the tab bar is named by its tabs.
        ++a.controls;
        QAccessibleInterface* ai = QAccessible::queryAccessibleInterface(w);
        const QString name = ai ? ai->text(QAccessible::Name).trimmed() : QString();
        const QString cls = QString::fromLatin1(w->metaObject()->className());
        const QString key = win + "|" + cls + "|" + w->objectName() + "|" + name + "|" + QString::number(w->x()) + "," + QString::number(w->y());
        if (a.seen.contains(key)) continue;
        a.seen.insert(key);
        QJsonObject info{{"window", win}, {"class", cls}, {"objectName", w->objectName()}, {"toolTip", w->toolTip()}};
        if (auto* b = qobject_cast<QAbstractButton*>(w)) info["text"] = b->text();
        bool tabNamed = false;
        if (auto* tb = qobject_cast<QTabBar*>(w)) tabNamed = tb->count() > 0;
        if (name.isEmpty() && !tabNamed) a.unnamed.append(info);
        // Keyboard: every control in a dialog must take focus with Tab
        // (toolbar buttons are reached through menus and shortcuts instead).
        const bool inToolbar = qobject_cast<QToolBar*>(w->parentWidget()) != nullptr;
        if (!inToolbar && (qobject_cast<QDialog*>(top) || !pageName.isEmpty()) && !(w->focusPolicy() & Qt::TabFocus) && w->isEnabled()) {
            info["name"] = name;
            a.unreachable.append(info);
        }
    }
}

// Closes the topmost modal window after auditing it; returns whether one was found.
bool closeModal(Audit& a, const QString& context) {
    QWidget* m = QApplication::activeModalWidget();
    if (!m) return false;
    auditWindow(m, a, context);
    if (auto* d = qobject_cast<QDialog*>(m)) d->reject();
    else m->close();
    return true;
}

void collectActions(QMenu* menu, QList<QAction*>& out, QSet<QMenu*>& visited) {
    if (!menu || visited.contains(menu)) return;
    visited.insert(menu);
    QMetaObject::invokeMethod(menu, "aboutToShow", Qt::DirectConnection);   // menus that fill themselves when opened
    for (QAction* act : menu->actions()) {
        if (act->isSeparator()) continue;
        if (act->menu()) collectActions(act->menu(), out, visited);
        else out.append(act);
    }
}

bool skip(const QString& text) {
    // Commands that end the session or open other programs, and ones that
    // erase things straight away (commands ending in "…" only open a dialog).
    for (const char* w : {"Quit", "Exit", "Folder", "Logs", "Restart"})
        if (text.contains(QLatin1String(w), Qt::CaseInsensitive)) return true;
    if (text.endsWith(QChar(0x2026))) return false;
    for (const char* w : {"Close", "Delete", "Remove", "Clear", "Reset", "Discard", "Revert", "Apply"})
        if (text.contains(QLatin1String(w), Qt::CaseInsensitive)) return true;
    return false;
}

} // namespace

int runUiAudit(MainWindow& window, const QString& reportPath, const QString& project) {
    Audit a;
    if (!project.isEmpty()) window.openProject(project);
    QApplication::processEvents();
    auditWindow(&window, a, QStringLiteral("main window"));
    for (QDockWidget* d : window.findChildren<QDockWidget*>()) auditWindow(d, a, QStringLiteral("dock"));

    QList<QAction*> actions;
    QSet<QMenu*> visited;
    for (QAction* top : window.menuBar()->actions()) collectActions(top->menu(), actions, visited);

    // While a command runs (possibly inside a modal exec()), audit and close
    // whatever modal window it opens.
    QString current;
    QTimer watchdog;
    watchdog.setInterval(40);
    QObject::connect(&watchdog, &QTimer::timeout, [&] { closeModal(a, current); });
    watchdog.start();

    for (QPointer<QAction> act : actions) {
        if (!act) continue;
        const QString text = act->text().remove(QLatin1Char('&'));
        if (!act->isEnabled() || skip(text)) {
            a.skipped.append(QJsonObject{{"action", text}, {"enabled", act->isEnabled()}});
            continue;
        }
        current = text;
        std::fprintf(stderr, "ui-audit: %s\n", qPrintable(text));   // the log shows where a crash happened
        const QWidgetList tops = QApplication::topLevelWidgets();
        const QSet<QWidget*> before(tops.begin(), tops.end());
        QElapsedTimer t;
        t.start();
        act->trigger();
        QApplication::processEvents();
        while (closeModal(a, current)) QApplication::processEvents();
        // Non-modal windows it opened.
        for (QWidget* w : QApplication::topLevelWidgets())
            if (!before.contains(w) && w->isVisible() && w != &window) {
                auditWindow(w, a, current);
                w->close();
            }
        QApplication::processEvents();
        a.actions.append(QJsonObject{{"action", text}, {"ms", static_cast<double>(t.elapsed())}});
    }
    // Every workspace page, as shown from the rail.
    for (const WorkspaceInfo& w : workspaceInfos()) {
        current = QStringLiteral("workspace ") + QString::fromLatin1(w.key);
        window.showWorkspace(w.id);
        QApplication::processEvents();
        while (closeModal(a, current)) QApplication::processEvents();
        if (QWidget* page = window.m_pageFor[static_cast<std::size_t>(w.id)])
            auditWindow(page, a, current, QStringLiteral("Workspace: ") + QString::fromLatin1(w.key));
    }
    watchdog.stop();

    // Background autosave: the commands above changed the documents. An
    // autosave writes a recovery file for each without blocking, and
    // discarding (as Save, Close and Quit do) removes them again.
    QJsonObject autosave;
    {
        const QDir dir(QString::fromStdWString(window.m_recovery->directory().wstring()));
        const auto files = [&] { return dir.entryList({QStringLiteral("*.acestudio")}, QDir::Files).size(); };
        int modified = 0;
        for (int i = 0; i < window.m_tabs->count(); ++i)
            if (static_cast<DocumentView*>(window.m_tabs->widget(i))->needsAutosave()) ++modified;
        QElapsedTimer t;
        t.start();
        window.autosaveTick();
        const qint64 tickMs = t.elapsed();
        window.m_autosaveQueue->drain();
        QApplication::processEvents();
        int stillPending = 0;
        for (int i = 0; i < window.m_tabs->count(); ++i)
            if (static_cast<DocumentView*>(window.m_tabs->widget(i))->needsAutosave()) ++stillPending;
        const auto written = files();
        for (int i = 0; i < window.m_tabs->count(); ++i) window.discardRecovery(static_cast<DocumentView*>(window.m_tabs->widget(i)));
        window.m_autosaveQueue->drain();
        autosave = QJsonObject{{"modifiedDocuments", modified}, {"tickMs", static_cast<double>(tickMs)},
                               {"recoveryFilesWritten", static_cast<int>(written)}, {"stillNeedingAutosave", stillPending},
                               {"recoveryFilesAfterDiscard", static_cast<int>(files())}};
    }

    QJsonObject report{{"actions", a.actions},       {"skipped", a.skipped},         {"windows", a.windows},
                       {"controls", a.controls},     {"unnamed", a.unnamed},         {"unreachable", a.unreachable},
                       {"autosave", autosave}};
    QFile f(reportPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 2;
    f.write(QJsonDocument(report).toJson());
    return 0;
}

} // namespace acestudio::ui
