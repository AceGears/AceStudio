#include "HomePage.h"

#include "../AppContext.h"
#include "../QtBridge.h"
#include "../Theme.h"
#include "../ui/Icons.h"
#include "../ui/Motion.h"

#include "acestudio/core/ProjectIO.h"
#include "acestudio/core/Version.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

#include <functional>
#include <thread>

namespace acestudio::ui {

namespace {

QFrame* card(QWidget* parent) {
    auto* f = new QFrame(parent);
    f->setProperty("card", true);
    return f;
}

QLabel* label(const QString& text, const char* role, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setProperty("role", role);
    l->setWordWrap(true);
    return l;
}

// A large icon button used for quick actions and templates.
QToolButton* tile(Glyph g, const QString& title, const QString& sub, QWidget* parent) {
    auto* b = new QToolButton(parent);
    b->setIcon(icon(g));
    b->setIconSize(QSize(28, 28));
    b->setText(sub.isEmpty() ? title : title + QStringLiteral("\n") + sub);
    b->setAccessibleName(title);
    b->setAccessibleDescription(sub);
    b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    b->setMinimumSize(132, 92);
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    b->setCursor(Qt::PointingHandCursor);
    b->setProperty("tile", true);
    b->setStyleSheet(QStringLiteral("QToolButton { border: 1px solid palette(mid); border-radius: 12px; padding: 10px; }"
                                    "QToolButton:hover { border-color: palette(highlight); }"));
    return b;
}

// Renders frame 1 of a project, fitted into `box`. Runs off the UI thread.
QImage renderThumbnail(const QString& path, int box) {
    auto doc = loadProject(toPath(path));
    if (!doc || doc.value().frameCount() == 0) return {};
    const Image img = doc.value().composite(0);
    QImage q = toQImage(img);
    if (q.isNull()) return {};
    // Pixel art: integer nearest-neighbour upscale when small, smooth when large.
    const int scale = std::max(1, box / std::max(q.width(), q.height()));
    if (scale > 1) q = q.scaled(q.width() * scale, q.height() * scale, Qt::KeepAspectRatio, Qt::FastTransformation);
    if (q.width() > box || q.height() > box) q = q.scaled(box, box, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return q;
}

} // namespace

std::vector<ProjectTemplate> HomePage::templates() {
    return {{tr("Tiny sprite"), 16, 16, 4, tr("16×16 · 4 frames")},
            {tr("Character"), 64, 64, 8, tr("64×64 · 8 frames")},
            {tr("Item icon"), 32, 32, 1, tr("32×32")},
            {tr("Portrait"), 128, 128, 1, tr("128×128")},
            {tr("Tile sheet"), 256, 256, 1, tr("256×256, 16 px grid")},
            {tr("Background"), 480, 270, 1, tr("480×270")}};
}

HomePage::HomePage(AppContext& ctx, QWidget* parent) : QWidget(parent), m_ctx(ctx), m_alive(std::make_shared<bool>(true)) {
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget(scroll);
    scroll->setWidget(content);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(scroll);

    auto* col = new QVBoxLayout(content);
    col->setContentsMargins(40, 28, 40, 32);
    col->setSpacing(18);

    // Header: the logo, the product name and the licence state.
    auto* header = new QHBoxLayout;
    auto* logo = new QLabel(content);
    logo->setPixmap(QPixmap(QStringLiteral(":/branding/logo-256.png")).scaled(112, 112, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    logo->setAccessibleName(tr("AceStudio logo"));
    header->addWidget(logo);
    auto* titles = new QVBoxLayout;
    titles->addStretch();
    titles->addWidget(label(tr("GAME ASSET STUDIO"), "eyebrow", content));
    titles->addWidget(label(tr("Welcome to AceStudio"), "h1", content));
    m_licence = label(QString(), "muted", content);
    titles->addWidget(label(tr("Version %1").arg(QString::fromLatin1(kVersionString)), "muted", content));
    titles->addWidget(m_licence);
    titles->addStretch();
    header->addSpacing(12);
    header->addLayout(titles, 1);
    col->addLayout(header);

    // Quick generation: straight into AI Studio with the prompt filled in.
    auto* gen = card(content);
    auto* genLay = new QVBoxLayout(gen);
    genLay->setContentsMargins(18, 14, 18, 16);
    genLay->addWidget(label(tr("Describe an asset"), "h2", gen));
    auto* genRow = new QHBoxLayout;
    m_prompt = new QLineEdit(gen);
    m_prompt->setPlaceholderText(tr("A detailed pixel-art blacksmith hammering at a glowing forge…"));
    m_prompt->setAccessibleName(tr("Asset description"));
    m_prompt->setMinimumHeight(34);
    m_type = new QComboBox(gen);
    m_type->setAccessibleName(tr("Asset type"));
    for (const char* t : {"Character", "Monster", "Item", "Prop", "Environment", "Tileset", "Effect", "UI element"})
        m_type->addItem(tr(t), QString::fromLatin1(t));
    auto* go = new QPushButton(icon(Glyph::Sparkle), tr("Generate"), gen);
    go->setProperty("accent", true);
    go->setMinimumHeight(34);
    connect(go, &QPushButton::clicked, this, [this] {
        Q_EMIT generateRequested(m_prompt->text().trimmed(), m_type->currentData().toString());
    });
    connect(m_prompt, &QLineEdit::returnPressed, go, &QPushButton::click);
    genRow->addWidget(m_prompt, 1);
    genRow->addWidget(m_type);
    genRow->addWidget(go);
    genLay->addLayout(genRow);
    col->addWidget(gen);

    // Quick actions.
    col->addWidget(label(tr("Start"), "h2", content));
    auto* actions = new QGridLayout;
    actions->setSpacing(10);
    struct Act { Glyph g; QString title, sub; std::function<void()> run; };
    const std::vector<Act> acts = {
        {Glyph::New, tr("New Project"), tr("Blank canvas"), [this] { Q_EMIT newProjectRequested(); }},
        {Glyph::Open, tr("Open Project"), tr(".acestudio file"), [this] { Q_EMIT openProjectRequested(); }},
        {Glyph::Image, tr("Import Image"), tr("PNG into a project"), [this] { Q_EMIT importImageRequested(); }},
        {Glyph::Characters, tr("Characters"), tr("Profiles and animation"), [this] { Q_EMIT workspaceRequested(QStringLiteral("characters")); }},
        {Glyph::World, tr("World Builder"), tr("Tilesets and scenes"), [this] { Q_EMIT workspaceRequested(QStringLiteral("world")); }},
        {Glyph::Integrations, tr("AI Assistants"), tr("Claude Code, Codex…"), [this] { Q_EMIT workspaceRequested(QStringLiteral("integrations")); }},
    };
    for (std::size_t i = 0; i < acts.size(); ++i) {
        QToolButton* b = tile(acts[i].g, acts[i].title, acts[i].sub, content);
        connect(b, &QToolButton::clicked, this, acts[i].run);
        actions->addWidget(b, static_cast<int>(i / 6), static_cast<int>(i % 6));
    }
    col->addLayout(actions);

    // Templates.
    col->addWidget(label(tr("Templates"), "h2", content));
    auto* tmpl = new QGridLayout;
    tmpl->setSpacing(10);
    const auto ts = templates();
    for (std::size_t i = 0; i < ts.size(); ++i) {
        QToolButton* b = tile(Glyph::Grid, ts[i].name, ts[i].description, content);
        const ProjectTemplate t = ts[i];
        connect(b, &QToolButton::clicked, this, [this, t] { Q_EMIT templateRequested(t); });
        tmpl->addWidget(b, static_cast<int>(i / 6), static_cast<int>(i % 6));
    }
    col->addLayout(tmpl);

    // Recent projects with thumbnails.
    auto* recentHeader = new QHBoxLayout;
    recentHeader->addWidget(label(tr("Recent projects"), "h2", content));
    recentHeader->addStretch();
    auto* guide = new QPushButton(tr("User Guide"), content);
    connect(guide, &QPushButton::clicked, this, &HomePage::guideRequested);
    recentHeader->addWidget(guide);
    col->addLayout(recentHeader);
    m_recentBox = new QWidget(content);
    m_recentGrid = new QGridLayout(m_recentBox);
    m_recentGrid->setContentsMargins(0, 0, 0, 0);
    m_recentGrid->setSpacing(12);
    col->addWidget(m_recentBox);
    col->addStretch();

    connect(&m_ctx, &AppContext::recentProjectsChanged, this, &HomePage::rebuildRecent);
    rebuildRecent();
}

HomePage::~HomePage() { *m_alive = false; }

void HomePage::refreshLicence(const QString& text) { m_licence->setText(text); }

void HomePage::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    m_prompt->setFocus();
}

void HomePage::rebuildRecent() {
    while (QLayoutItem* it = m_recentGrid->takeAt(0)) {
        delete it->widget();
        delete it;
    }
    m_thumbLabels.clear();
    const QStringList recent = m_ctx.recentProjects();
    if (recent.isEmpty()) {
        m_recentGrid->addWidget(label(tr("Projects you open or save appear here."), "muted", m_recentBox), 0, 0);
        return;
    }
    QStringList toRender;
    int i = 0;
    for (const QString& path : recent) {
        if (i >= 12) break;
        auto* b = new QToolButton(m_recentBox);
        const QFileInfo fi(path);
        b->setText(fi.completeBaseName());
        b->setToolTip(QDir::toNativeSeparators(path));
        b->setAccessibleName(tr("Open %1").arg(fi.completeBaseName()));
        b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        b->setIconSize(QSize(120, 120));
        b->setFixedSize(150, 160);
        b->setIcon(icon(Glyph::Image));
        b->setCursor(Qt::PointingHandCursor);
        b->setStyleSheet(QStringLiteral("QToolButton { border: 1px solid palette(mid); border-radius: 12px; padding: 8px; }"
                                        "QToolButton:hover { border-color: palette(highlight); }"));
        connect(b, &QToolButton::clicked, this, [this, path] { Q_EMIT openRecentRequested(path); });
        m_recentGrid->addWidget(b, i / 6, i % 6);
        if (auto it = m_thumbCache.find(path); it != m_thumbCache.end()) b->setIcon(QIcon(QPixmap::fromImage(it->second)));
        else if (fi.exists()) toRender << path;
        else {
            b->setEnabled(false);
            b->setToolTip(tr("%1 (missing)").arg(QDir::toNativeSeparators(path)));
        }
        m_thumbLabels[path] = nullptr;
        ++i;
    }
    m_recentGrid->setColumnStretch(6, 1);
    if (toRender.isEmpty()) return;
    // One background thread renders the missing thumbnails in order.
    QPointer<HomePage> self(this);
    std::weak_ptr<bool> alive = m_alive;
    std::thread([self, alive, toRender] {
        for (const QString& p : toRender) {
            if (alive.expired()) return;
            QImage img = renderThumbnail(p, 120);
            if (alive.expired()) return;
            QMetaObject::invokeMethod(qApp, [self, p, img] {
                if (self) self->thumbnailReady(p, img);
            });
        }
    }).detach();
}

void HomePage::thumbnailReady(const QString& path, const QImage& img) {
    if (img.isNull()) return;
    m_thumbCache[path] = img;
    for (int i = 0; i < m_recentGrid->count(); ++i)
        if (auto* b = qobject_cast<QToolButton*>(m_recentGrid->itemAt(i)->widget()))
            if (b->toolTip() == QDir::toNativeSeparators(path)) {
                b->setIcon(QIcon(QPixmap::fromImage(img)));
                motion::fadeIn(b, 160);
            }
}

} // namespace acestudio::ui
