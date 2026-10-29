#include "MainWindow.h"

#include "AppContext.h"
#include "CanvasWidget.h"
#include "DocumentView.h"
#include "NewProjectDialog.h"
#include "QtBridge.h"
#include "Theme.h"
#include "panels/LayersPanel.h"
#include "panels/LogPanel.h"
#include "panels/PalettePanel.h"
#include "panels/PropertiesPanel.h"
#include "panels/TimelinePanel.h"

#include "acestudio/core/Log.h"
#include "acestudio/core/ProjectIO.h"
#include "acestudio/core/Version.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace acestudio::ui {

namespace {
const QString kProjectFilter = QObject::tr("AceStudio Project (*.acestudio)");

QColor settingColor(const Settings& s, const std::string& key, const char* fallback) {
    const auto c = Rgba8::fromHex(s.get<std::string>(key));
    return c ? toQColor(*c) : QColor(fallback);
}
} // namespace

MainWindow::MainWindow(AppContext& ctx, QWidget* parent) : QMainWindow(parent), m_ctx(ctx) {
    setWindowTitle(QStringLiteral("AceStudio"));
    setAcceptDrops(true);
    setDockNestingEnabled(true);
    resize(1440, 900);

    m_tabs = new QTabWidget(this);
    m_tabs->setDocumentMode(true);
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    connect(m_tabs, &QTabWidget::tabCloseRequested, this, [this](int i) { closeTab(i); });
    connect(m_tabs, &QTabWidget::currentChanged, this, &MainWindow::onCurrentTabChanged);

    m_stack = new QStackedWidget(this);
    m_stack->addWidget(buildWelcome());
    m_stack->addWidget(m_tabs);
    setCentralWidget(m_stack);

    buildDocks();
    buildMenus();

    m_statusCursor = new QLabel(this);
    m_statusZoom = new QLabel(this);
    m_statusCanvas = new QLabel(this);
    for (QLabel* l : {m_statusCanvas, m_statusZoom, m_statusCursor}) {
        l->setMinimumWidth(110);
        statusBar()->addPermanentWidget(l);
    }

    connect(&m_ctx, &AppContext::recentProjectsChanged, this, &MainWindow::rebuildRecentMenus);
    rebuildRecentMenus();

    m_defaultState = saveState();
    restoreWindowState();
    onCurrentTabChanged();
}

// ------------------------------------------------------------------ layout

QWidget* MainWindow::buildWelcome() {
    auto* page = new QWidget(this);
    auto* title = new QLabel(QStringLiteral("<span style='font-size:22pt; font-weight:600'>AceStudio</span>"
                                            "<br><span style='color:palette(placeholder-text)'>%1 %2</span>")
                                 .arg(tr("Version"), QString::fromLatin1(kVersionString)), page);
    auto* newBtn = new QPushButton(tr("New Project…"), page);
    auto* openBtn = new QPushButton(tr("Open Project…"), page);
    newBtn->setDefault(true);
    connect(newBtn, &QPushButton::clicked, this, &MainWindow::newProject);
    connect(openBtn, &QPushButton::clicked, this, &MainWindow::openProjectDialog);

    m_welcomeRecent = new QListWidget(page);
    m_welcomeRecent->setMaximumWidth(560);
    m_welcomeRecent->setMinimumHeight(180);
    connect(m_welcomeRecent, &QListWidget::itemActivated, this,
            [this](QListWidgetItem* it) { openProject(it->data(Qt::UserRole).toString()); });

    auto* buttons = new QHBoxLayout;
    buttons->addWidget(newBtn);
    buttons->addWidget(openBtn);
    buttons->addStretch();

    auto* col = new QVBoxLayout;
    col->addWidget(title);
    col->addSpacing(16);
    col->addLayout(buttons);
    col->addSpacing(20);
    col->addWidget(new QLabel(tr("Recent projects"), page));
    col->addWidget(m_welcomeRecent);
    col->addStretch();

    auto* outer = new QHBoxLayout(page);
    outer->addStretch();
    outer->addLayout(col, 3);
    outer->addStretch();
    outer->setContentsMargins(40, 60, 40, 40);
    return page;
}

void MainWindow::buildDocks() {
    auto makeDock = [this](const QString& title, const QString& objName, QWidget* content, Qt::DockWidgetArea area) {
        auto* dock = new QDockWidget(title, this);
        dock->setObjectName(objName);   // required for saveState/restoreState
        dock->setWidget(content);
        dock->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
                          QDockWidget::DockWidgetFloatable);
        addDockWidget(area, dock);
        return dock;
    };
    m_layers = new LayersPanel(this);
    m_props = new PropertiesPanel(this);
    m_palette = new PalettePanel(this);
    m_timeline = new TimelinePanel(this);
    m_log = new LogPanel(this);

    m_layersDock = makeDock(tr("Layers"), QStringLiteral("dock.layers"), m_layers, Qt::RightDockWidgetArea);
    m_propsDock = makeDock(tr("Properties"), QStringLiteral("dock.properties"), m_props, Qt::RightDockWidgetArea);
    m_paletteDock = makeDock(tr("Palette"), QStringLiteral("dock.palette"), m_palette, Qt::RightDockWidgetArea);
    m_timelineDock = makeDock(tr("Timeline"), QStringLiteral("dock.timeline"), m_timeline, Qt::BottomDockWidgetArea);
    m_logDock = makeDock(tr("Log"), QStringLiteral("dock.log"), m_log, Qt::BottomDockWidgetArea);
    tabifyDockWidget(m_timelineDock, m_logDock);
    m_timelineDock->raise();
    resizeDocks({m_layersDock}, {300}, Qt::Horizontal);
    resizeDocks({m_timelineDock}, {130}, Qt::Vertical);
}

void MainWindow::buildMenus() {
    auto docAction = [this](QMenu* menu, const QString& text, const QKeySequence& key, auto slot) {
        QAction* a = menu->addAction(text);
        if (!key.isEmpty()) a->setShortcut(key);
        connect(a, &QAction::triggered, this, slot);
        m_docActions << a;
        return a;
    };
    auto planned = [](QMenu* menu, const QString& what) {
        QAction* a = menu->addAction(what);
        a->setEnabled(false);
        return a;
    };

    // File
    QMenu* file = menuBar()->addMenu(tr("&File"));
    file->addAction(tr("&New Project…"), QKeySequence::New, this, &MainWindow::newProject);
    file->addAction(tr("&Open…"), QKeySequence::Open, this, &MainWindow::openProjectDialog);
    m_recentMenu = file->addMenu(tr("Open &Recent"));
    file->addSeparator();
    docAction(file, tr("&Save"), QKeySequence::Save, [this] { save(currentView()); });
    docAction(file, tr("Save &As…"), QKeySequence::SaveAs, [this] { saveAs(currentView()); });
    docAction(file, tr("&Close"), QKeySequence::Close, [this] { closeTab(m_tabs->currentIndex()); });
    file->addSeparator();
    QAction* quit = file->addAction(tr("E&xit"), QKeySequence::Quit, this, &QWidget::close);
    quit->setMenuRole(QAction::QuitRole);

    // Menus whose functionality lands in later phases are present for
    // orientation but clearly marked unavailable (no fake actions).
    planned(menuBar()->addMenu(tr("&Edit")), tr("Undo/redo and clipboard — Phase 2"));

    // View
    QMenu* view = menuBar()->addMenu(tr("&View"));
    docAction(view, tr("Zoom &In"), QKeySequence::ZoomIn, [this] { currentView()->canvas()->zoomIn(); });
    docAction(view, tr("Zoom &Out"), QKeySequence::ZoomOut, [this] { currentView()->canvas()->zoomOut(); });
    docAction(view, tr("Zoom to &Fit"), QKeySequence(Qt::CTRL | Qt::Key_0), [this] { currentView()->canvas()->zoomToFit(); });
    docAction(view, tr("&Actual Size"), QKeySequence(Qt::CTRL | Qt::Key_1), [this] { currentView()->canvas()->zoomActualSize(); });
    view->addSeparator();
    m_gridAction = view->addAction(tr("Pixel &Grid"));
    m_gridAction->setCheckable(true);
    m_gridAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Apostrophe));
    m_gridAction->setChecked(m_ctx.settings().get<bool>("canvas.grid.visible"));
    connect(m_gridAction, &QAction::toggled, this, [this](bool on) {
        (void)m_ctx.settings().set("canvas.grid.visible", on);
        for (int i = 0; i < m_tabs->count(); ++i)
            static_cast<DocumentView*>(m_tabs->widget(i))->canvas()->setGridVisible(on);
    });
    view->addSeparator();
    QMenu* theme = view->addMenu(tr("&Theme"));
    auto* themeGroup = new QActionGroup(this);
    m_darkAction = theme->addAction(tr("Dark"));
    m_lightAction = theme->addAction(tr("Light"));
    for (QAction* a : {m_darkAction, m_lightAction}) { a->setCheckable(true); themeGroup->addAction(a); }
    (m_ctx.settings().get<std::string>("general.theme") == "light" ? m_lightAction : m_darkAction)->setChecked(true);
    connect(m_darkAction, &QAction::triggered, this, [this] { setThemeAndPersist(QStringLiteral("dark")); });
    connect(m_lightAction, &QAction::triggered, this, [this] { setThemeAndPersist(QStringLiteral("light")); });

    planned(menuBar()->addMenu(tr("&Image")), tr("Resize, crop and color modes — Phase 2"));
    planned(menuBar()->addMenu(tr("&Layer")), tr("Layer editing — Phase 2"));

    // Animation
    QMenu* anim = menuBar()->addMenu(tr("&Animation"));
    docAction(anim, tr("&Play / Pause"), QKeySequence(Qt::Key_Return), [this] { m_timeline->togglePlayback(); });
    docAction(anim, tr("&Next Frame"), QKeySequence(Qt::Key_Period), [this] { m_timeline->stepFrame(1); });
    docAction(anim, tr("P&revious Frame"), QKeySequence(Qt::Key_Comma), [this] { m_timeline->stepFrame(-1); });
    anim->addSeparator();
    planned(anim, tr("Frame editing and onion skin — Phase 3"));

    planned(menuBar()->addMenu(tr("AI &Studio")), tr("Local AI generation — Phase 4"));
    planned(menuBar()->addMenu(tr("&Tilesets")), tr("Tileset and terrain tools — Phase 6"));
    planned(menuBar()->addMenu(tr("T&ools")), tr("Drawing tools — Phase 2"));

    // Window
    QMenu* window = menuBar()->addMenu(tr("&Window"));
    for (QDockWidget* d : {m_layersDock, m_propsDock, m_paletteDock, m_timelineDock, m_logDock})
        window->addAction(d->toggleViewAction());
    window->addSeparator();
    window->addAction(tr("&Reset Layout"), this, &MainWindow::resetLayout);

    // Help
    QMenu* help = menuBar()->addMenu(tr("&Help"));
    help->addAction(tr("Open &Logs Folder"), this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(fromPath(m_ctx.logDir())));
    });
    help->addAction(tr("Third-Party &Notices"), this, &MainWindow::showNotices);
    QAction* about = help->addAction(tr("&About AceStudio"), this, &MainWindow::showAbout);
    about->setMenuRole(QAction::AboutRole);
}

void MainWindow::restoreWindowState() {
    const auto geom = QByteArray::fromBase64(QByteArray::fromStdString(m_ctx.settings().get<std::string>("window.geometry")));
    const auto state = QByteArray::fromBase64(QByteArray::fromStdString(m_ctx.settings().get<std::string>("window.state")));
    if (!geom.isEmpty()) restoreGeometry(geom);
    if (!state.isEmpty() && !restoreState(state)) ACE_LOG_WARN("ui", "Saved window layout was not usable; using default");
}

void MainWindow::saveWindowState() {
    (void)m_ctx.settings().set("window.geometry", saveGeometry().toBase64().toStdString());
    (void)m_ctx.settings().set("window.state", saveState().toBase64().toStdString());
}

void MainWindow::resetLayout() {
    for (QDockWidget* d : {m_layersDock, m_propsDock, m_paletteDock, m_timelineDock, m_logDock}) {
        d->setFloating(false);
        d->show();
    }
    restoreState(m_defaultState);
}

// ------------------------------------------------------------------ documents

DocumentView* MainWindow::currentView() const { return qobject_cast<DocumentView*>(m_tabs->currentWidget()); }

void MainWindow::addView(DocumentView* view) {
    CanvasAppearance look;
    const Settings& s = m_ctx.settings();
    look.background = settingColor(s, "canvas.background", "#1B1B20");
    look.checker1 = settingColor(s, "canvas.checker.color1", "#2A2A31");
    look.checker2 = settingColor(s, "canvas.checker.color2", "#34343C");
    look.checkerSize = s.get<int>("canvas.checker.size");
    look.gridVisible = s.get<bool>("canvas.grid.visible");
    look.gridColor = settingColor(s, "canvas.grid.color", "#FFFFFF1F");
    look.gridMinZoom = s.get<int>("canvas.grid.minZoom");
    look.zoomStep = s.get<double>("canvas.zoomStep");
    view->canvas()->setAppearance(look);

    connect(view, &DocumentView::modifiedChanged, this, [this, view] { updateTabTitle(view); });
    connect(view, &DocumentView::cursorMoved, this, [this](int x, int y, bool inside) {
        m_statusCursor->setText(inside ? QStringLiteral("X %1  Y %2").arg(x).arg(y) : QString());
    });
    connect(view, &DocumentView::zoomChanged, this,
            [this](double z) { m_statusZoom->setText(QStringLiteral("%1%").arg(qRound(z * 100))); });

    const int idx = m_tabs->addTab(view, view->displayName());
    updateTabTitle(view);
    m_tabs->setCurrentIndex(idx);
}

void MainWindow::updateTabTitle(DocumentView* view) {
    const int i = m_tabs->indexOf(view);
    if (i < 0) return;
    m_tabs->setTabText(i, view->displayName() + (view->isModified() ? QStringLiteral(" •") : QString()));
    m_tabs->setTabToolTip(i, view->filePath().isEmpty() ? tr("Not saved yet") : QDir::toNativeSeparators(view->filePath()));
    if (view == currentView()) onCurrentTabChanged();
}

void MainWindow::onCurrentTabChanged() {
    DocumentView* v = currentView();
    m_stack->setCurrentIndex(v ? 1 : 0);
    m_layers->setView(v);
    m_props->setView(v);
    m_palette->setView(v);
    m_timeline->setView(v);
    if (v) {
        setWindowTitle(QStringLiteral("%1%2 — AceStudio").arg(v->displayName(), v->isModified() ? QStringLiteral(" •") : QString()));
        m_statusCanvas->setText(QStringLiteral("%1 × %2").arg(v->document().width()).arg(v->document().height()));
        m_statusZoom->setText(QStringLiteral("%1%").arg(qRound(v->canvas()->zoom() * 100)));
    } else {
        setWindowTitle(QStringLiteral("AceStudio"));
        m_statusCanvas->clear();
        m_statusZoom->clear();
        m_statusCursor->clear();
    }
    updateActions();
}

void MainWindow::updateActions() {
    const bool hasDoc = currentView() != nullptr;
    for (QAction* a : m_docActions) a->setEnabled(hasDoc);
}

void MainWindow::newProject() {
    NewProjectDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted) return;
    auto doc = Document::create(dlg.canvasWidth(), dlg.canvasHeight(), dlg.colorMode());
    if (!doc) {
        QMessageBox::warning(this, tr("New Project"), qs(doc.error().message));
        return;
    }
    (void)doc.value().addLayer("Layer 1");
    for (int i = 1; i < dlg.frameCount(); ++i) (void)doc.value().insertFrame(i);
    auto* view = new DocumentView(std::move(doc).value(), QString(), ProjectInfo{}, m_tabs);
    addView(view);
    view->notifyDocumentChanged(true);   // new and unsaved
    ACE_LOG_INFO("ui", "Created new " + std::to_string(dlg.canvasWidth()) + "x" + std::to_string(dlg.canvasHeight()) + " project");
}

void MainWindow::openProjectDialog() {
    const QString start = m_ctx.recentProjects().isEmpty() ? QDir::homePath()
                                                           : QFileInfo(m_ctx.recentProjects().first()).absolutePath();
    const QStringList files = QFileDialog::getOpenFileNames(this, tr("Open Project"), start, kProjectFilter);
    for (const QString& f : files) openProject(f);
}

bool MainWindow::openProject(const QString& pathIn) {
    const QString path = QFileInfo(pathIn).absoluteFilePath();
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto* v = static_cast<DocumentView*>(m_tabs->widget(i));
        if (QFileInfo(v->filePath()) == QFileInfo(path)) {
            m_tabs->setCurrentIndex(i);
            return true;
        }
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    ProjectInfo info;
    auto doc = loadProject(toPath(path), &info);
    QApplication::restoreOverrideCursor();
    if (!doc) {
        QString msg = tr("Could not open %1.\n\n%2").arg(QDir::toNativeSeparators(path), qs(doc.error().message));
        if (doc.error().code == ErrorCode::NotFound) {
            m_ctx.removeRecentProject(path);
        } else if (doc.error().code == ErrorCode::CorruptData &&
                   QFileInfo::exists(path + QStringLiteral(".bak"))) {
            msg += tr("\n\nA backup from the previous save exists:\n%1\nYou can open it from File › Open.")
                       .arg(QDir::toNativeSeparators(path + QStringLiteral(".bak")));
        }
        QMessageBox::warning(this, tr("Open Project"), msg);
        return false;
    }
    addView(new DocumentView(std::move(doc).value(), path, info, m_tabs));
    m_ctx.addRecentProject(path);
    return true;
}

bool MainWindow::save(DocumentView* view) {
    if (!view) return false;
    if (view->filePath().isEmpty()) return saveAs(view);
    const QString err = view->saveTo(QString());
    if (!err.isEmpty()) {
        QMessageBox::critical(this, tr("Save"), tr("The project was not saved. Your previous file is unchanged.\n\n%1").arg(err));
        return false;
    }
    m_ctx.addRecentProject(view->filePath());
    statusBar()->showMessage(tr("Saved %1").arg(view->displayName()), 3000);
    return true;
}

bool MainWindow::saveAs(DocumentView* view) {
    if (!view) return false;
    QString suggested = view->filePath();
    if (suggested.isEmpty()) {
        const QString dir = m_ctx.recentProjects().isEmpty() ? QDir::homePath()
                                                             : QFileInfo(m_ctx.recentProjects().first()).absolutePath();
        suggested = QDir(dir).filePath(QStringLiteral("untitled.acestudio"));
    }
    QString path = QFileDialog::getSaveFileName(this, tr("Save Project As"), suggested, kProjectFilter);
    if (path.isEmpty()) return false;
    if (!path.endsWith(QStringLiteral(".acestudio"), Qt::CaseInsensitive)) {
        path += QStringLiteral(".acestudio");
        // The native dialog only confirmed overwriting the name without the
        // extension, so confirm again for the real target.
        if (QFileInfo::exists(path) &&
            QMessageBox::question(this, tr("Save Project As"), tr("%1 already exists. Replace it?\nA .bak copy of the old file will be kept.")
                                                                   .arg(QDir::toNativeSeparators(path))) != QMessageBox::Yes)
            return false;
    }
    const QString err = view->saveTo(path);
    if (!err.isEmpty()) {
        QMessageBox::critical(this, tr("Save As"), tr("The project was not saved.\n\n%1").arg(err));
        return false;
    }
    m_ctx.addRecentProject(path);
    updateTabTitle(view);
    return true;
}

bool MainWindow::maybeSave(DocumentView* view) {
    if (!view || !view->isModified()) return true;
    m_tabs->setCurrentWidget(view);
    const auto choice = QMessageBox::warning(
        this, tr("Unsaved Changes"), tr("Save changes to %1 before closing?").arg(view->displayName()),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (choice == QMessageBox::Cancel) return false;
    if (choice == QMessageBox::Save) return save(view);
    return true;
}

bool MainWindow::closeTab(int index) {
    auto* view = qobject_cast<DocumentView*>(m_tabs->widget(index));
    if (!view || !maybeSave(view)) return false;
    m_tabs->removeTab(m_tabs->indexOf(view));
    view->deleteLater();
    onCurrentTabChanged();
    return true;
}

// ------------------------------------------------------------------ misc

void MainWindow::rebuildRecentMenus() {
    const QStringList recent = m_ctx.recentProjects();
    m_recentMenu->clear();
    m_welcomeRecent->clear();
    for (const QString& p : recent) {
        const QString label = QStringLiteral("%1  —  %2").arg(QFileInfo(p).fileName(), QDir::toNativeSeparators(QFileInfo(p).absolutePath()));
        m_recentMenu->addAction(label, this, [this, p] { openProject(p); });
        auto* item = new QListWidgetItem(label, m_welcomeRecent);
        item->setData(Qt::UserRole, p);
    }
    if (recent.isEmpty()) {
        m_recentMenu->addAction(tr("No recent projects"))->setEnabled(false);
        auto* item = new QListWidgetItem(tr("No recent projects yet"), m_welcomeRecent);
        item->setFlags(Qt::NoItemFlags);
    } else {
        m_recentMenu->addSeparator();
        m_recentMenu->addAction(tr("Clear Recent Projects"), this, [this] { m_ctx.clearRecentProjects(); });
    }
}

void MainWindow::setThemeAndPersist(const QString& theme) {
    (void)m_ctx.settings().set("general.theme", theme.toStdString());
    applyTheme(*qApp, themeFromString(theme), settingColor(m_ctx.settings(), "general.accentColor", "#4F8CFF"));
}

void MainWindow::showAbout() {
    QMessageBox::about(this, tr("About AceStudio"),
        tr("<h3>AceStudio %1</h3>"
           "<p>AI-assisted game asset creation and 2D animation studio.</p>"
           "<p>Project format schema %2.</p>"
           "<p>Built with Qt %3, used under the GNU LGPL v3 and dynamically linked. "
           "See Help › Third-Party Notices.</p>")
            .arg(QString::fromLatin1(kVersionString)).arg(kProjectSchemaVersion).arg(QString::fromLatin1(qVersion())));
}

void MainWindow::showNotices() {
    QMessageBox box(this);
    box.setWindowTitle(tr("Third-Party Notices"));
    box.setTextFormat(Qt::RichText);
    box.setText(tr(
        "<p>AceStudio includes the following third-party software:</p><ul>"
        "<li><b>Qt %1</b> — GNU LGPL v3. Dynamically linked; you may replace the Qt libraries. "
        "Source: https://download.qt.io/</li>"
        "<li><b>nlohmann/json 3.12.0</b> — MIT License</li>"
        "<li><b>miniz 3.1.2</b> — MIT License</li>"
        "<li><b>stb_image</b> — MIT License / Public Domain</li>"
        "</ul><p>Full license texts will ship in the installer's <i>licenses</i> folder "
        "(installer packaging is not built yet).</p>")
                    .arg(QString::fromLatin1(qVersion())));
    box.exec();
}

void MainWindow::closeEvent(QCloseEvent* e) {
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (!maybeSave(static_cast<DocumentView*>(m_tabs->widget(i)))) {
            e->ignore();
            return;
        }
    }
    saveWindowState();
    m_ctx.saveSettings();
    ACE_LOG_INFO("app", "Shutting down");
    e->accept();
}

void MainWindow::dragEnterEvent(QDragEnterEvent* e) {
    if (!e->mimeData()->hasUrls()) return;
    for (const QUrl& u : e->mimeData()->urls())
        if (u.isLocalFile() && u.toLocalFile().endsWith(QStringLiteral(".acestudio"), Qt::CaseInsensitive)) {
            e->acceptProposedAction();
            return;
        }
}

void MainWindow::dropEvent(QDropEvent* e) {
    for (const QUrl& u : e->mimeData()->urls())
        if (u.isLocalFile() && u.toLocalFile().endsWith(QStringLiteral(".acestudio"), Qt::CaseInsensitive))
            openProject(u.toLocalFile());
}

} // namespace acestudio::ui
