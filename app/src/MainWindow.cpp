#include "MainWindow.h"
#include "licensing/Licensing.h"
#include "scripting/AppBridgeServer.h"
#include "scripting/McpSetupDialog.h"
#include "scripting/ScriptDialog.h"

#include "AppContext.h"
#include "CanvasWidget.h"
#include "DocumentView.h"
#include "NewProjectDialog.h"
#include "QtBridge.h"
#include "Theme.h"
#include "panels/LayersPanel.h"
#include "panels/LogPanel.h"
#include "panels/PalettePanel.h"
#include "panels/PreviewPanel.h"
#include "AnimationDialogs.h"
#include "ExportDialog.h"
#include "assets/AssetDialogs.h"
#include "assets/TilesetDialog.h"
#include "ai/AnimationDialog.h"
#include "ai/CharacterDialog.h"
#include "ai/AiPanel.h"
#include "ai/AiService.h"
#include "panels/PropertiesPanel.h"
#include "panels/TimelinePanel.h"
#include "panels/ToolsPanel.h"
#include "ImageDialogs.h"
#include "LayerDialogs.h"
#include "acestudio/core/PaletteIO.h"
#include "ToolState.h"
#include "acestudio/core/PngCodec.h"

#include "acestudio/core/Log.h"
#include "acestudio/core/ProjectIO.h"
#include "acestudio/core/Version.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QGuiApplication>
#include <QScreen>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>

#include <algorithm>
#include <QFileInfo>
#include <QShortcut>
#include <QTimer>
#include <QLabel>
#include <QListWidget>
#include <QMenuBar>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QScrollArea>
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

namespace {
// Bump when the default dock arrangement changes, so saved layouts from older
// versions are not restored over it.
constexpr int kLayoutVersion = 2;
} // namespace

MainWindow::MainWindow(AppContext& ctx, QWidget* parent) : QMainWindow(parent), m_ctx(ctx) {
    setWindowTitle(QStringLiteral("AceStudio"));
    setAcceptDrops(true);
    setDockNestingEnabled(true);
    // Default size, but never larger than the screen (minus the taskbar).
    QSize size(1440, 900);
    if (QScreen* scr = QGuiApplication::primaryScreen())
        size = size.boundedTo(scr->availableGeometry().size() * 0.9);
    resize(size);

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

    initLicensing(m_ctx.dataDir());
    m_tools = new ToolState(this);
    buildDocks();
    m_bridge = new AppBridgeServer(
        {[this] { return currentView(); },
         [this] {
             std::vector<DocumentView*> v;
             for (int i = 0; i < m_tabs->count(); ++i) v.push_back(static_cast<DocumentView*>(m_tabs->widget(i)));
             return v;
         },
         [this](const QString& p, QString* e) { return openAnyFile(p, e); }, scriptsFolder(m_ctx.dataDir())},
        this);
    if (m_ctx.settings().get<bool>("mcp.bridgeEnabled")) (void)m_bridge->start();
    buildMenus();

    m_statusCursor = new QLabel(this);
    m_statusZoom = new QLabel(this);
    m_statusCanvas = new QLabel(this);
    for (QLabel* l : {m_statusCanvas, m_statusZoom, m_statusCursor}) {
        l->setMinimumWidth(110);
        statusBar()->addPermanentWidget(l);
    }
    // Trial / licence state; click it to open Help › Licence.
    m_statusLicence = new QPushButton(this);
    m_statusLicence->setFlat(true);
    m_statusLicence->setAccessibleName(tr("Licence status"));
    connect(m_statusLicence, &QPushButton::clicked, this, &MainWindow::showLicence);
    statusBar()->addPermanentWidget(m_statusLicence);
    updateLicenceStatus();
    if (!writesAllowed(currentLicence()))
        QTimer::singleShot(0, this, [this] {
            statusBar()->showMessage(tr("The trial has ended: you can open and view your work; saving and exporting need a licence key "
                                        "(Help › Licence)."),
                                     15000);
        });

    connect(&m_ctx, &AppContext::recentProjectsChanged, this, &MainWindow::rebuildRecentMenus);
    rebuildRecentMenus();

    m_recovery = std::make_unique<RecoveryStore>(m_ctx.dataDir() / "recovery");
    loadBrushTips();
    m_autosaveTimer = new QTimer(this);
    connect(m_autosaveTimer, &QTimer::timeout, this, &MainWindow::autosaveTick);
    // Buffered log lines reach the disk within a second even when nothing
    // else is logged (the logger itself flushes on warnings and errors).
    auto* logFlush = new QTimer(this);
    connect(logFlush, &QTimer::timeout, this, [] { Logger::instance().flush(); });
    logFlush->start(1000);
    applyAutosaveInterval();

    m_shortcuts.collect(menuBar(), m_recentMenu);
    m_shortcuts.apply(m_ctx.settings().get("keyboard.shortcuts"));
    for (const QString& c : m_shortcuts.conflicts()) ACE_LOG_WARN("shortcuts", "Conflicting shortcut " + c.toStdString());

    m_defaultState = saveState(kLayoutVersion);
    if (qEnvironmentVariableIsSet("ACESTUDIO_DEBUG_LAYOUT"))
        for (QDockWidget* d : findChildren<QDockWidget*>())
            ACE_LOG_INFO("layout", d->objectName().toStdString() + " min " + std::to_string(d->minimumSizeHint().width()) + "x" +
                                       std::to_string(d->minimumSizeHint().height()));
    restoreWindowState();
    onCurrentTabChanged();
}

// ------------------------------------------------------------------ layout

QWidget* MainWindow::buildWelcome() {
    auto* page = new QWidget(this);
    auto* title = new QLabel(QStringLiteral("<span style='font-size:22pt; font-weight:600'>AceStudio</span>"
                                            "<br><span style='color:%3'>%1 %2</span>")
                                 .arg(tr("Version"), QString::fromLatin1(kVersionString),
                                      palette().color(QPalette::PlaceholderText).name()), page);
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
    auto makeDock = [this](const QString& title, const QString& objName, QWidget* content, Qt::DockWidgetArea area,
                           bool scroll = false) {
        auto* dock = new QDockWidget(title, this);
        dock->setObjectName(objName);   // required for saveState/restoreState
        if (scroll) {
            // Form-like panels scroll on short screens instead of squeezing their controls.
            auto* scroller = new QScrollArea(dock);
            scroller->setWidget(content);
            scroller->setWidgetResizable(true);
            scroller->setFrameShape(QFrame::NoFrame);
            scroller->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            dock->setWidget(scroller);
        } else {
            dock->setWidget(content);
        }
        dock->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
                          QDockWidget::DockWidgetFloatable);
        addDockWidget(area, dock);
        return dock;
    };
    m_layers = new LayersPanel(this);
    m_props = new PropertiesPanel(this);
    m_palette = new PalettePanel(m_tools, this);
    m_toolsPanel = new ToolsPanel(m_tools, this);
    m_timeline = new TimelinePanel(this);
    m_log = new LogPanel(this);
    m_ai = new AiService(m_ctx, this);
    m_aiPanel = new AiPanel(*m_ai, m_ctx, this);
    connect(m_aiPanel, &AiPanel::statusMessage, this, [this](const QString& t) { statusBar()->showMessage(t, 6000); });
    connect(m_aiPanel, &AiPanel::resultAsDocument, this, [this](const Image& img, const QString& name) { openImageAsDocument(img, name); });
    connect(m_aiPanel, &AiPanel::resultAsAnimation, this, &MainWindow::openAnimationAsDocument);
    connect(m_aiPanel, &AiPanel::resultAsLayer, this, [this](DocumentView* v, const Image& img, const QString& name, LayerId above, int frame) {
        // The view may have been closed while the job ran.
        for (int i = 0; i < m_tabs->count(); ++i)
            if (m_tabs->widget(i) == v) {
                m_tabs->setCurrentIndex(i);
                insertImageAsLayer(v, img, name, v->document().layer(above) ? std::optional<LayerId>(above) : std::nullopt,
                                   frame < v->document().frameCount() ? frame : -1);
                return;
            }
        openImageAsDocument(img, name);
    });
    m_preview = new PreviewPanel(this);
    m_preview->setZoom(m_ctx.settings().get<int>("animation.preview.zoom"));
    connect(m_preview, &PreviewPanel::zoomChanged, this, [this](int z) { (void)m_ctx.settings().set("animation.preview.zoom", z); });
    connect(m_timeline, &TimelinePanel::tagsRequested, this, &MainWindow::editTags);

    m_toolsDock = makeDock(tr("Tools"), QStringLiteral("dock.tools"), m_toolsPanel, Qt::LeftDockWidgetArea, true);
    m_layersDock = makeDock(tr("Layers"), QStringLiteral("dock.layers"), m_layers, Qt::RightDockWidgetArea);
    m_propsDock = makeDock(tr("Properties"), QStringLiteral("dock.properties"), m_props, Qt::RightDockWidgetArea, true);
    m_paletteDock = makeDock(tr("Palette"), QStringLiteral("dock.palette"), m_palette, Qt::RightDockWidgetArea, true);
    m_timelineDock = makeDock(tr("Timeline"), QStringLiteral("dock.timeline"), m_timeline, Qt::BottomDockWidgetArea);
    m_logDock = makeDock(tr("Log"), QStringLiteral("dock.log"), m_log, Qt::BottomDockWidgetArea);
    m_previewDock = makeDock(tr("Preview"), QStringLiteral("dock.preview"), m_preview, Qt::RightDockWidgetArea);
    m_aiDock = makeDock(tr("AI Studio"), QStringLiteral("dock.ai"), m_aiPanel, Qt::RightDockWidgetArea);
    tabifyDockWidget(m_layersDock, m_aiDock);
    m_layersDock->raise();
    // Two tab groups on the right keep the column's minimum height small
    // enough for 1366x768-class screens (four stacked docks did not fit a
    // 1456x819 logical desktop: found on a real Windows PC).
    tabifyDockWidget(m_paletteDock, m_previewDock);
    tabifyDockWidget(m_paletteDock, m_propsDock);
    m_paletteDock->raise();
    tabifyDockWidget(m_timelineDock, m_logDock);
    m_timelineDock->raise();
    resizeDocks({m_layersDock, m_toolsDock}, {300, 170}, Qt::Horizontal);
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
    file->addAction(tr("&Import PNG as New Project…"), this, [this] { importPng(false); });
    docAction(file, tr("Import PNG as &Layer…"), QKeySequence(), [this] { importPng(true); });
    docAction(file, tr("&Export Frame as PNG…"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E), [this] { exportPng(); });
    QMenu* exportMenu = file->addMenu(tr("Export &Animation"));
    docAction(exportMenu, tr("&Sprite Sheet…"), QKeySequence(), [this] { exportAnimation(static_cast<int>(ExportKind::SpriteSheet)); });
    docAction(exportMenu, tr("Animated &GIF…"), QKeySequence(), [this] { exportAnimation(static_cast<int>(ExportKind::Gif)); });
    docAction(exportMenu, tr("Animated &PNG (APNG)…"), QKeySequence(), [this] { exportAnimation(static_cast<int>(ExportKind::Apng)); });
    docAction(exportMenu, tr("Animated &WebP…"), QKeySequence(), [this] { exportAnimation(static_cast<int>(ExportKind::Webp)); });
    docAction(exportMenu, tr("PNG Se&quence…"), QKeySequence(), [this] { exportAnimation(static_cast<int>(ExportKind::PngSequence)); });
    file->addSeparator();
    QAction* quit = file->addAction(tr("E&xit"), QKeySequence::Quit, this, &QWidget::close);
    quit->setMenuRole(QAction::QuitRole);

    // Menus whose functionality lands in later phases are present for
    // orientation but clearly marked unavailable (no fake actions).
    QMenu* edit = menuBar()->addMenu(tr("&Edit"));
    m_undoAction = docAction(edit, tr("&Undo"), QKeySequence::Undo, [this] { report(currentView()->editor().undo()); });
    m_redoAction = docAction(edit, tr("&Redo"), QKeySequence(), [this] { report(currentView()->editor().redo()); });
    m_redoAction->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_Y), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z)});
    edit->addSeparator();
    docAction(edit, tr("Select &All"), QKeySequence::SelectAll, [this] { currentView()->selectAll(); });
    docAction(edit, tr("&Deselect"), QKeySequence(Qt::CTRL | Qt::Key_D), [this] { currentView()->deselect(); });
    docAction(edit, tr("D&elete Selection / Clear Layer"), QKeySequence::Delete, [this] { currentView()->clearSelectionContents(); });
    docAction(edit, tr("&Free Transform"), QKeySequence(Qt::CTRL | Qt::Key_T), [this] { currentView()->beginFreeTransform(); })
        ->setToolTip(tr("Move, scale and rotate the selection (or the layer) with handles; Enter applies"));
    docAction(edit, tr("&Invert Selection"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_I), [this] {
        auto* v = currentView();
        Mask inv = v->hasSelection() ? *v->selection() : Mask(v->document().width(), v->document().height());
        inv.invert();
        report(v->editor().setSelection(std::move(inv), "Invert Selection"));   // one undo step
    });
    edit->addSeparator();
    QMenu* brushes = edit->addMenu(tr("&Brush Tips"));
    docAction(brushes, tr("New Brush from &Selection"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_B), [this] {
        auto* v = currentView();
        const Image src = v->editingMask() ? v->document().renderMask(v->currentLayer(), v->currentFrame())
                                           : v->document().renderLayer(v->currentLayer(), v->currentFrame());
        auto tip = brushTipFromSelection(src, v->hasSelection() ? *v->selection() : Mask());
        if (!tip) { report(tip.error()); return; }
        addBrushTip(tr("Brush %1").arg(m_tools->tips().size() + 1), std::move(tip).value());
    });
    brushes->addAction(tr("&Import Brush Tip…"), this, [this] {
        const QStringList files = QFileDialog::getOpenFileNames(this, tr("Import Brush Tips"), QString(), tr("PNG images (*.png)"));
        for (const QString& f : files) {
            auto tip = loadBrushTip(toPath(f));
            if (!tip) { report(tip.error()); continue; }
            addBrushTip(QFileInfo(f).completeBaseName(), std::move(tip).value());
        }
    });
    brushes->addAction(tr("&Export Current Brush Tip…"), this, [this] {
        const int i = m_tools->currentTip();
        if (i < 0) { report(Error{ErrorCode::InvalidArgument, "No brush tip is selected"}); return; }
        const auto& tip = m_tools->tips()[static_cast<std::size_t>(i)];
        if (!licenceAllows(this, tr("Exporting"))) return;
        const QString f = QFileDialog::getSaveFileName(this, tr("Export Brush Tip"), tip.name + QStringLiteral(".png"), tr("PNG images (*.png)"));
        if (!f.isEmpty()) report(saveBrushTip(*tip.image, toPath(f)));
    });
    brushes->addAction(tr("&Delete Current Brush Tip"), this, [this] {
        const int i = m_tools->currentTip();
        if (i < 0) return;
        const QString name = m_tools->tips()[static_cast<std::size_t>(i)].name;
        if (QMessageBox::question(this, tr("Delete Brush Tip"), tr("Delete the brush tip \"%1\"?").arg(name)) != QMessageBox::Yes) return;
        std::error_code ec;
        std::filesystem::remove(brushDir() / toPath(name + QStringLiteral(".png")), ec);
        m_tools->removeTip(i);
    });
    edit->addSeparator();
    edit->addAction(tr("&Keyboard Shortcuts…"), this, &MainWindow::editShortcuts);

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
    QMenu* tiled = view->addMenu(tr("&Tiled Mode"));
    m_tileGroup = new QActionGroup(this);
    const std::pair<TileMode, QString> tileModes[] = {{TileMode::None, tr("&Off")}, {TileMode::Horizontal, tr("&Horizontal")},
                                                       {TileMode::Vertical, tr("&Vertical")}, {TileMode::Both, tr("&Both")}};
    for (const auto& [mode, label] : tileModes) {
        QAction* a = docAction(tiled, label, QKeySequence(), [this, mode = mode] { report(currentView()->editor().setTileMode(mode)); });
        a->setCheckable(true);
        a->setData(static_cast<int>(mode));
        m_tileGroup->addAction(a);
    }
    view->addSeparator();
    QMenu* theme = view->addMenu(tr("&Theme"));
    auto* themeGroup = new QActionGroup(this);
    m_darkAction = theme->addAction(tr("Dark"));
    m_lightAction = theme->addAction(tr("Light"));
    QAction* contrast = theme->addAction(tr("High Contrast"));
    contrast->setToolTip(tr("White on black with yellow selection and focus. Chosen automatically when Windows uses a contrast theme."));
    for (QAction* a : {m_darkAction, m_lightAction, contrast}) { a->setCheckable(true); themeGroup->addAction(a); }
    const std::string savedTheme = m_ctx.settings().get<std::string>("general.theme");
    (systemHighContrast() || savedTheme == "highContrast" ? contrast : savedTheme == "light" ? m_lightAction : m_darkAction)->setChecked(true);
    connect(m_darkAction, &QAction::triggered, this, [this] { setThemeAndPersist(QStringLiteral("dark")); });
    connect(m_lightAction, &QAction::triggered, this, [this] { setThemeAndPersist(QStringLiteral("light")); });
    connect(contrast, &QAction::triggered, this, [this] { setThemeAndPersist(QStringLiteral("highContrast")); });
    QMenu* textSize = view->addMenu(tr("Te&xt Size"));
    auto* sizeGroup = new QActionGroup(this);
    for (int pct : {100, 125, 150, 175, 200}) {
        QAction* a = textSize->addAction(tr("%1%").arg(pct));
        a->setCheckable(true);
        a->setChecked(m_ctx.settings().get<int>("general.textScale") == pct);
        sizeGroup->addAction(a);
        connect(a, &QAction::triggered, this, [this, pct] {
            (void)m_ctx.settings().set("general.textScale", pct);
            applyTextScale(*qApp, pct);
        });
    }

    QMenu* image = menuBar()->addMenu(tr("&Image"));
    docAction(image, tr("&Fit Canvas to Content"), QKeySequence(), [this] {
        report(currentView()->editor().fitCanvasToContent());
    })->setToolTip(tr("Resize the canvas to the pixels on every layer and frame, including pixels past the edges"));
    docAction(image, tr("&Canvas Size…"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_C), [this] {
        auto* v = currentView();
        CanvasSizeDialog dlg(v->document().width(), v->document().height(), this);
        if (dlg.exec() == QDialog::Accepted)
            report(v->editor().resizeCanvas(dlg.newWidth(), dlg.newHeight(), dlg.offsetX(), dlg.offsetY()));
    });
    docAction(image, tr("C&rop to Selection"), QKeySequence(), [this] {
        auto* v = currentView();
        if (!v->hasSelection()) { report(Error{ErrorCode::InvalidArgument, "Make a selection first"}); return; }
        report(v->editor().cropCanvas(v->selection()->bounds()));
    });
    image->addSeparator();
    docAction(image, tr("Flip Canvas &Horizontal"), QKeySequence(), [this] { report(currentView()->editor().flipCanvas(true)); });
    docAction(image, tr("Flip Canvas &Vertical"), QKeySequence(), [this] { report(currentView()->editor().flipCanvas(false)); });
    docAction(image, tr("Rotate Canvas 90° Clockwise"), QKeySequence(), [this] { report(currentView()->editor().rotateCanvas90(true)); });
    docAction(image, tr("Rotate Canvas 90° Counter-clockwise"), QKeySequence(), [this] { report(currentView()->editor().rotateCanvas90(false)); });
    docAction(image, tr("Rotate Canvas 180°"), QKeySequence(), [this] { report(currentView()->editor().rotateCanvas180()); });
    image->addSeparator();
    docAction(image, tr("Flip Layer Horizontal"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_H), [this] {
        transformCurrentLayer(tr("Flip Layer Horizontal"), false, [](const Image& i) { return flipHorizontal(i); });
    });
    docAction(image, tr("Flip Layer Vertical"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_V), [this] {
        transformCurrentLayer(tr("Flip Layer Vertical"), false, [](const Image& i) { return flipVertical(i); });
    });
    docAction(image, tr("Rotate Layer 90° Clockwise"), QKeySequence(), [this] {
        transformCurrentLayer(tr("Rotate Layer 90°"), false, [](const Image& i) { return rotate90(i, true); });
    });
    docAction(image, tr("Rotate Layer 90° Counter-clockwise"), QKeySequence(), [this] {
        transformCurrentLayer(tr("Rotate Layer -90°"), false, [](const Image& i) { return rotate90(i, false); });
    });
    docAction(image, tr("R&otate Layer (RotSprite)…"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R), [this] {
        RotateDialog dlg(this);
        if (dlg.exec() != QDialog::Accepted) return;
        const double deg = dlg.degrees();
        const bool expand = dlg.expand();
        transformCurrentLayer(tr("Rotate Layer %1°").arg(deg), dlg.allFrames(),
                              [deg, expand](const Image& i) { return rotSprite(i, deg, 24, expand); });
    });
    docAction(image, tr("&Scale Layer…"), QKeySequence(), [this] {
        ScaleDialog dlg(this);
        if (dlg.exec() != QDialog::Accepted) return;
        const double sx = dlg.percentX() / 100.0, sy = dlg.percentY() / 100.0;
        transformCurrentLayer(tr("Scale Layer"), dlg.allFrames(), [sx, sy](const Image& i) {
            auto r = scaleNearest(i, std::max(1, static_cast<int>(std::lround(i.width() * sx))),
                                  std::max(1, static_cast<int>(std::lround(i.height() * sy))));
            return r ? std::move(r).value() : i;
        });
    });
    docAction(image, tr("Out&line…"), QKeySequence(), [this] {
        OutlineDialog dlg(this);
        if (dlg.exec() != QDialog::Accepted) return;
        const QColor c = m_tools->primary();
        const Rgba8 color{static_cast<std::uint8_t>(c.red()), static_cast<std::uint8_t>(c.green()),
                          static_cast<std::uint8_t>(c.blue()), static_cast<std::uint8_t>(c.alpha())};
        const OutlineMode mode = dlg.mode();
        const bool diag = dlg.diagonal();
        transformCurrentLayer(tr("Outline"), dlg.allFrames(), [=](const Image& i) { return outline(i, color, mode, diag); });
    });
    image->addSeparator();
    QMenu* modeMenu = image->addMenu(tr("Color &Mode"));
    docAction(modeMenu, tr("RGBA"), QKeySequence(), [this] { report(currentView()->editor().convertColorMode(ColorMode::Rgba)); });
    docAction(modeMenu, tr("Indexed (nearest palette colour)"), QKeySequence(), [this] {
        report(currentView()->editor().convertColorMode(ColorMode::Indexed, false));
    });
    docAction(modeMenu, tr("Indexed (ordered dither)"), QKeySequence(), [this] {
        report(currentView()->editor().convertColorMode(ColorMode::Indexed, true));
    });
    docAction(modeMenu, tr("Grayscale"), QKeySequence(), [this] { report(currentView()->editor().convertColorMode(ColorMode::Grayscale)); });
    QMenu* layer = menuBar()->addMenu(tr("&Layer"));
    docAction(layer, tr("&New Layer"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N), [this] {
        auto* v = currentView();
        auto r = v->editor().addLayer("", LayerKind::Raster, v->currentLayer());
        if (r) v->setCurrentLayer(r.value()); else report(r.error());
    });
    docAction(layer, tr("New &Group"), QKeySequence(), [this] {
        auto* v = currentView();
        auto r = v->editor().addLayer("", LayerKind::Group, v->currentLayer());
        if (r) v->setCurrentLayer(r.value()); else report(r.error());
    });
    docAction(layer, tr("&Duplicate Layer"), QKeySequence(Qt::CTRL | Qt::Key_J), [this] {
        auto* v = currentView();
        auto r = v->editor().duplicateLayer(v->currentLayer());
        if (r) v->setCurrentLayer(r.value()); else report(r.error());
    });
    m_mergeDownAction = docAction(layer, tr("&Merge Down"), QKeySequence(Qt::CTRL | Qt::Key_E), [this] {
        report(currentView()->editor().mergeDown(currentView()->currentLayer()));
    });
    layer->addSeparator();
    QMenu* maskMenu = layer->addMenu(tr("Layer &Mask"));
    m_addMaskAction = docAction(maskMenu, tr("&Add Mask (Reveal All)"), QKeySequence(), [this] {
        auto* v = currentView();
        if (report(v->editor().addLayerMask(v->currentLayer()))) v->setEditingMask(true);
    });
    m_addMaskSelAction = docAction(maskMenu, tr("Add Mask from &Selection"), QKeySequence(), [this] {
        auto* v = currentView();
        if (!v->hasSelection()) { report(Error{ErrorCode::InvalidArgument, "Make a selection first"}); return; }
        if (report(v->editor().addLayerMask(v->currentLayer(), v->selection()))) v->setEditingMask(true);
    });
    m_editMaskAction = docAction(maskMenu, tr("&Edit Mask"), QKeySequence(Qt::CTRL | Qt::Key_Backslash), [this] {
        currentView()->setEditingMask(!currentView()->editingMask());
    });
    m_editMaskAction->setCheckable(true);
    m_disableMaskAction = docAction(maskMenu, tr("&Disable Mask"), QKeySequence(), [this] {
        auto* v = currentView();
        const Layer* l = v->document().layer(v->currentLayer());
        if (l) report(v->editor().setLayerMaskEnabled(l->id, !l->maskEnabled));
    });
    m_disableMaskAction->setCheckable(true);
    m_applyMaskAction = docAction(maskMenu, tr("A&pply Mask"), QKeySequence(), [this] {
        report(currentView()->editor().removeLayerMask(currentView()->currentLayer(), true));
    });
    m_deleteMaskAction = docAction(maskMenu, tr("De&lete Mask"), QKeySequence(), [this] {
        report(currentView()->editor().removeLayerMask(currentView()->currentLayer(), false));
    });
    m_clipAction = docAction(layer, tr("&Clipping Mask"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_G), [this] {
        auto* v = currentView();
        const Layer* l = v->document().layer(v->currentLayer());
        if (l) report(v->editor().setLayerClipped(l->id, !l->clipped));
    });
    m_clipAction->setCheckable(true);
    m_clipAction->setToolTip(tr("Show this layer only where the layer below it has pixels"));
    QMenu* adjMenu = layer->addMenu(tr("New &Adjustment Layer"));
    const std::pair<AdjustmentType, QString> adjTypes[] = {
        {AdjustmentType::BrightnessContrast, tr("&Brightness / Contrast…")}, {AdjustmentType::HueSaturation, tr("&Hue / Saturation…")},
        {AdjustmentType::Levels, tr("&Levels…")}, {AdjustmentType::Curves, tr("&Curves…")},
        {AdjustmentType::Invert, tr("&Invert")}, {AdjustmentType::Posterize, tr("&Posterize…")}, {AdjustmentType::Threshold, tr("&Threshold…")}};
    for (const auto& [type, label] : adjTypes) {
        docAction(adjMenu, label, QKeySequence(), [this, type = type] {
            auto* v = currentView();
            const std::size_t before = v->editor().history().index();
            Adjustment adj{type};
            if (type == AdjustmentType::Posterize) adj.a = 4;
            if (type == AdjustmentType::Threshold) adj.a = 128;
            if (type == AdjustmentType::Levels) { adj.b = 255; adj.c = 100; }
            if (type == AdjustmentType::Curves) adj.points = {{0, 0}, {255, 255}};
            auto id = v->editor().addAdjustmentLayer(adj, v->currentLayer());
            if (!id) { report(id.error()); return; }
            v->setCurrentLayer(id.value());
            if (type == AdjustmentType::Invert) return;
            AdjustmentDialog dlg(v->editor(), id.value(), this);
            if (dlg.exec() != QDialog::Accepted)
                while (v->editor().history().index() > before && v->editor().history().canUndo()) (void)v->editor().undo();
        });
    }
    m_adjSettingsAction = docAction(layer, tr("Adjustment &Settings…"), QKeySequence(), [this] {
        auto* v = currentView();
        const Layer* l = v->document().layer(v->currentLayer());
        if (!l || l->kind != LayerKind::Adjustment) { report(Error{ErrorCode::InvalidArgument, "Select an adjustment layer"}); return; }
        AdjustmentDialog dlg(v->editor(), l->id, this);
        dlg.exec();
    });
    layer->addSeparator();
    docAction(layer, tr("De&lete Layer"), QKeySequence(), [this] {
        report(currentView()->editor().removeLayer(currentView()->currentLayer()));
    });

    // Animation
    QMenu* anim = menuBar()->addMenu(tr("&Animation"));
    docAction(anim, tr("&Play / Pause"), QKeySequence(Qt::Key_Return), [this] {
        // Enter also finishes a curve, polygon or move in progress.
        if (auto* v = currentView(); v && v->hasGesture()) { v->finishGesture(true); return; }
        m_timeline->togglePlayback();
    });
    docAction(anim, tr("&Next Frame"), QKeySequence(Qt::Key_Period), [this] { m_timeline->stepFrame(1); });
    docAction(anim, tr("P&revious Frame"), QKeySequence(Qt::Key_Comma), [this] { m_timeline->stepFrame(-1); });
    anim->addSeparator();
    docAction(anim, tr("New &Frame"), QKeySequence(Qt::ALT | Qt::Key_N), [this] {
        auto* v = currentView();
        const int at = v->currentFrame() + 1;
        auto r = v->editor().insertFrame(at, v->document().frames()[static_cast<std::size_t>(at - 1)].durationMs);
        if (r) v->setCurrentFrame(at); else report(r.error());
    });
    docAction(anim, tr("D&uplicate Frame"), QKeySequence(Qt::ALT | Qt::Key_D), [this] {
        auto* v = currentView();
        auto r = v->editor().duplicateFrame(v->currentFrame());
        if (r) v->setCurrentFrame(r.value()); else report(r.error());
    });
    docAction(anim, tr("Delete Fra&me"), QKeySequence(), [this] {
        report(currentView()->editor().removeFrame(currentView()->currentFrame()));
    });
    docAction(anim, tr("Move Frame &Left"), QKeySequence(Qt::ALT | Qt::Key_Left), [this] { m_timeline->moveCurrentFrame(-1); });
    docAction(anim, tr("Move Frame R&ight"), QKeySequence(Qt::ALT | Qt::Key_Right), [this] { m_timeline->moveCurrentFrame(1); });
    anim->addSeparator();
    docAction(anim, tr("&Tags…"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_T), [this] { editTags(); });
    anim->addSeparator();
    m_onionAction = anim->addAction(tr("&Onion Skin"));
    m_onionAction->setCheckable(true);
    m_onionAction->setShortcut(QKeySequence(Qt::Key_F3));
    m_onionAction->setToolTip(tr("Onion skin: show neighbouring frames (F3)"));
    m_onionAction->setChecked(m_ctx.settings().get<bool>("animation.onionSkin.enabled"));
    connect(m_onionAction, &QAction::toggled, this, [this](bool on) {
        (void)m_ctx.settings().set("animation.onionSkin.enabled", on);
        applyOnionSkin();
    });
    m_timeline->addActionButton(m_onionAction);
    anim->addAction(tr("Onion Skin &Settings…"), this, &MainWindow::editOnionSkin);

    QMenu* aiMenu = menuBar()->addMenu(tr("AI &Studio"));
    auto showAi = [this](int tab) { m_aiDock->show(); m_aiDock->raise(); m_aiPanel->focusTab(tab); };
    aiMenu->addAction(tr("&Generate Image…"), this, [showAi] { showAi(0); });
    aiMenu->addAction(tr("&Remove Background…"), this, [showAi] { showAi(1); });
    aiMenu->addAction(tr("AI &Upscale…"), this, [showAi] { showAi(2); });
    aiMenu->addSeparator();
    aiMenu->addAction(tr("&Characters…"), this, [this] {
        CharacterDialog dlg(m_ctx.dataDir() / "characters", currentView(), this);
        dlg.exec();
    });
    aiMenu->addAction(tr("Generate &Animation…"), this, [this] {
        AnimationDialog dlg(m_ctx, *m_ai, currentView(), [this](const std::vector<Image>& frames, const std::vector<int>& durations,
                                                               const QString& name) { openAnimationAsDocument(frames, durations, name); },
                            this);
        dlg.exec();
    });
    aiMenu->addSeparator();
    aiMenu->addAction(tr("&Models…"), m_aiPanel, &AiPanel::showModels);
    aiMenu->addAction(tr("&Set Up AI Runtime…"), m_aiPanel, &AiPanel::showSetup);
    aiMenu->addAction(tr("Restart AI &Worker"), m_ai, &AiService::restartWorker);
    aiMenu->addAction(tr("Open AI &Data Folder"), this, [this] {
        if (const auto* env = m_ai->environment()) {
            std::error_code ec;
            std::filesystem::create_directories(env->root, ec);
            QDesktopServices::openUrl(QUrl::fromLocalFile(fromPath(env->root)));
        }
    });
    QMenu* assets = menuBar()->addMenu(tr("A&ssets"));
    auto openImage = [this](const Image& img, const QString& name) { openImageAsDocument(img, name); };
    auto openFrames = [this](const std::vector<Image>& frames, const std::vector<int>& durations, const QString& name) {
        openAnimationAsDocument(frames, durations, name);
    };
    assets->addAction(tr("&Tileset Generator…"), this, [this, openImage] {
        TilesetDialog dlg(*m_ai, currentView(), openImage, this);
        dlg.exec();
    });
    assets->addAction(tr("New Tileset &Template…"), this, [this, openImage] { newTilesetTemplate(openImage, this); });
    assets->addAction(tr("Check Tileset &Seams…"), this, [this] { checkDocumentSeams(currentView(), this); });
    assets->addSeparator();
    assets->addAction(tr("&Parallax Background…"), this, [this] {
        ParallaxDialog dlg(*m_ai, currentView(), this);
        dlg.exec();
    });
    assets->addAction(tr("&UI Kit Generator…"), this, [this, openImage] {
        UiKitDialog dlg(currentView(), openImage, this);
        dlg.exec();
    });
    assets->addSeparator();
    assets->addAction(tr("Smart S&licer…"), this, [this, openFrames] {
        SliceDialog dlg(*m_ai, currentView(), openFrames, this);
        dlg.exec();
    });
    assets->addAction(tr("Pack Sprite &Atlas…"), this, [this] {
        AtlasDialog dlg(currentView(), this);
        dlg.exec();
    });
    // Scripts
    QMenu* scripts = menuBar()->addMenu(tr("S&cripts"));
    auto runScript = [this](const std::filesystem::path& scriptFile) {
        // Not modal: the editor stays usable while the script runs.
        auto* dlg = new ScriptDialog(scriptFile, currentView(), this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->setModal(false);
        dlg->show();
        dlg->start();
    };
    connect(scripts, &QMenu::aboutToShow, this, [this, scripts, runScript] {
        scripts->clear();
        const auto dir = scriptsFolder(m_ctx.dataDir());
        scripts->addAction(tr("&Run Script…"), this, [this, dir, runScript] {
            const QString f = QFileDialog::getOpenFileName(this, tr("Run Lua Script"), fromPath(dir), tr("Lua scripts (*.lua)"));
            if (!f.isEmpty()) runScript(toPath(f));
        });
        std::vector<std::filesystem::path> found;
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(dir, ec))
            if (e.path().extension() == ".lua") found.push_back(e.path());
        std::sort(found.begin(), found.end());
        if (!found.empty()) scripts->addSeparator();
        for (const auto& f : found)
            scripts->addAction(fromPath(f.stem()), this, [f, runScript] { runScript(f); });
        scripts->addSeparator();
        scripts->addAction(tr("Open Scripts &Folder"), this, [dir] { QDesktopServices::openUrl(QUrl::fromLocalFile(fromPath(dir))); });
        scripts->addSeparator();
        QAction* allow = scripts->addAction(tr("Allow AI &Assistants to Use the Open Document"));
        allow->setCheckable(true);
        allow->setChecked(m_bridge && m_bridge->running());
        allow->setToolTip(tr("Lets acestudio-mcp (Claude Code, Cursor, VS Code, Codex…) look at the open document and run "
                             "Lua scripts on it. Every change is one Edit > Undo step."));
        scripts->addAction(tr("Set &Up AI Assistants (MCP)…"), this, [this] {
            McpSetupDialog dlg(this);
            dlg.exec();
        });
        connect(allow, &QAction::toggled, this, [this](bool on) {
            (void)m_ctx.settings().set("mcp.bridgeEnabled", on);
            if (on) {
                if (!m_bridge->start()) statusBar()->showMessage(tr("Could not open the AI assistant bridge (see the log)"), 5000);
            } else {
                m_bridge->stop();
            }
        });
    });
    scripts->addAction(tr("&Run Script…"));   // replaced when the menu opens
    QMenu* toolsMenu = menuBar()->addMenu(tr("T&ools"));
    auto* toolGroup = new QActionGroup(this);
    for (Tool t : kAllTools) {
        QAction* a = toolsMenu->addAction(toolName(t));
        a->setShortcut(QKeySequence(toolShortcut(t)));
        a->setCheckable(true);
        a->setChecked(m_tools->tool() == t);
        toolGroup->addAction(a);
        connect(a, &QAction::triggered, this, [this, t] { m_tools->setTool(t); });
        connect(m_tools, &ToolState::changed, a, [this, a, t] { a->setChecked(m_tools->tool() == t); });
    }
    toolsMenu->addSeparator();
    toolsMenu->addAction(tr("Swap Colors"), QKeySequence(Qt::Key_X), m_tools, &ToolState::swapColors);
    toolsMenu->addAction(tr("Smaller Brush"), QKeySequence(Qt::Key_BracketLeft), this,
                         [this] { m_tools->setBrushSize(m_tools->brush().size - 1); });
    toolsMenu->addAction(tr("Larger Brush"), QKeySequence(Qt::Key_BracketRight), this,
                         [this] { m_tools->setBrushSize(m_tools->brush().size + 1); });

    // Window
    QMenu* window = menuBar()->addMenu(tr("&Window"));
    for (QDockWidget* d : {m_toolsDock, m_layersDock, m_aiDock, m_propsDock, m_paletteDock, m_previewDock, m_timelineDock, m_logDock})
        window->addAction(d->toggleViewAction());
    window->addSeparator();
    m_workspaceMenu = window->addMenu(tr("&Workspaces"));
    connect(m_workspaceMenu, &QMenu::aboutToShow, this, &MainWindow::rebuildWorkspaceMenu);
    rebuildWorkspaceMenu();
    window->addAction(tr("&Reset Layout"), this, &MainWindow::resetLayout);

    // Help
    QMenu* help = menuBar()->addMenu(tr("&Help"));
    help->addAction(tr("Open &Logs Folder"), this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(fromPath(m_ctx.logDir())));
    });
    help->addAction(tr("&Licence…"), this, &MainWindow::showLicence);
    help->addAction(tr("Third-Party &Notices"), this, &MainWindow::showNotices);
    QAction* about = help->addAction(tr("&About AceStudio"), this, &MainWindow::showAbout);
    about->setMenuRole(QAction::AboutRole);
}

void MainWindow::restoreWindowState() {
    const auto geom = QByteArray::fromBase64(QByteArray::fromStdString(m_ctx.settings().get<std::string>("window.geometry")));
    const auto state = QByteArray::fromBase64(QByteArray::fromStdString(m_ctx.settings().get<std::string>("window.state")));
    if (!geom.isEmpty()) restoreGeometry(geom);
    if (!state.isEmpty() && !restoreState(state, kLayoutVersion))
        ACE_LOG_INFO("ui", "Saved window layout is from an older version or not usable; using the default layout");
}

void MainWindow::saveWindowState() {
    (void)m_ctx.settings().set("window.geometry", saveGeometry().toBase64().toStdString());
    (void)m_ctx.settings().set("window.state", saveState(kLayoutVersion).toBase64().toStdString());
}

void MainWindow::resetLayout() {
    for (QDockWidget* d : {m_toolsDock, m_layersDock, m_aiDock, m_propsDock, m_paletteDock, m_previewDock, m_timelineDock, m_logDock}) {
        d->setFloating(false);
        d->show();
    }
    restoreState(m_defaultState, kLayoutVersion);
}

// ------------------------------------------------------------------ workspaces

namespace {
const char* const kBuiltinWorkspaces[] = {"Painting", "Animation", "AI", "Minimal"};
}

void MainWindow::applyBuiltinWorkspace(const QString& name) {
    resetLayout();   // every preset starts from the default arrangement
    if (name == QLatin1String("Animation")) {
        m_previewDock->show();
        m_previewDock->raise();
        resizeDocks({m_timelineDock}, {height() / 3}, Qt::Vertical);
    } else if (name == QLatin1String("AI")) {
        m_aiDock->show();
        m_aiDock->raise();
        m_logDock->show();
        m_logDock->raise();
        resizeDocks({m_aiDock}, {width() / 3}, Qt::Horizontal);
    } else if (name == QLatin1String("Minimal")) {
        for (QDockWidget* d : {m_aiDock, m_propsDock, m_paletteDock, m_previewDock, m_timelineDock, m_logDock}) d->hide();
    } else {   // Painting
        m_layersDock->raise();
        m_paletteDock->raise();
    }
}

void MainWindow::rebuildWorkspaceMenu() {
    m_workspaceMenu->clear();
    for (const char* n : kBuiltinWorkspaces) {
        const QString name = QString::fromLatin1(n);
        m_workspaceMenu->addAction(tr(n), this, [this, name] { applyBuiltinWorkspace(name); });
    }
    const nlohmann::json saved = m_ctx.settings().get("window.workspaces");
    if (saved.is_object() && !saved.empty()) {
        m_workspaceMenu->addSeparator();
        for (auto it = saved.begin(); it != saved.end(); ++it) {
            const QString name = QString::fromStdString(it.key());
            m_workspaceMenu->addAction(name, this, [this, name] { applyWorkspace(name); });
        }
    }
    m_workspaceMenu->addSeparator();
    m_workspaceMenu->addAction(tr("&Save Current Layout…"), this, &MainWindow::saveWorkspace);
    QMenu* del = m_workspaceMenu->addMenu(tr("&Delete"));
    del->setEnabled(saved.is_object() && !saved.empty());
    if (saved.is_object())
        for (auto it = saved.begin(); it != saved.end(); ++it) {
            const std::string key = it.key();
            del->addAction(QString::fromStdString(key), this, [this, key] {
                nlohmann::json w = m_ctx.settings().get("window.workspaces");
                if (w.is_object()) w.erase(key);
                (void)m_ctx.settings().set("window.workspaces", w);
                m_ctx.saveSettings();
            });
        }
}

void MainWindow::saveWorkspace() {
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Save Workspace"), tr("Name:"), QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    if (name.size() > 64) { statusBar()->showMessage(tr("Workspace names are limited to 64 characters"), 5000); return; }
    nlohmann::json w = m_ctx.settings().get("window.workspaces");
    if (!w.is_object()) w = nlohmann::json::object();
    const std::string key = name.toStdString();
    if (w.contains(key) && QMessageBox::question(this, tr("Save Workspace"), tr("Replace the workspace “%1”?").arg(name)) != QMessageBox::Yes)
        return;
    w[key] = saveState(kLayoutVersion).toBase64().toStdString();
    (void)m_ctx.settings().set("window.workspaces", w);
    m_ctx.saveSettings();
    statusBar()->showMessage(tr("Workspace “%1” saved").arg(name), 4000);
}

void MainWindow::applyWorkspace(const QString& name) {
    const nlohmann::json w = m_ctx.settings().get("window.workspaces");
    const std::string key = name.toStdString();
    if (!w.is_object() || !w.contains(key) || !w[key].is_string()) return;
    const QByteArray state = QByteArray::fromBase64(QByteArray::fromStdString(w[key].get<std::string>()));
    if (!restoreState(state, kLayoutVersion))
        statusBar()->showMessage(tr("Workspace “%1” was saved by an older version and cannot be restored").arg(name), 6000);
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
    if (m_onionAction && m_onionAction->isChecked()) view->setOnionSkin(onionSkinFromSettings());

    connect(view, &DocumentView::modifiedChanged, this, [this, view] { updateTabTitle(view); });
    connect(view, &DocumentView::documentChanged, this, [this, view] { if (view == currentView()) updateActions(); });
    connect(view, &DocumentView::editingMaskChanged, this, [this, view] { if (view == currentView()) updateActions(); });
    connect(view, &DocumentView::currentLayerChanged, this, [this, view] { if (view == currentView()) updateActions(); });
    connect(view, &DocumentView::statusMessage, this, [this](const QString& m) { statusBar()->showMessage(m, 4000); });
    connect(view, &DocumentView::cursorMoved, this, [this](int x, int y, bool inside) {
        m_statusCursor->setText(inside ? QStringLiteral("X %1  Y %2").arg(x).arg(y) : QString());
    });
    connect(view, &DocumentView::zoomChanged, this,
            [this](double z) { m_statusZoom->setText(QStringLiteral("%1%").arg(qRound(z * 100))); });

    const int idx = m_tabs->addTab(view, view->displayName());
    connect(view, &DocumentView::documentChanged, m_bridge, &AppBridgeServer::documentsChanged);
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
    if (m_bridge) m_bridge->documentsChanged();
    m_stack->setCurrentIndex(v ? 1 : 0);
    m_layers->setView(v);
    m_props->setView(v);
    m_palette->setView(v);
    m_timeline->setView(v);
    m_preview->setView(v);
    m_aiPanel->setView(v);
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
    DocumentView* v = currentView();
    const bool hasDoc = v != nullptr;
    for (QAction* a : m_docActions) a->setEnabled(hasDoc);
    if (hasDoc) {
        const Layer* l = v->document().layer(v->currentLayer());
        const bool mask = l && l->hasMask;
        m_editMaskAction->setEnabled(mask);
        m_editMaskAction->setChecked(v->editingMask());
        m_disableMaskAction->setEnabled(mask);
        m_applyMaskAction->setEnabled(mask && l->kind == LayerKind::Raster);
        m_deleteMaskAction->setEnabled(mask);
        const bool canMask = l && !mask;
        m_addMaskAction->setEnabled(canMask);
        m_addMaskSelAction->setEnabled(canMask);
        m_adjSettingsAction->setEnabled(l && l->kind == LayerKind::Adjustment);
        std::string why;
        const bool canMerge = l && v->editor().canMergeDown(l->id, &why);
        m_mergeDownAction->setToolTip(canMerge ? tr("Merge into the layer below") : QString::fromStdString(why));
        m_mergeDownAction->setEnabled(canMerge);
        m_disableMaskAction->setChecked(mask && !l->maskEnabled);
        m_clipAction->setChecked(l && l->clipped);
        for (QAction* a : m_tileGroup->actions()) a->setChecked(a->data().toInt() == static_cast<int>(v->document().tileMode()));
    }
    if (m_undoAction && m_redoAction) {
        const auto& h = hasDoc ? &v->editor().history() : nullptr;
        m_undoAction->setEnabled(h && h->canUndo());
        m_redoAction->setEnabled(h && h->canRedo());
        m_undoAction->setText(h && h->canUndo() ? tr("&Undo %1").arg(qs(h->undoLabel())) : tr("&Undo"));
        m_redoAction->setText(h && h->canRedo() ? tr("&Redo %1").arg(qs(h->redoLabel())) : tr("&Redo"));
    }
}

std::filesystem::path MainWindow::brushDir() const { return m_ctx.dataDir() / "brushes"; }

void MainWindow::loadBrushTips() {
    std::error_code ec;
    if (!std::filesystem::is_directory(brushDir(), ec)) return;
    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::directory_iterator(brushDir(), ec))
        if (e.path().extension() == ".png") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (const auto& f : files) {
        auto tip = loadBrushTip(f);
        if (tip) m_tools->addTip(fromPath(f.stem()), std::move(tip).value());
        else ACE_LOG_WARN("ui", "Skipping brush tip " + fromPath(f).toStdString() + ": " + tip.error().message);
    }
}

void MainWindow::addBrushTip(const QString& name, Image tip) {
    std::error_code ec;
    std::filesystem::create_directories(brushDir(), ec);
    if (auto st = saveBrushTip(tip, brushDir() / toPath(name + QStringLiteral(".png"))); !st) report(st);
    const int w = tip.width(), h = tip.height();
    m_tools->addTip(name, std::move(tip));
    m_tools->setBrushShape(BrushShape::Custom);
    statusBar()->showMessage(tr("Brush tip \"%1\" added (%2×%3)").arg(name).arg(w).arg(h), 4000);
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
    auto* view = new DocumentView(std::move(doc).value(), QString(), ProjectInfo{}, m_tools, m_tabs);
    addView(view);
    view->markNeverSaved();
    ACE_LOG_INFO("ui", "Created new " + std::to_string(dlg.canvasWidth()) + "x" + std::to_string(dlg.canvasHeight()) + " project");
}

void MainWindow::openProjectDialog() {
    const QString start = m_ctx.recentProjects().isEmpty() ? QDir::homePath()
                                                           : QFileInfo(m_ctx.recentProjects().first()).absolutePath();
    const QStringList files = QFileDialog::getOpenFileNames(this, tr("Open Project"), start, kProjectFilter);
    for (const QString& f : files) openProject(f);
}

bool MainWindow::openAnyFile(const QString& path, QString* error) {
    const QString ext = QFileInfo(path).suffix().toLower();
    if (!QFileInfo::exists(path)) {
        if (error) *error = tr("%1 does not exist").arg(path);
        return false;
    }
    if (ext == "acestudio") return openProject(path);
    if (ext == "png") {
        auto img = readPng(toPath(path));
        if (!img) {
            if (error) *error = qs(img.error().message);
            return false;
        }
        return openImageAsDocument(img.value(), QFileInfo(path).completeBaseName()) != nullptr;
    }
    if (error) *error = tr("AceStudio opens .acestudio projects and .png images here");
    return false;
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
    addView(new DocumentView(std::move(doc).value(), path, info, m_tools, m_tabs));
    m_ctx.addRecentProject(path);
    return true;
}

bool MainWindow::save(DocumentView* view) {
    if (!view) return false;
    if (!licenceAllows(this, tr("Saving"))) return false;
    if (view->filePath().isEmpty()) return saveAs(view);
    const QString err = view->saveTo(QString());
    if (!err.isEmpty()) {
        QMessageBox::critical(this, tr("Save"), tr("The project was not saved. Your previous file is unchanged.\n\n%1").arg(err));
        return false;
    }
    m_ctx.addRecentProject(view->filePath());
    discardRecovery(view);
    statusBar()->showMessage(tr("Saved %1").arg(view->displayName()), 3000);
    return true;
}

bool MainWindow::saveAs(DocumentView* view) {
    if (!view) return false;
    if (!licenceAllows(this, tr("Saving"))) return false;
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
    discardRecovery(view);
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
    discardRecovery(view);
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

void MainWindow::showLicence() {
    LicenceDialog dlg(this);
    connect(&dlg, &LicenceDialog::licenceChanged, this, &MainWindow::updateLicenceStatus);
    dlg.exec();
    updateLicenceStatus();
}

void MainWindow::updateLicenceStatus() {
    const LicenceStatus& s = currentLicence();
    QString text;
    switch (s.mode) {
    case LicenceMode::Licensed: text = tr("Licensed"); break;
    case LicenceMode::Trial: text = tr("Trial: %n day(s) left", nullptr, s.trialDaysLeft); break;
    case LicenceMode::TrialEnded: text = tr("Trial ended: enter a key"); break;
    }
    m_statusLicence->setText(text);
    m_statusLicence->setToolTip(qs(describeLicence(s)));
}

void MainWindow::showAbout() {
    QMessageBox::about(this, tr("About AceStudio"),
        tr("<h3>AceStudio %1</h3>"
           "<p>AI-assisted game asset creation and 2D animation studio.</p>"
           "<p>%4</p>"
           "<p>Project format schema %2.</p>"
           "<p>Built with Qt %3, used under the GNU LGPL v3 and dynamically linked. "
           "See Help › Third-Party Notices.</p>")
            .arg(QString::fromLatin1(kVersionString)).arg(kProjectSchemaVersion).arg(QString::fromLatin1(qVersion()))
            .arg(qs(describeLicence(currentLicence())).toHtmlEscaped()));
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
        "<li><b>libwebp 1.6.0</b> — BSD 3-Clause License, Copyright (c) 2010 Google Inc.; "
        "with the WebM project's additional patent grant</li>"
        "<li><b>Lua 5.4.9</b> — MIT License, Copyright (C) 1994-2026 Lua.org, PUC-Rio</li>"
        "<li><b>sol2 3.3.0</b> — MIT License, Copyright (c) 2013-2022 Rapptz, ThePhD, and contributors</li>"
        "<li><b>Monocypher 4.0.3</b> — BSD 2-Clause or CC0 (licence key signatures)</li>"
        "<li><b>Python 3.12</b> (bundled, for the optional AI features) — PSF License; <b>pip</b> — MIT License</li>"
        "</ul><p>The full licence texts are in the <i>licences</i> folder next to AceStudio.exe.</p>")
                    .arg(QString::fromLatin1(qVersion())));
    box.exec();
}

bool MainWindow::report(const Status& st) {
    if (!st) statusBar()->showMessage(qs(st.error().message), 4000);
    return st.ok();
}

void MainWindow::report(const Error& e) { statusBar()->showMessage(qs(e.message), 4000); }

void MainWindow::importPng(bool asLayer) {
    const QString path = QFileDialog::getOpenFileName(this, asLayer ? tr("Import PNG as Layer") : tr("Import PNG"),
                                                      QDir::homePath(), tr("PNG Images (*.png)"));
    if (path.isEmpty()) return;
    auto img = readPng(toPath(path));
    if (!img) {
        QMessageBox::warning(this, tr("Import PNG"), tr("Could not read %1.\n\n%2").arg(QDir::toNativeSeparators(path), qs(img.error().message)));
        return;
    }
    const QString name = QFileInfo(path).completeBaseName();
    if (asLayer) {
        if (DocumentView* v = currentView()) insertImageAsLayer(v, img.value(), name);
    } else {
        openImageAsDocument(img.value(), name);
    }
}

DocumentView* MainWindow::openAnimationAsDocument(const std::vector<Image>& frames, const std::vector<int>& durations,
                                                 const QString& name) {
    if (frames.empty()) return nullptr;
    auto doc = Document::create(frames[0].width(), frames[0].height());
    if (!doc) { report(doc.error()); return nullptr; }
    Document& d = doc.value();
    const LayerId l = d.addLayer(name.toStdString()).value();
    for (std::size_t f = 0; f < frames.size(); ++f) {
        if (f > 0)
            if (auto r = d.insertFrame(static_cast<int>(f)); !r) { report(r.error()); return nullptr; }
        if (f < durations.size()) (void)d.setFrameDuration(static_cast<int>(f), durations[f]);
        if (!frames[f].isNull()) (void)d.setCel(l, static_cast<int>(f), Cel{frames[f], 0, 0});
    }
    auto* view = new DocumentView(std::move(doc).value(), QString(), ProjectInfo{}, m_tools, m_tabs);
    addView(view);
    view->markNeverSaved();
    return view;
}

DocumentView* MainWindow::openImageAsDocument(const Image& img, const QString& name) {
    auto doc = Document::create(img.width(), img.height());
    if (!doc) { report(doc.error()); return nullptr; }
    const LayerId l = doc.value().addLayer(name.toStdString()).value();
    (void)doc.value().setCel(l, 0, Cel{img, 0, 0});
    auto* view = new DocumentView(std::move(doc).value(), QString(), ProjectInfo{}, m_tools, m_tabs);
    addView(view);
    view->markNeverSaved();
    return view;
}

bool MainWindow::insertImageAsLayer(DocumentView* v, const Image& src, const QString& name, std::optional<LayerId> above, int frame) {
    // Placed at the top-left as one undo step. A larger image can grow the
    // canvas (also part of that step) or keep its full pixels past the edges.
    Editor& ed = v->editor();
    const int f = frame >= 0 ? frame : v->currentFrame();
    const bool larger = src.width() > v->document().width() || src.height() > v->document().height();
    bool expand = false;
    if (larger) {
        QMessageBox box(QMessageBox::Question, tr("Insert Image"),
                        tr("The image (%1×%2) is larger than the canvas (%3×%4).")
                            .arg(src.width()).arg(src.height()).arg(v->document().width()).arg(v->document().height()),
                        QMessageBox::NoButton, this);
        box.setInformativeText(tr("Expand the canvas to fit it, or keep the canvas size? Pixels outside the canvas are kept "
                                  "on the layer and reappear if the canvas grows later."));
        QPushButton* grow = box.addButton(tr("Expand Canvas"), QMessageBox::AcceptRole);
        box.addButton(tr("Keep Canvas Size"), QMessageBox::RejectRole);
        QPushButton* cancel = box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(grow);
        box.exec();
        if (box.clickedButton() == cancel) return false;
        expand = box.clickedButton() == grow;
    }
    const std::size_t mark = ed.history().mark();
    if (expand && !report(ed.resizeCanvas(std::max(src.width(), v->document().width()),
                                          std::max(src.height(), v->document().height()), 0, 0)))
        return false;
    auto layerId = ed.addImageLayer(name.toStdString(), src, above.value_or(v->currentLayer()), f);
    if (!layerId) {
        if (expand) (void)ed.undo();
        report(layerId.error());
        return false;
    }
    if (expand) (void)ed.history().squash(mark, "Insert " + name.toStdString());
    v->setCurrentLayer(layerId.value());
    updateActions();   // the undo label changes when steps are grouped
    return true;
}

void MainWindow::exportAnimation(int kind) {
    DocumentView* v = currentView();
    if (!v) return;
    if (!licenceAllows(this, tr("Exporting"))) return;
    const auto k = static_cast<ExportKind>(kind);
    nlohmann::json presets = m_ctx.settings().get("export.presets");
    if (!presets.is_object()) presets = nlohmann::json::object();
    const std::string key = ExportDialog::kindKey(k).toStdString();
    const nlohmann::json preset = presets.contains(key) ? presets[key] : nlohmann::json::object();
    QString suggested = v->filePath();
    if (suggested.isEmpty()) {
        // Unsaved documents export next to the last export of this kind.
        QString folder = QDir::homePath();
        if (preset.contains("folder") && preset["folder"].is_string()) {
            const QString f = QString::fromStdString(preset["folder"].get<std::string>());
            if (QDir(f).exists()) folder = f;
        }
        suggested = QDir(folder).filePath(QStringLiteral("untitled.acestudio"));
    }
    ExportDialog dlg(k, v->document(), v->currentFrame(), suggested, preset, this);
    if (dlg.exec() != QDialog::Accepted) return;
    statusBar()->showMessage(dlg.resultMessage(), 6000);
    presets[key] = dlg.preset();
    (void)m_ctx.settings().set("export.presets", presets);
    m_ctx.saveSettings();
}

void MainWindow::editTags() {
    DocumentView* v = currentView();
    if (!v) return;
    TagsDialog dlg(v, this);
    dlg.exec();
}

OnionSkin MainWindow::onionSkinFromSettings() const {
    const Settings& s = m_ctx.settings();
    OnionSkin o;
    o.before = std::clamp(s.get<int>("animation.onionSkin.before"), 0, 8);
    o.after = std::clamp(s.get<int>("animation.onionSkin.after"), 0, 8);
    o.opacity = std::clamp(static_cast<int>(std::lround(s.get<double>("animation.onionSkin.opacity") * 100)), 5, 100);
    o.tint = s.get<bool>("animation.onionSkin.tint");
    o.wrap = s.get<bool>("animation.onionSkin.wrap");
    return o;
}

void MainWindow::applyOnionSkin() {
    const bool on = m_onionAction && m_onionAction->isChecked();
    const OnionSkin o = onionSkinFromSettings();
    for (int i = 0; i < m_tabs->count(); ++i)
        if (auto* v = qobject_cast<DocumentView*>(m_tabs->widget(i)))
            v->setOnionSkin(on ? std::optional<OnionSkin>(o) : std::nullopt);
}

void MainWindow::editOnionSkin() {
    OnionSkinDialog dlg(onionSkinFromSettings(), this);
    if (dlg.exec() != QDialog::Accepted) return;
    const OnionSkin o = dlg.options();
    Settings& s = m_ctx.settings();
    (void)s.set("animation.onionSkin.before", o.before);
    (void)s.set("animation.onionSkin.after", o.after);
    (void)s.set("animation.onionSkin.opacity", o.opacity / 100.0);
    (void)s.set("animation.onionSkin.tint", o.tint);
    (void)s.set("animation.onionSkin.wrap", o.wrap);
    if (!m_onionAction->isChecked()) m_onionAction->setChecked(true);   // changing settings implies wanting to see them
    else applyOnionSkin();
}

void MainWindow::exportPng() {
    DocumentView* v = currentView();
    if (!v) return;
    QString base = v->filePath().isEmpty() ? QDir(QDir::homePath()).filePath(QStringLiteral("untitled"))
                                           : v->filePath().left(v->filePath().lastIndexOf(QLatin1Char('.')));
    const QString suggested = v->document().frameCount() > 1 ? QStringLiteral("%1_%2.png").arg(base).arg(v->currentFrame() + 1)
                                                            : base + QStringLiteral(".png");
    if (!licenceAllows(this, tr("Exporting"))) return;
    const QString path = QFileDialog::getSaveFileName(this, tr("Export Frame as PNG"), suggested, tr("PNG Images (*.png)"));
    if (path.isEmpty()) return;
    if (auto st = writePng(v->document().composite(v->currentFrame()), toPath(path)); !st) {
        QMessageBox::critical(this, tr("Export PNG"), qs(st.error().message));
        return;
    }
    statusBar()->showMessage(tr("Exported %1").arg(QDir::toNativeSeparators(path)), 4000);
}

void MainWindow::applyAutosaveInterval() {
    const int sec = m_ctx.settings().get<int>("general.autosaveIntervalSec");
    if (sec <= 0) {
        m_autosaveTimer->stop();
        ACE_LOG_INFO("autosave", "Autosave disabled");
        return;
    }
    m_autosaveTimer->start(std::max(5, sec) * 1000);
}

void MainWindow::autosaveTick() {
    for (int i = 0; i < m_tabs->count(); ++i) {
        auto* v = static_cast<DocumentView*>(m_tabs->widget(i));
        if (!v->needsAutosave() || v->isBusy()) continue;
        const auto st = m_recovery->write(v->recoveryId(), v->document(), v->filePath().toStdString(),
                                          v->displayName().toStdString());
        if (st) {
            v->markAutosaved();
            ACE_LOG_DEBUG("autosave", "Recovery copy written for " + v->displayName().toStdString());
        } else {
            ACE_LOG_ERROR("autosave", "Recovery copy failed: " + st.error().message);
            if (!m_autosaveErrorShown) {
                m_autosaveErrorShown = true;
                statusBar()->showMessage(tr("Autosave failed: %1").arg(qs(st.error().message)), 8000);
            }
        }
    }
}

void MainWindow::discardRecovery(DocumentView* view) {
    if (!view || !m_recovery) return;
    (void)m_recovery->remove(view->recoveryId());
}

void MainWindow::offerRecovery() {
    std::vector<std::string> problems;
    const auto entries = m_recovery->orphaned(&problems);
    for (const auto& p : problems) ACE_LOG_WARN("recovery", p);
    if (entries.empty()) return;

    QString list;
    for (const auto& e : entries) {
        list += QStringLiteral("• %1  (%2)\n").arg(qs(e.title), qs(e.savedUtc).left(19).replace(QLatin1Char('T'), QLatin1Char(' ')));
    }
    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(tr("Recover Unsaved Work"));
    box.setText(tr("AceStudio did not close normally. Unsaved work was found for:"));
    box.setInformativeText(list + tr("\nTimes are UTC. Recovered documents open as unsaved changes; save them to keep them."));
    QPushButton* recover = box.addButton(tr("Recover"), QMessageBox::AcceptRole);
    QPushButton* discard = box.addButton(tr("Discard"), QMessageBox::DestructiveRole);
    box.addButton(tr("Decide Later"), QMessageBox::RejectRole);
    box.setDefaultButton(recover);
    box.exec();

    if (box.clickedButton() == discard) {
        if (QMessageBox::question(this, tr("Discard Recovered Work"),
                                  tr("Permanently delete the recovered copies? This cannot be undone.")) != QMessageBox::Yes)
            return;
        for (const auto& e : entries) (void)m_recovery->remove(e.id);
        ACE_LOG_INFO("recovery", "User discarded " + std::to_string(entries.size()) + " recovery file(s)");
        return;
    }
    if (box.clickedButton() != recover) return;

    for (const auto& e : entries) {
        auto doc = loadProject(e.projectFile);
        if (!doc) {
            QMessageBox::warning(this, tr("Recover"), tr("Could not recover %1:\n%2").arg(qs(e.title), qs(doc.error().message)));
            continue;   // keep the files so nothing is lost
        }
        auto* view = new DocumentView(std::move(doc).value(), qs(e.originalPath), ProjectInfo{}, m_tools, m_tabs);
        view->setRecoveryId(e.id);   // keep protecting it under the same id
        addView(view);
        view->markNeverSaved();
        ACE_LOG_INFO("recovery", "Recovered " + e.title);
    }
    autosaveTick();   // re-stamp recovered files with this process id
}

void MainWindow::transformCurrentLayer(const QString& label, bool allFrames, const Editor::ImageFn& fn) {
    auto* v = currentView();
    if (!v) return;
    report(v->editor().transformLayer(v->currentLayer(), v->currentFrame(), allFrames, v->selection(), label.toStdString(), fn));
}

void MainWindow::editShortcuts() {
    ShortcutsDialog dlg(m_shortcuts, m_ctx.settings().get("keyboard.shortcuts"), this);
    if (dlg.exec() != QDialog::Accepted) return;
    (void)m_ctx.settings().set("keyboard.shortcuts", dlg.overrides());
    m_shortcuts.apply(dlg.overrides());
    m_ctx.saveSettings();
    statusBar()->showMessage(tr("Keyboard shortcuts updated"), 3000);
}

void MainWindow::closeEvent(QCloseEvent* e) {
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (!maybeSave(static_cast<DocumentView*>(m_tabs->widget(i)))) {
            e->ignore();
            return;
        }
    }
    for (int i = 0; i < m_tabs->count(); ++i) discardRecovery(static_cast<DocumentView*>(m_tabs->widget(i)));
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
