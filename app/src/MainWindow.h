#pragma once

#include <QMainWindow>

class QAction;
class QDockWidget;
class QLabel;
class QListWidget;
class QMenu;
class QStackedWidget;
class QTabWidget;

namespace acestudio::ui {

class AppContext;
class DocumentView;
class LayersPanel;
class LogPanel;
class PalettePanel;
class PropertiesPanel;
class TimelinePanel;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(AppContext& ctx, QWidget* parent = nullptr);

    // Opens a project; returns false (after informing the user) on failure.
    bool openProject(const QString& path);

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

    AppContext& m_ctx;
    QStackedWidget* m_stack = nullptr;
    QTabWidget* m_tabs = nullptr;
    QListWidget* m_welcomeRecent = nullptr;
    QMenu* m_recentMenu = nullptr;

    QDockWidget* m_layersDock = nullptr;
    QDockWidget* m_propsDock = nullptr;
    QDockWidget* m_paletteDock = nullptr;
    QDockWidget* m_timelineDock = nullptr;
    QDockWidget* m_logDock = nullptr;
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
    QAction* m_darkAction = nullptr;
    QAction* m_lightAction = nullptr;
    QByteArray m_defaultState;
};

} // namespace acestudio::ui
