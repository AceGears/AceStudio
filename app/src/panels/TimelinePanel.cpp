#include "TimelinePanel.h"
#include "DocumentView.h"
#include "QtBridge.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
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
        m_view->setCurrentFrame((m_view->currentFrame() + 1) % m_view->document().frameCount());
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
    controls->addWidget(prev);
    controls->addWidget(m_play);
    controls->addWidget(next);
    controls->addSpacing(12);
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

void TimelinePanel::stepFrame(int delta) {
    if (!m_view) return;
    const int n = m_view->document().frameCount();
    m_view->setCurrentFrame(((m_view->currentFrame() + delta) % n + n) % n);
}

void TimelinePanel::togglePlayback() {
    if (m_timer->isActive()) return stop();
    if (!m_view || m_view->document().frameCount() < 2) return;
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
