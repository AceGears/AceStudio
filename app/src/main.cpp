#include "AppContext.h"
#include "MainWindow.h"
#include "QtBridge.h"
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

    const auto accent = Rgba8::fromHex(ctx.settings().get<std::string>("general.accentColor"));
    applyTheme(app, themeFromString(qs(ctx.settings().get<std::string>("general.theme"))),
               accent ? toQColor(*accent) : QColor());

    MainWindow window(ctx);
    window.show();

    // Files passed on the command line (e.g. via Explorer "Open with").
    const QStringList args = QApplication::arguments();
    for (int i = 1; i < args.size(); ++i)
        if (!args[i].startsWith(QLatin1Char('-'))) window.openProject(args[i]);

    QTimer::singleShot(0, &window, [&window] { window.offerRecovery(); });

    if (!startupWarning.isEmpty())
        QTimer::singleShot(0, &window, [&window, startupWarning] {
            QMessageBox::warning(&window, QObject::tr("AceStudio"), startupWarning);
        });

    const int rc = QApplication::exec();
    Logger::instance().flush();
    return rc;
}
