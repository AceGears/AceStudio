#include "PreviewPanel.h"
#include "CanvasWidget.h"
#include "DocumentView.h"
#include "QtBridge.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QScrollArea>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace acestudio::ui {

namespace {
constexpr int kFollowPlayhead = -1;
constexpr int kAllFrames = -2;
}

PreviewPanel::PreviewPanel(QWidget* parent) : QWidget(parent) {
    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    m_timer->setTimerType(Qt::PreciseTimer);
    connect(m_timer, &QTimer::timeout, this, [this] {
        if (!m_view) return;
        const bool more = m_player.advance();
        showFrame();
        if (more) tick();   // a finished finite tag holds its last frame
    });

    m_source = new QComboBox(this);
    m_source->setToolTip(tr("What to play"));
    connect(m_source, &QComboBox::currentIndexChanged, this, &PreviewPanel::restart);
    m_zoom = new QSpinBox(this);
    m_zoom->setRange(1, 32);
    m_zoom->setValue(4);
    m_zoom->setSuffix(QStringLiteral("×"));
    m_zoom->setToolTip(tr("Preview zoom"));
    connect(m_zoom, &QSpinBox::valueChanged, this, [this](int z) { showFrame(); Q_EMIT zoomChanged(z); });
    m_play = new QToolButton(this);
    m_play->setCheckable(true);
    m_play->setChecked(true);
    m_play->setText(tr("Pause"));
    m_play->setAutoRaise(true);
    connect(m_play, &QToolButton::toggled, this, &PreviewPanel::setPlaying);

    auto* top = new QHBoxLayout;
    top->addWidget(m_source, 1);
    top->addWidget(m_zoom);
    top->addWidget(m_play);

    m_image = new QLabel(this);
    m_image->setAlignment(Qt::AlignCenter);
    m_image->setAutoFillBackground(true);
    auto* scroll = new QScrollArea(this);
    scroll->setWidget(m_image);
    scroll->setWidgetResizable(true);
    scroll->setMinimumSize(96, 64);
    m_status = new QLabel(this);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 4, 6, 6);
    layout->addLayout(top);
    layout->addWidget(scroll, 1);
    layout->addWidget(m_status);
    setView(nullptr);
}

void PreviewPanel::setZoom(int zoom) { m_zoom->setValue(zoom); }
int PreviewPanel::zoom() const { return m_zoom->value(); }

void PreviewPanel::setView(DocumentView* view) {
    if (m_view) disconnect(m_view, nullptr, this, nullptr);
    m_view = view;
    if (m_view) {
        connect(m_view, &DocumentView::documentChanged, this, [this] {
            rebuildTags();
            // Keep the loop running across edits; only the pixels change.
            if (m_player.sequence().empty()) restart(); else showFrame();
        });
        connect(m_view, &DocumentView::currentFrameChanged, this, [this] {
            if (m_source->currentData().toInt() == kFollowPlayhead) {
                const auto t = tagAt(m_view->document(), m_view->currentFrame());
                const auto cur = tagAt(m_view->document(), m_player.currentFrame());
                if (t != cur || !t) restart();
            }
        });
    }
    rebuildTags();
    restart();
    setEnabled(m_view != nullptr);
}

void PreviewPanel::rebuildTags() {
    const QVariant keep = m_source->currentData();
    const QSignalBlocker block(m_source);
    m_source->clear();
    m_source->addItem(tr("Tag at playhead"), kFollowPlayhead);
    m_source->addItem(tr("All frames"), kAllFrames);
    if (m_view)
        for (std::size_t i = 0; i < m_view->document().tags().size(); ++i)
            m_source->addItem(tr("Tag: %1").arg(qs(m_view->document().tags()[i].name)), static_cast<int>(i));
    const int idx = m_source->findData(keep);
    m_source->setCurrentIndex(idx >= 0 ? idx : 0);
}

void PreviewPanel::restart() {
    m_timer->stop();
    if (!m_view) {
        m_image->clear();
        m_status->setText(tr("No document"));
        return;
    }
    const Document& d = m_view->document();
    const int src = m_source->currentData().toInt();
    if (src >= 0 && static_cast<std::size_t>(src) < d.tags().size()) {
        m_player.reset(d, d.tags()[static_cast<std::size_t>(src)].fromFrame);
    } else if (src == kAllFrames) {
        m_player.resetAll(d);
    } else {
        m_player.reset(d, m_view->currentFrame());
    }
    showFrame();
    if (m_playing) tick();
}

void PreviewPanel::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    if (m_stale) showFrame();
}

void PreviewPanel::showFrame() {
    if (!m_view || m_player.sequence().empty()) return;
    // Compositing a large canvas is expensive; skip it while the panel is hidden
    // (e.g. behind another tab) and catch up when it is shown again.
    if (!isVisible()) { m_stale = true; return; }
    m_stale = false;
    const Document& d = m_view->document();
    const int f = std::clamp(m_player.currentFrame(), 0, d.frameCount() - 1);
    const QImage img = toQImage(d.composite(f));
    const int z = m_zoom->value();
    // Checkerboard behind transparent pixels, in the canvas colours.
    const CanvasAppearance& look = m_view->canvas()->appearance();
    QPixmap pm(img.width() * z, img.height() * z);
    {
        const int s = std::max(2, look.checkerSize);
        QPixmap tile(s * 2, s * 2);
        QPainter tp(&tile);
        tp.fillRect(0, 0, s * 2, s * 2, look.checker1);
        tp.fillRect(s, 0, s, s, look.checker2);
        tp.fillRect(0, s, s, s, look.checker2);
        tp.end();
        QPainter p(&pm);
        p.drawTiledPixmap(pm.rect(), tile);
        p.drawImage(pm.rect(), img);   // nearest-neighbour: no smooth transform hint set
    }
    m_image->setPixmap(pm);
    m_status->setText(tr("Frame %1 · %2 ms%3").arg(f + 1).arg(d.frames()[static_cast<std::size_t>(f)].durationMs)
                          .arg(m_player.finished() ? tr(" · finished") : QString()));
}

void PreviewPanel::tick() {
    if (!m_view || m_player.sequence().empty()) return;
    const Document& d = m_view->document();
    const int f = std::clamp(m_player.currentFrame(), 0, d.frameCount() - 1);
    m_timer->start(d.frames()[static_cast<std::size_t>(f)].durationMs);
}

void PreviewPanel::setPlaying(bool on) {
    m_playing = on;
    m_play->setText(on ? tr("Pause") : tr("Play"));
    if (!on) { m_timer->stop(); return; }
    if (m_player.finished()) restart(); else tick();
}

} // namespace acestudio::ui
