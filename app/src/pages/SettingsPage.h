#pragma once
// Settings workspace: categories on the left, a form on the right. Every
// control edits a real setting the application reads, and changes apply and
// save immediately (AppContext::setSetting).

#include <QWidget>

class QListWidget;
class QStackedWidget;

namespace acestudio::ui {

class AppContext;

class SettingsPage : public QWidget {
    Q_OBJECT
public:
    explicit SettingsPage(AppContext& ctx, QWidget* parent = nullptr);

Q_SIGNALS:
    void openLogsRequested();
    void openModelsRequested();
    void openAiSetupRequested();
    void openShortcutsRequested();
    void resetLayoutRequested();

private:
    QWidget* appearance();
    QWidget* editor();
    QWidget* generation();
    QWidget* animation();
    QWidget* exporting();
    QWidget* integrations();
    QWidget* advanced();
    void addCategory(const QString& name, QWidget* page);

    AppContext& m_ctx;
    QListWidget* m_categories = nullptr;
    QStackedWidget* m_pages = nullptr;
};

} // namespace acestudio::ui
