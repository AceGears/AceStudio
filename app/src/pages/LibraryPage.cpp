#include "LibraryPage.h"

#include "../AppContext.h"
#include "../QtBridge.h"
#include "../ui/Icons.h"

#include "acestudio/core/AnimCodec.h"
#include "acestudio/core/FileUtil.h"
#include "acestudio/core/PngCodec.h"
#include "acestudio/core/ProjectIO.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <set>
#include <thread>

namespace acestudio::ui {

namespace {

constexpr int kThumb = 112;
constexpr int kMaxFiles = 3000;   // a library folder pointed at a whole drive stays bounded

QImage fit(QImage q) {
    if (q.isNull()) return q;
    const int scale = std::max(1, kThumb / std::max(q.width(), q.height()));
    if (scale > 1) q = q.scaled(q.width() * scale, q.height() * scale, Qt::KeepAspectRatio, Qt::FastTransformation);
    if (q.width() > kThumb || q.height() > kThumb) q = q.scaled(kThumb, kThumb, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return q;
}

// Reads one file into an entry with its thumbnail. Off the UI thread.
std::optional<LibraryEntry> describe(const QFileInfo& fi) {
    LibraryEntry e;
    e.path = fi.absoluteFilePath();
    e.name = fi.completeBaseName();
    e.modified = fi.lastModified();
    e.bytes = fi.size();
    const QString ext = fi.suffix().toLower();
    if (ext == QLatin1String("acestudio")) {
        e.kind = LibraryEntry::Kind::Project;
        auto doc = loadProject(toPath(e.path));
        if (!doc) return std::nullopt;
        e.frames = doc.value().frameCount();
        e.thumb = fit(toQImage(doc.value().composite(0)));
        return e;
    }
    if (fi.size() > (256 << 20)) return std::nullopt;
    auto bytes = readFileBytes(toPath(e.path), 256u << 20);
    if (!bytes) return std::nullopt;
    if (ext == QLatin1String("gif")) {
        auto a = decodeGif(bytes.value());
        if (!a || a.value().frames.empty()) return std::nullopt;
        e.frames = static_cast<int>(a.value().frames.size());
        e.kind = e.frames > 1 ? LibraryEntry::Kind::Animation : LibraryEntry::Kind::Image;
        e.thumb = fit(toQImage(a.value().frames.front().image));
        return e;
    }
    if (ext == QLatin1String("webp")) {
        auto a = decodeWebp(bytes.value());
        if (!a || a.value().frames.empty()) return std::nullopt;
        e.frames = static_cast<int>(a.value().frames.size());
        e.kind = e.frames > 1 ? LibraryEntry::Kind::Animation : LibraryEntry::Kind::Image;
        e.thumb = fit(toQImage(a.value().frames.front().image));
        return e;
    }
    // PNG (APNG is read as its first frame by the PNG decoder; frame count from the APNG decoder).
    auto img = decodePng(bytes.value());
    if (!img) return std::nullopt;
    if (auto a = decodeApng(bytes.value()); a && a.value().frames.size() > 1) {
        e.frames = static_cast<int>(a.value().frames.size());
        e.kind = LibraryEntry::Kind::Animation;
    }
    e.thumb = fit(toQImage(img.value()));
    return e;
}

QString kindText(const LibraryEntry& e) {
    switch (e.kind) {
    case LibraryEntry::Kind::Project: return e.frames > 1 ? QObject::tr("Project · %1 frames").arg(e.frames) : QObject::tr("Project");
    case LibraryEntry::Kind::Animation: return QObject::tr("Animation · %1 frames").arg(e.frames);
    case LibraryEntry::Kind::Image: return QObject::tr("Image");
    }
    return {};
}

} // namespace

LibraryPage::LibraryPage(AppContext& ctx, QWidget* parent) : QWidget(parent), m_ctx(ctx), m_alive(std::make_shared<bool>(true)) {
    auto* col = new QVBoxLayout(this);
    col->setContentsMargins(28, 22, 28, 16);
    col->setSpacing(10);
    auto* h = new QLabel(tr("Asset Library"), this);
    h->setProperty("role", "h1");
    col->addWidget(h);
    auto* sub = new QLabel(tr("Projects, sprites and animations from your library folders and recent projects. Tags and favourites "
                              "are kept by AceStudio; your files are not changed."),
                           this);
    sub->setProperty("role", "muted");
    sub->setWordWrap(true);
    col->addWidget(sub);

    auto* bar = new QHBoxLayout;
    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("Search names and tags"));
    m_search->setAccessibleName(tr("Search the library"));
    m_search->setClearButtonEnabled(true);
    m_kind = new QComboBox(this);
    m_kind->setAccessibleName(tr("Show"));
    m_kind->addItems({tr("Everything"), tr("Projects"), tr("Images"), tr("Animations"), tr("Favourites")});
    m_sort = new QComboBox(this);
    m_sort->setAccessibleName(tr("Sort by"));
    m_sort->addItems({tr("Newest first"), tr("Name"), tr("Largest first")});
    m_folders = new QComboBox(this);
    m_folders->setAccessibleName(tr("Library folders"));
    // A long folder path must not widen the page (and the window).
    m_folders->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_folders->setMinimumContentsLength(18);
    m_folders->setMinimumWidth(200);
    auto* add = new QPushButton(icon(Glyph::Plus), tr("Add Folder…"), this);
    auto* remove = new QPushButton(tr("Remove Folder"), this);
    auto* refresh = new QPushButton(icon(Glyph::Refresh), tr("Rescan"), this);
    bar->addWidget(m_search, 2);
    bar->addWidget(m_kind);
    bar->addWidget(m_sort);
    col->addLayout(bar);
    // Folders on their own row, so the page fits a 1280 px screen with Windows fonts.
    auto* folderBar = new QHBoxLayout;
    folderBar->addWidget(m_folders, 1);
    folderBar->addWidget(add);
    folderBar->addWidget(remove);
    folderBar->addWidget(refresh);
    col->addLayout(folderBar);

    m_list = new QListWidget(this);
    m_list->setAccessibleName(tr("Assets"));
    m_list->setViewMode(QListView::IconMode);
    m_list->setIconSize(QSize(kThumb, kThumb));
    m_list->setGridSize(QSize(kThumb + 40, kThumb + 54));
    m_list->setResizeMode(QListView::Adjust);
    m_list->setMovement(QListView::Static);
    m_list->setUniformItemSizes(true);
    m_list->setWordWrap(true);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    col->addWidget(m_list, 1);
    m_status = new QLabel(this);
    m_status->setProperty("role", "muted");
    col->addWidget(m_status);

    connect(m_search, &QLineEdit::textChanged, this, &LibraryPage::refill);
    connect(m_kind, &QComboBox::currentIndexChanged, this, &LibraryPage::refill);
    connect(m_sort, &QComboBox::currentIndexChanged, this, &LibraryPage::refill);
    connect(add, &QPushButton::clicked, this, &LibraryPage::addFolder);
    connect(remove, &QPushButton::clicked, this, &LibraryPage::removeFolder);
    connect(refresh, &QPushButton::clicked, this, &LibraryPage::rescan);
    connect(m_list, &QListWidget::itemActivated, this, [this](QListWidgetItem* it) { Q_EMIT openRequested(it->data(Qt::UserRole).toString()); });
    connect(m_list, &QListWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QListWidgetItem* it = m_list->itemAt(pos);
        if (!it) return;
        QMenu menu(this);
        menu.addAction(tr("Open"), this, [this, it] { Q_EMIT openRequested(it->data(Qt::UserRole).toString()); });
        menu.addAction(isFavorite(it->data(Qt::UserRole).toString()) ? tr("Remove from Favourites") : tr("Add to Favourites"), this,
                       [this, it] { toggleFavorite(it); });
        menu.addAction(tr("Edit Tags…"), this, [this, it] { editTags(it); });
        menu.exec(m_list->viewport()->mapToGlobal(pos));
    });
}

LibraryPage::~LibraryPage() { *m_alive = false; }

void LibraryPage::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    if (!m_scanned) rescan();
}

QStringList LibraryPage::tagsOf(const QString& path) const {
    const auto tags = m_ctx.settings().get("library.tags");
    QStringList out;
    if (tags.is_object() && tags.contains(path.toStdString()) && tags[path.toStdString()].is_array())
        for (const auto& t : tags[path.toStdString()])
            if (t.is_string()) out << QString::fromStdString(t.get<std::string>());
    return out;
}

bool LibraryPage::isFavorite(const QString& path) const {
    const auto fav = m_ctx.settings().get("library.favorites");
    return fav.is_array() && std::find(fav.begin(), fav.end(), path.toStdString()) != fav.end();
}

void LibraryPage::rescan() {
    m_scanned = true;
    const int gen = ++m_generation;
    QStringList folders;
    for (const auto& f : m_ctx.settings().get("library.folders"))
        if (f.is_string()) folders << QString::fromStdString(f.get<std::string>());
    m_folders->clear();
    for (const QString& f : folders) m_folders->addItem(QDir::toNativeSeparators(f), f);
    if (folders.isEmpty()) m_folders->addItem(tr("(no folders yet)"));
    const QStringList recent = m_ctx.recentProjects();
    m_status->setText(tr("Scanning…"));
    QPointer<LibraryPage> self(this);
    std::weak_ptr<bool> alive = m_alive;
    std::thread([self, alive, folders, recent, gen] {
        std::vector<QFileInfo> files;
        std::set<QString> seen;
        auto consider = [&](const QFileInfo& fi) {
            if (files.size() >= static_cast<std::size_t>(kMaxFiles)) return;
            const QString p = fi.absoluteFilePath();
            if (fi.isFile() && seen.insert(p).second) files.push_back(fi);
        };
        for (const QString& r : recent) consider(QFileInfo(r));
        for (const QString& f : folders) {
            QDirIterator it(f, {QStringLiteral("*.acestudio"), QStringLiteral("*.png"), QStringLiteral("*.gif"), QStringLiteral("*.webp")},
                            QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext() && files.size() < static_cast<std::size_t>(kMaxFiles)) {
                if (alive.expired()) return;
                consider(QFileInfo(it.next()));
            }
        }
        std::vector<LibraryEntry> entries;
        for (const QFileInfo& fi : files) {
            if (alive.expired()) return;
            if (auto e = describe(fi)) entries.push_back(std::move(*e));
        }
        if (alive.expired()) return;
        QMetaObject::invokeMethod(qApp, [self, entries = std::move(entries), gen]() mutable {
            if (self && gen == self->m_generation) self->scanFinished(std::move(entries));
        });
    }).detach();
}

void LibraryPage::scanFinished(std::vector<LibraryEntry> entries) {
    m_entries = std::move(entries);
    refill();
}

void LibraryPage::refill() {
    m_list->clear();
    std::vector<const LibraryEntry*> shown;
    const QString q = m_search->text().trimmed();
    for (const LibraryEntry& e : m_entries) {
        const int k = m_kind->currentIndex();
        if (k == 1 && e.kind != LibraryEntry::Kind::Project) continue;
        if (k == 2 && e.kind != LibraryEntry::Kind::Image) continue;
        if (k == 3 && !(e.kind == LibraryEntry::Kind::Animation || e.frames > 1)) continue;
        if (k == 4 && !isFavorite(e.path)) continue;
        if (!q.isEmpty() && !e.name.contains(q, Qt::CaseInsensitive) &&
            !tagsOf(e.path).join(QLatin1Char(' ')).contains(q, Qt::CaseInsensitive))
            continue;
        shown.push_back(&e);
    }
    const int sort = m_sort->currentIndex();
    std::stable_sort(shown.begin(), shown.end(), [sort](const LibraryEntry* a, const LibraryEntry* b) {
        if (sort == 1) return a->name.compare(b->name, Qt::CaseInsensitive) < 0;
        if (sort == 2) return a->bytes > b->bytes;
        return a->modified > b->modified;
    });
    for (const LibraryEntry* e : shown) {
        auto* it = new QListWidgetItem(QIcon(QPixmap::fromImage(e->thumb)), (isFavorite(e->path) ? QStringLiteral("★ ") : QString()) + e->name);
        it->setData(Qt::UserRole, e->path);
        const QStringList tags = tagsOf(e->path);
        it->setToolTip(QStringLiteral("%1\n%2%3").arg(QDir::toNativeSeparators(e->path), kindText(*e),
                                                     tags.isEmpty() ? QString() : QStringLiteral("\n#") + tags.join(QStringLiteral(" #"))));
        m_list->addItem(it);
    }
    m_status->setText(tr("%1 of %2 assets").arg(shown.size()).arg(m_entries.size()));
}

void LibraryPage::editTags(QListWidgetItem* it) {
    const QString path = it->data(Qt::UserRole).toString();
    bool ok = false;
    const QString text = QInputDialog::getText(this, tr("Tags"), tr("Tags, separated by spaces:"), QLineEdit::Normal,
                                               tagsOf(path).join(QLatin1Char(' ')), &ok);
    if (!ok) return;
    nlohmann::json tags = m_ctx.settings().get("library.tags");
    if (!tags.is_object()) tags = nlohmann::json::object();
    nlohmann::json list = nlohmann::json::array();
    for (const QString& t : text.split(QLatin1Char(' '), Qt::SkipEmptyParts)) list.push_back(t.toStdString());
    if (list.empty()) tags.erase(path.toStdString());
    else tags[path.toStdString()] = list;
    m_ctx.setSetting("library.tags", tags);
    refill();
}

void LibraryPage::toggleFavorite(QListWidgetItem* it) {
    const std::string path = it->data(Qt::UserRole).toString().toStdString();
    nlohmann::json fav = m_ctx.settings().get("library.favorites");
    if (!fav.is_array()) fav = nlohmann::json::array();
    if (auto f = std::find(fav.begin(), fav.end(), path); f != fav.end()) fav.erase(f);
    else fav.push_back(path);
    m_ctx.setSetting("library.favorites", fav);
    refill();
}

void LibraryPage::addFolder() {
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Add Library Folder"));
    if (dir.isEmpty()) return;
    nlohmann::json folders = m_ctx.settings().get("library.folders");
    if (!folders.is_array()) folders = nlohmann::json::array();
    if (std::find(folders.begin(), folders.end(), dir.toStdString()) == folders.end()) folders.push_back(dir.toStdString());
    m_ctx.setSetting("library.folders", folders);
    rescan();
}

void LibraryPage::removeFolder() {
    const QString dir = m_folders->currentData().toString();
    if (dir.isEmpty()) return;
    nlohmann::json folders = m_ctx.settings().get("library.folders");
    if (folders.is_array())
        if (auto f = std::find(folders.begin(), folders.end(), dir.toStdString()); f != folders.end()) folders.erase(f);
    m_ctx.setSetting("library.folders", folders);
    rescan();
}

} // namespace acestudio::ui
