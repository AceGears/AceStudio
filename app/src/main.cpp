#include "AppContext.h"
#include "MainWindow.h"
#include "QtBridge.h"
#include "SingleInstance.h"
#include "Theme.h"

#include "acestudio/core/Log.h"
#include "acestudio/core/Version.h"

#include <QApplication>
#include <QMessageBox>
#include <QTimer>

int main(int argc, char* argv[]) {
    using namespace acestudio;
    using namespace acestudio::ui;

    QApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("AceStudio"));
    QApplication::setOrganizationName(QStringLiteral("AceStudio"));
    QApplication::setApplicationVersion(QString::fromLatin1(kVersionString));

    AppContext ctx;
    const QString startupWarning = ctx.initialize();

    // Files passed on the command line (e.g. via Explorer "Open with").
    const QStringList args = QApplication::arguments();
    QStringList files;
    bool newInstance = false;
    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == QLatin1String("--new-instance")) newInstance = true;
        else if (!args[i].startsWith(QLatin1Char('-'))) files << args[i];
    }
    SingleInstance single(ctx.dataDir());
    if (!newInstance) {
        if (single.forwardToRunning(files)) {
            ACE_LOG_INFO("ui", "AceStudio is already running; handed over " + std::to_string(files.size()) + " file(s)");
            Logger::instance().flush();
            return 0;
        }
        single.listen();
    }

    const auto accent = Rgba8::fromHex(ctx.settings().get<std::string>("general.accentColor"));
    applyTheme(app, themeFromString(qs(ctx.settings().get<std::string>("general.theme"))),
               accent ? toQColor(*accent) : QColor());

    MainWindow window(ctx);
    window.show();
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
