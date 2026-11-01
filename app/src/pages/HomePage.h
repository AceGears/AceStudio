#pragma once
// Home workspace: identity, quick generation, quick actions, templates and
// recent projects with real thumbnails (frame 1 of each project, rendered on
// a background thread so the dashboard never waits for disk).

#include <QWidget>

#include <map>
#include <memory>

class QComboBox;
class QGridLayout;
class QLabel;
class QLineEdit;

namespace acestudio::ui {

class AppContext;

struct ProjectTemplate {
    QString name;
    int width = 64, height = 64, frames = 1;
    QString description;
};

class HomePage : public QWidget {
    Q_OBJECT
public:
    explicit HomePage(AppContext& ctx, QWidget* parent = nullptr);
    ~HomePage() override;
    static std::vector<ProjectTemplate> templates();
    void refreshLicence(const QString& text);

Q_SIGNALS:
    void newProjectRequested();
    void openProjectRequested();
    void openRecentRequested(const QString& path);
    void importImageRequested();
    void templateRequested(const acestudio::ui::ProjectTemplate& t);
    void generateRequested(const QString& prompt, const QString& assetType);
    void workspaceRequested(const QString& id);
    void guideRequested();

protected:
    void showEvent(QShowEvent* e) override;

private:
    void rebuildRecent();
    void thumbnailReady(const QString& path, const QImage& img);

    AppContext& m_ctx;
    QLineEdit* m_prompt = nullptr;
    QComboBox* m_type = nullptr;
    QLabel* m_licence = nullptr;
    QGridLayout* m_recentGrid = nullptr;
    QWidget* m_recentBox = nullptr;
    std::map<QString, QLabel*> m_thumbLabels;
    std::map<QString, QImage> m_thumbCache;   // path -> thumbnail (cleared when a project changes on disk)
    std::shared_ptr<bool> m_alive;
};

} // namespace acestudio::ui
