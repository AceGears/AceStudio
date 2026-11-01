#include "AppContext.h"
#include "MainWindow.h"
#include "QtBridge.h"
#include "SingleInstance.h"
#include "Accessibility.h"
#include "Theme.h"
#include "selftest/UiAudit.h"

#include "acestudio/core/Log.h"
#include "acestudio/core/Version.h"

#include <QApplication>
#include <QMessageBox>
#include <QTimer>

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
        else if (!args[i].startsWith(QLatin1Char('-'))) files << args[i];
    }
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

    MainWindow window(ctx);
    window.show();
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
