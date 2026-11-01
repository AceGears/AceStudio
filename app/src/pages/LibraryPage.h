#pragma once
// Asset Library workspace: projects and images from the folders you add (and
// your recent projects), with thumbnails, search, kind filters, sorting, tags
// and favourites. Scanning and thumbnails run on a background thread; tags
// and favourites are stored in settings (library.*), never in your files.

#include <QWidget>

#include <QDateTime>
#include <QImage>

#include <memory>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;

namespace acestudio::ui {

class AppContext;

struct LibraryEntry {
    QString path;
    QString name;
    enum class Kind { Project, Image, Animation } kind = Kind::Image;
    QDateTime modified;
    qint64 bytes = 0;
    int frames = 1;
    QImage thumb;
};

class LibraryPage : public QWidget {
    Q_OBJECT
public:
    explicit LibraryPage(AppContext& ctx, QWidget* parent = nullptr);
    ~LibraryPage() override;
    void rescan();

Q_SIGNALS:
    void openRequested(const QString& path);

protected:
    void showEvent(QShowEvent* e) override;

private:
    void scanFinished(std::vector<LibraryEntry> entries);
    void refill();
    void editTags(QListWidgetItem* it);
    void toggleFavorite(QListWidgetItem* it);
    void addFolder();
    void removeFolder();
    QStringList tagsOf(const QString& path) const;
    bool isFavorite(const QString& path) const;

    AppContext& m_ctx;
    QLineEdit* m_search = nullptr;
    QComboBox* m_kind = nullptr;
    QComboBox* m_sort = nullptr;
    QComboBox* m_folders = nullptr;
    QListWidget* m_list = nullptr;
    QLabel* m_status = nullptr;
    std::vector<LibraryEntry> m_entries;
    std::shared_ptr<bool> m_alive;
    bool m_scanned = false;
    int m_generation = 0;
};

} // namespace acestudio::ui
