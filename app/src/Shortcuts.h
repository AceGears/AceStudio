#pragma once

// Customisable keyboard shortcuts.
//
// Every menu action gets a stable id (objectName) derived from its menu and
// label, e.g. "edit.undo". Defaults come from the code; user overrides are
// stored in the "keyboard.shortcuts" setting as {id: "Ctrl+Shift+N"} (an
// empty string removes the shortcut).

#include <QDialog>
#include <QList>
#include <QPointer>
#include <QString>

#include <nlohmann/json.hpp>

class QAction;
class QLineEdit;
class QMenuBar;
class QTableWidget;

namespace acestudio::ui {

class ShortcutRegistry {
public:
    // Walks the menu bar (skipping `skipMenu`, e.g. the recent-files list)
    // and records each action's id and default shortcut.
    void collect(QMenuBar* bar, const QObject* skipMenu = nullptr);
    void apply(const nlohmann::json& overrides);
    // The recorded actions that still exist: menus that rebuild themselves
    // when opened (Scripts, Workspaces) delete theirs.
    QList<QAction*> actions() const;
    // Human-readable list of clashing shortcuts (empty = none).
    QStringList conflicts() const;

    static QString defaultShortcut(const QAction* a);
    static QString displayName(const QAction* a);   // "Edit › Undo"

private:
    QList<QPointer<QAction>> m_actions;
};

class ShortcutsDialog : public QDialog {
    Q_OBJECT
public:
    ShortcutsDialog(const ShortcutRegistry& reg, const nlohmann::json& overrides, QWidget* parent = nullptr);
    // Overrides relative to defaults, ready to store in settings.
    nlohmann::json overrides() const { return m_result; }

private:
    void accept() override;
    void filter(const QString& text);

    const ShortcutRegistry& m_reg;
    QTableWidget* m_table = nullptr;
    QLineEdit* m_search = nullptr;
    nlohmann::json m_result = nlohmann::json::object();
};

} // namespace acestudio::ui
