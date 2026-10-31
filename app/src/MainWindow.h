#pragma once

#include "acestudio/core/Animation.h"

#include "acestudio/core/Result.h"

#include "acestudio/core/Editor.h"
#include "acestudio/core/Recovery.h"
#include "Shortcuts.h"

#include <QMainWindow>

#include <memory>

class QAction;
class QActionGroup;
class QDockWidget;
class QLabel;
class QListWidget;
class QMenu;
class QStackedWidget;
class QTabWidget;
class QTimer;

namespace acestudio::ui {

class AppContext;
class DocumentView;
class LayersPanel;
class LogPanel;
class PalettePanel;
class PreviewPanel;
class AiPanel;
class AiService;
class PropertiesPanel;
class TimelinePanel;
class ToolState;
class ToolsPanel;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(AppContext& ctx, QWidget* parent = nullptr);

    // Opens a project; returns false (after informing the user) on failure.
    bool openProject(const QString& path);
    // Offers to restore documents left behind by a crash.
    void offerRecovery();

protected:
    void closeEvent(QCloseEvent* e) override;
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dropEvent(QDropEvent* e) override;

private:
    void buildMenus();
    void buildDocks();
    QWidget* buildWelcome();
    void restoreWindowState();
    void saveWindowState();

    void newProject();
    void openProjectDialog();
    bool save(DocumentView* view);
    bool saveAs(DocumentView* view);
    bool closeTab(int index);
    bool maybeSave(DocumentView* view);   // false = user cancelled

    void addView(DocumentView* view);
    DocumentView* currentView() const;
    void onCurrentTabChanged();
    void updateTabTitle(DocumentView* view);
    void updateActions();
    void rebuildRecentMenus();
    void setThemeAndPersist(const QString& theme);
    void resetLayout();
    void showAbout();
    void showNotices();
    void importPng(bool asLayer);
public:
    // Used by AI Studio for results.
    DocumentView* openAnimationAsDocument(const std::vector<Image>& frames, const std::vector<int>& durations, const QString& name);
    DocumentView* openImageAsDocument(const Image& img, const QString& name);
    bool insertImageAsLayer(DocumentView* view, const Image& img, const QString& name, std::optional<LayerId> above = {},
                            int frame = -1);
private:
    void exportPng();
    void exportAnimation(int kind);   // ExportKind
    void editTags();
    void editOnionSkin();
    OnionSkin onionSkinFromSettings() const;
    void applyOnionSkin();
    bool report(const Status& st);   // true when it succeeded
    void report(const Error& e);
    void autosaveTick();
    void discardRecovery(DocumentView* view);
    void applyAutosaveInterval();
    void transformCurrentLayer(const QString& label, bool allFrames, const Editor::ImageFn& fn);
    void editShortcuts();

    AppContext& m_ctx;
    QStackedWidget* m_stack = nullptr;
    QTabWidget* m_tabs = nullptr;
    QListWidget* m_welcomeRecent = nullptr;
    QMenu* m_recentMenu = nullptr;

    ToolState* m_tools = nullptr;
    ToolsPanel* m_toolsPanel = nullptr;
    QDockWidget* m_toolsDock = nullptr;
    QAction* m_undoAction = nullptr;
    QAction* m_redoAction = nullptr;
    QDockWidget* m_layersDock = nullptr;
    QDockWidget* m_propsDock = nullptr;
    QDockWidget* m_paletteDock = nullptr;
    QDockWidget* m_timelineDock = nullptr;
    QDockWidget* m_logDock = nullptr;
    QDockWidget* m_previewDock = nullptr;
    PreviewPanel* m_preview = nullptr;
    QDockWidget* m_aiDock = nullptr;
    AiPanel* m_aiPanel = nullptr;
    AiService* m_ai = nullptr;
    QAction* m_onionAction = nullptr;
    LayersPanel* m_layers = nullptr;
    PropertiesPanel* m_props = nullptr;
    PalettePanel* m_palette = nullptr;
    TimelinePanel* m_timeline = nullptr;
    LogPanel* m_log = nullptr;

    QLabel* m_statusCursor = nullptr;
    QLabel* m_statusZoom = nullptr;
    QLabel* m_statusCanvas = nullptr;

    QList<QAction*> m_docActions;          // enabled only with an open document
    QAction* m_gridAction = nullptr;
    QAction* m_editMaskAction = nullptr;
    QAction* m_disableMaskAction = nullptr;
    QAction* m_addMaskAction = nullptr;
    QAction* m_addMaskSelAction = nullptr;
    QAction* m_applyMaskAction = nullptr;
    QAction* m_deleteMaskAction = nullptr;
    QAction* m_mergeDownAction = nullptr;
    QAction* m_adjSettingsAction = nullptr;
    QAction* m_clipAction = nullptr;
    QActionGroup* m_tileGroup = nullptr;
    std::filesystem::path brushDir() const;
    void loadBrushTips();
    void addBrushTip(const QString& name, Image tip);
    QAction* m_darkAction = nullptr;
    QAction* m_lightAction = nullptr;
    QByteArray m_defaultState;
    QMenu* m_workspaceMenu = nullptr;
    void rebuildWorkspaceMenu();
    void applyBuiltinWorkspace(const QString& name);
    void applyWorkspace(const QString& name);
    void saveWorkspace();
    ShortcutRegistry m_shortcuts;
    std::unique_ptr<RecoveryStore> m_recovery;
    QTimer* m_autosaveTimer = nullptr;
    bool m_autosaveErrorShown = false;
};

} // namespace acestudio::ui
