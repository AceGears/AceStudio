#pragma once

#include <QPointer>
#include <QWidget>

class QLabel;
class QListWidget;
class QTimer;
class QToolButton;

namespace acestudio::ui {

class DocumentView;

// Frame strip with real-time playback that honours per-frame durations and
// loops. Frame editing (insert/delete/reorder, onion skin) is Phase 3.
class TimelinePanel : public QWidget {
    Q_OBJECT
public:
    explicit TimelinePanel(QWidget* parent = nullptr);
    void setView(DocumentView* view);

public Q_SLOTS:
    void togglePlayback();
    void stepFrame(int delta);

private:
    void rebuild();
    void syncSelection(int frame);
    void scheduleNext();
    void stop();

    QPointer<DocumentView> m_view;
    QListWidget* m_frames = nullptr;
    QLabel* m_info = nullptr;
    QLabel* m_tags = nullptr;
    QToolButton* m_play = nullptr;
    QTimer* m_timer = nullptr;
    bool m_syncing = false;
};

} // namespace acestudio::ui
