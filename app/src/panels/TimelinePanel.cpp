#include "TimelinePanel.h"
#include "DocumentView.h"
#include "QtBridge.h"

#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace acestudio::ui {

namespace {
// Transport icons drawn in code so they render identically on every
// platform and theme (stock media icons are missing on some systems).
enum class Glyph { Prev, Play, Pause, Next };
QIcon transportIcon(Glyph g, const QColor& color) {
    QPixmap pm(32, 32);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    auto tri = [&](qreal x0, qreal x1) {
        QPainterPath path;
        path.moveTo(x0, 7); path.lineTo(x1, 16); path.lineTo(x0, 25); path.closeSubpath();
        p.drawPath(path);
    };
    switch (g) {
    case Glyph::Play: tri(10, 25); break;
    case Glyph::Pause: p.drawRect(QRectF(9, 7, 5, 18)); p.drawRect(QRectF(18, 7, 5, 18)); break;
    case Glyph::Next: tri(8, 20); p.drawRect(QRectF(21, 7, 3, 18)); break;
    case Glyph::Prev:
        p.drawRect(QRectF(8, 7, 3, 18));
        { QPainterPath path; path.moveTo(24, 7); path.lineTo(12, 16); path.lineTo(24, 25); path.closeSubpath(); p.drawPath(path); }
        break;
    }
    return QIcon(pm);
}
} // namespace

TimelinePanel::TimelinePanel(QWidget* parent) : QWidget(parent) {
    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    m_timer->setTimerType(Qt::PreciseTimer);
    connect(m_timer, &QTimer::timeout, this, [this] {
        if (!m_view) return stop();
        if (!m_player.advance()) return stop();   // finite tag repeat finished
        m_view->setCurrentFrame(std::clamp(m_player.currentFrame(), 0, m_view->document().frameCount() - 1));
        scheduleNext();
    });

    const QColor fg = palette().color(QPalette::WindowText);
    auto makeButton = [this, fg](Glyph icon, const QString& tip) {
        auto* b = new QToolButton(this);
        b->setIcon(transportIcon(icon, fg));
        b->setIconSize(QSize(16, 16));
        b->setToolTip(tip);
        b->setAutoRaise(true);
        return b;
    };
    auto* prev = makeButton(Glyph::Prev, tr("Previous frame (,)"));
    m_play = makeButton(Glyph::Play, tr("Play / pause (Enter)"));
    auto* next = makeButton(Glyph::Next, tr("Next frame (.)"));
    connect(prev, &QToolButton::clicked, this, [this] { stepFrame(-1); });
    connect(next, &QToolButton::clicked, this, [this] { stepFrame(1); });
    connect(m_play, &QToolButton::clicked, this, &TimelinePanel::togglePlayback);

    m_info = new QLabel(this);
    m_tags = new QLabel(this);
    m_tags->setTextFormat(Qt::RichText);

    auto* controls = new QHBoxLayout;
    m_controls = controls;
    controls->addWidget(prev);
    controls->addWidget(m_play);
    controls->addWidget(next);
    controls->addSpacing(12);
    auto frameButton = [this, controls](const QString& text, const QString& tip, auto fn) {
        auto* b = new QToolButton(this);
        b->setText(text);
        b->setToolTip(tip);
        b->setAutoRaise(true);
        connect(b, &QToolButton::clicked, this, fn);
        controls->addWidget(b);
    };
    frameButton(tr("+ Frame"), tr("Insert an empty frame after the current one"), [this] {
        if (!m_view) return;
        const int at = m_view->currentFrame() + 1;
        auto r = m_view->editor().insertFrame(at, m_view->document().frames()[static_cast<std::size_t>(at - 1)].durationMs);
        if (r) m_view->setCurrentFrame(at); else Q_EMIT m_view->statusMessage(qs(r.error().message));
    });
    frameButton(tr("Duplicate"), tr("Duplicate the current frame"), [this] {
        if (!m_view) return;
        auto r = m_view->editor().duplicateFrame(m_view->currentFrame());
        if (r) m_view->setCurrentFrame(r.value()); else Q_EMIT m_view->statusMessage(qs(r.error().message));
    });
    frameButton(tr("Delete"), tr("Delete the current frame"), [this] {
        if (!m_view) return;
        auto st = m_view->editor().removeFrame(m_view->currentFrame());
        if (!st) Q_EMIT m_view->statusMessage(qs(st.error().message));
    });
    frameButton(tr("Tags…"), tr("Add, edit or remove animation tags"), [this] { Q_EMIT tagsRequested(); });
    controls->addSpacing(12);
    m_actionInsert = controls->count();
    controls->addWidget(m_info);
    controls->addStretch();
    controls->addWidget(m_tags);

    m_frames = new QListWidget(this);
    m_frames->setFlow(QListView::LeftToRight);
    m_frames->setWrapping(false);
    m_frames->setUniformItemSizes(true);
    m_frames->setGridSize(QSize(76, 46));
    m_frames->setTextElideMode(Qt::ElideNone);
    m_frames->setStyleSheet(QStringLiteral("QListWidget::item { padding: 0px; }"));
    m_frames->setViewMode(QListView::ListMode);
    m_frames->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_frames->setFixedHeight(64);
    m_frames->setToolTip(tr("Drag to reorder · double-click for duration · right-click for more"));
    m_frames->setDragDropMode(QAbstractItemView::InternalMove);
    m_frames->setDefaultDropAction(Qt::MoveAction);
    m_frames->setMovement(QListView::Snap);
    m_frames->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_frames, &QWidget::customContextMenuRequested, this, &TimelinePanel::showContextMenu);
    connect(m_frames, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) { editDuration(m_frames->row(item)); });
    // The list moves its own row first; apply the real (undoable) move once
    // the drop has finished, then rebuild from the document.
    connect(m_frames->model(), &QAbstractItemModel::rowsMoved, this,
            [this](const QModelIndex&, int start, int, const QModelIndex&, int row) {
                if (m_syncing || !m_view) return;
                const int to = row > start ? row - 1 : row;
                QTimer::singleShot(0, this, [this, start, to] {
                    if (!m_view) return;
                    if (start == to) return rebuild();
                    auto st = m_view->editor().moveFrame(start, to);
                    if (!st) { Q_EMIT m_view->statusMessage(qs(st.error().message)); rebuild(); return; }
                    m_view->setCurrentFrame(to);
                });
            });
    connect(m_frames, &QListWidget::currentRowChanged, this, [this](int row) {
        if (!m_syncing && m_view && row >= 0) m_view->setCurrentFrame(row);
    });

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 4, 6, 6);
    layout->addLayout(controls);
    layout->addWidget(m_frames);
    layout->addStretch();
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    rebuild();
}

void TimelinePanel::setView(DocumentView* view) {
    stop();
    if (m_view) disconnect(m_view, nullptr, this, nullptr);
    m_view = view;
    if (m_view) {
        connect(m_view, &DocumentView::documentChanged, this, &TimelinePanel::rebuild);
        connect(m_view, &DocumentView::currentFrameChanged, this, &TimelinePanel::syncSelection);
    }
    rebuild();
}

void TimelinePanel::rebuild() {
    m_syncing = true;
    m_frames->clear();
    if (m_view) {
        const Document& d = m_view->document();
        for (int i = 0; i < d.frameCount(); ++i) {
            auto* item = new QListWidgetItem(QStringLiteral("%1\n%2 ms").arg(i + 1).arg(d.frames()[static_cast<std::size_t>(i)].durationMs));
            item->setTextAlignment(Qt::AlignCenter);
            item->setFlags(item->flags() | Qt::ItemIsDragEnabled);
            if (auto t = tagAt(d, i)) {
                const auto& tag = d.tags()[*t];
                QColor c(qs(tag.color).left(7));
                c.setAlpha(70);
                item->setBackground(c);
                item->setToolTip(tr("Frame %1 · tag “%2”").arg(i + 1).arg(qs(tag.name)));
            }
            m_frames->addItem(item);
        }
        QStringList tags;
        for (const auto& t : d.tags())
            tags << QStringLiteral("<span style='color:%1'>■</span> %2 (%3–%4)")
                        .arg(qs(t.color).left(7), qs(t.name).toHtmlEscaped()).arg(t.fromFrame + 1).arg(t.toFrame + 1);
        m_tags->setText(tags.join(QStringLiteral("&nbsp;&nbsp;")));
    } else {
        m_tags->clear();
    }
    m_syncing = false;
    syncSelection(m_view ? m_view->currentFrame() : -1);
    setEnabled(m_view != nullptr);
}

void TimelinePanel::syncSelection(int frame) {
    m_syncing = true;
    m_frames->setCurrentRow(frame);
    m_syncing = false;
    if (m_view)
        m_info->setText(tr("Frame %1 of %2").arg(frame + 1).arg(m_view->document().frameCount()));
    else
        m_info->setText(tr("No document"));
}

void TimelinePanel::addActionButton(QAction* action) {
    auto* b = new QToolButton(this);
    b->setDefaultAction(action);
    b->setAutoRaise(true);
    m_controls->insertWidget(m_actionInsert++, b);
}

bool TimelinePanel::isPlaying() const { return m_timer->isActive(); }

void TimelinePanel::moveCurrentFrame(int delta) {
    if (!m_view) return;
    const int from = m_view->currentFrame();
    const int to = from + delta;
    if (to < 0 || to >= m_view->document().frameCount()) return;
    auto st = m_view->editor().moveFrame(from, to);
    if (!st) return void(Q_EMIT m_view->statusMessage(qs(st.error().message)));
    m_view->setCurrentFrame(to);
}

void TimelinePanel::editDuration(int f) {
    if (!m_view || f < 0) return;
    bool ok = false;
    const int ms = QInputDialog::getInt(this, tr("Frame Duration"), tr("Duration of frame %1 (ms):").arg(f + 1),
                                        m_view->document().frames()[static_cast<std::size_t>(f)].durationMs, 1, 65535, 10, &ok);
    if (!ok) return;
    auto st = m_view->editor().setFrameDuration(f, ms);
    if (!st) Q_EMIT m_view->statusMessage(qs(st.error().message));
}

void TimelinePanel::editAllDurations() {
    if (!m_view) return;
    bool ok = false;
    const int ms = QInputDialog::getInt(this, tr("Frame Duration"), tr("Duration of every frame (ms):"),
                                        m_view->document().frames()[static_cast<std::size_t>(m_view->currentFrame())].durationMs,
                                        1, 65535, 10, &ok);
    if (!ok) return;
    auto st = m_view->editor().setFrameDurations(0, m_view->document().frameCount() - 1, ms);
    if (!st) Q_EMIT m_view->statusMessage(qs(st.error().message));
}

void TimelinePanel::showContextMenu(const QPoint& pos) {
    if (!m_view) return;
    QListWidgetItem* item = m_frames->itemAt(pos);
    if (item) m_view->setCurrentFrame(m_frames->row(item));
    QMenu menu(this);
    const int f = m_view->currentFrame();
    const int n = m_view->document().frameCount();
    menu.addAction(tr("Frame &Duration…"), this, [this, f] { editDuration(f); });
    menu.addAction(tr("Duration of &All Frames…"), this, [this] { editAllDurations(); });
    menu.addSeparator();
    menu.addAction(tr("Move &Left"), this, [this] { moveCurrentFrame(-1); })->setEnabled(f > 0);
    menu.addAction(tr("Move &Right"), this, [this] { moveCurrentFrame(1); })->setEnabled(f + 1 < n);
    menu.addSeparator();
    menu.addAction(tr("&Tags…"), this, [this] { Q_EMIT tagsRequested(); });
    menu.exec(m_frames->viewport()->mapToGlobal(pos));
}

void TimelinePanel::stepFrame(int delta) {
    if (!m_view) return;
    const int n = m_view->document().frameCount();
    m_view->setCurrentFrame(((m_view->currentFrame() + delta) % n + n) % n);
}

void TimelinePanel::togglePlayback() {
    if (m_timer->isActive()) return stop();
    if (!m_view || m_view->document().frameCount() < 2) return;
    // Plays the tag under the playhead (direction and repeats honoured),
    // or every frame when the playhead is outside all tags.
    m_player.reset(m_view->document(), m_view->currentFrame());
    m_play->setIcon(transportIcon(Glyph::Pause, palette().color(QPalette::WindowText)));
    scheduleNext();
}

void TimelinePanel::scheduleNext() {
    if (!m_view) return stop();
    const auto& frames = m_view->document().frames();
    const int f = std::clamp(m_view->currentFrame(), 0, static_cast<int>(frames.size()) - 1);
    m_timer->start(frames[static_cast<std::size_t>(f)].durationMs);
}

void TimelinePanel::stop() {
    m_timer->stop();
    if (m_play) m_play->setIcon(transportIcon(Glyph::Play, palette().color(QPalette::WindowText)));
}

} // namespace acestudio::ui
