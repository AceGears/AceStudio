#include "AppContext.h"
#include "MainWindow.h"
#include "QtBridge.h"
#include "SingleInstance.h"
#include "Accessibility.h"
#include "Theme.h"
#include "selftest/UiAudit.h"
#include "ui/Motion.h"

#include "acestudio/core/Log.h"
#include "acestudio/core/Version.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QLinearGradient>
#include <QPainter>
#include <QSplashScreen>
#include <QMessageBox>
#include <QTimer>

#include <memory>

namespace {
// The splash: the AceStudio logo on the charcoal brand background.
QPixmap splashPixmap(qreal dpr) {
    const QSize logical(520, 400);
    QPixmap pm(logical * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    QLinearGradient bg(0, 0, 0, logical.height());
    bg.setColorAt(0, QColor("#161A21"));
    bg.setColorAt(1, QColor("#0B0D10"));
    p.setPen(QColor("#262C36"));
    p.setBrush(bg);
    p.drawRoundedRect(QRectF(0.5, 0.5, logical.width() - 1, logical.height() - 1), 16, 16);
    const QPixmap logo(QStringLiteral(":/branding/logo-512.png"));
    const int side = 300;
    p.drawPixmap(QRect((logical.width() - side) / 2, 24, side, side), logo);
    p.setPen(QColor("#8C95A7"));
    QFont f = p.font();
    f.setPointSizeF(9);
    p.setFont(f);
    p.drawText(QRect(0, logical.height() - 58, logical.width(), 20), Qt::AlignCenter,
               QStringLiteral("Version %1").arg(QString::fromLatin1(acestudio::kVersionString)));
    return pm;
}
} // namespace

int main(int argc, char* argv[]) {
    using namespace acestudio;
    using namespace acestudio::ui;

    QApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    // --ui-audit report.json [project]: the UI and accessibility check (selftest/UiAudit.h).
    QString auditReport, auditProject;
    for (int i = 1; i < argc; ++i)
        if (QString::fromLocal8Bit(argv[i]) == QLatin1String("--ui-audit") && i + 1 < argc) {
            auditReport = QString::fromLocal8Bit(argv[i + 1]);
            if (i + 2 < argc) auditProject = QString::fromLocal8Bit(argv[i + 2]);
        }
    if (!auditReport.isEmpty()) QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);   // so the audit can close them
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("AceStudio"));
    QApplication::setOrganizationName(QStringLiteral("AceStudio"));
    QApplication::setApplicationVersion(QString::fromLatin1(kVersionString));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/branding/logo-256.png")));

    installAccessibilityHelper(app);
    AppContext ctx;
    const QString startupWarning = ctx.initialize();

    // Files passed on the command line (e.g. via Explorer "Open with").
    const QStringList args = QApplication::arguments();
    QStringList files;
    bool newInstance = false;
    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == QLatin1String("--new-instance")) newInstance = true;
        else if (args[i] == QLatin1String("--ui-audit")) i += 2;   // its report and project are not files to open
        else if (!args[i].startsWith(QLatin1Char('-'))) files << QFileInfo(args[i]).absoluteFilePath();
    }
    // Started from the Start menu, the working folder is the install folder,
    // and file dialogs given no folder opened there (seen on the test PC).
    // The home folder is the better start; command-line files are resolved
    // above, before this.
    if (QDir(QDir::currentPath()) == QDir(QCoreApplication::applicationDirPath())) QDir::setCurrent(QDir::homePath());
    SingleInstance single(ctx.dataDir());
    if (!newInstance && auditReport.isEmpty()) {
        if (single.forwardToRunning(files)) {
            ACE_LOG_INFO("ui", "AceStudio is already running; handed over " + std::to_string(files.size()) + " file(s)");
            Logger::instance().flush();
            return 0;
        }
        single.listen();
    }

    const auto accent = Rgba8::fromHex(ctx.settings().get<std::string>("general.accentColor"));
    // A Windows contrast theme wins over the saved theme.
    const ThemeKind theme = systemHighContrast() ? ThemeKind::HighContrast : themeFromString(qs(ctx.settings().get<std::string>("general.theme")));
    applyTheme(app, theme, accent ? toQColor(*accent) : QColor());
    applyTextScale(app, ctx.settings().get<int>("general.textScale"));
    motion::configure(ctx.settings().get<bool>("ui.reducedMotion"), ctx.settings().get<int>("ui.animationSpeed"));

    std::unique_ptr<QSplashScreen> splash;
    if (auditReport.isEmpty() && ctx.settings().get<bool>("ui.showSplash")) {
        splash = std::make_unique<QSplashScreen>(splashPixmap(app.devicePixelRatio()));
        splash->setAttribute(Qt::WA_TranslucentBackground);
        splash->show();
        app.processEvents();
    }

    MainWindow window(ctx);
    window.show();
    if (splash) splash->finish(&window);
    // Settings › Appearance › Open at start: the last project.
    if (files.isEmpty() && auditReport.isEmpty() && ctx.settings().get<std::string>("general.startup") == "lastProject" &&
        !ctx.recentProjects().isEmpty() && QFileInfo::exists(ctx.recentProjects().first()))
        files << ctx.recentProjects().first();
    if (!auditReport.isEmpty()) {
        const int rc = runUiAudit(window, auditReport, auditProject);
        Logger::instance().flush();
        return rc;
    }
    for (const QString& f : files) window.openProject(f);
    QObject::connect(&single, &SingleInstance::openRequested, &window, [&window](const QStringList& more) {
        if (window.isMinimized()) window.showNormal();
        window.raise();
        window.activateWindow();
        for (const QString& f : more) window.openProject(f);
    });

    QTimer::singleShot(0, &window, [&window] { window.offerRecovery(); });

    if (!startupWarning.isEmpty())
        QTimer::singleShot(0, &window, [&window, startupWarning] {
            QMessageBox::warning(&window, QObject::tr("AceStudio"), startupWarning);
        });

    const int rc = QApplication::exec();
    Logger::instance().flush();
    return rc;
}
