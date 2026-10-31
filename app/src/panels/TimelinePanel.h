#pragma once

#include "acestudio/core/Animation.h"

#include <QPointer>
#include <QWidget>

#include <utility>

class QAction;
class QLabel;
class QHBoxLayout;
class QListWidget;
class QTimer;
class QToolButton;

namespace acestudio::ui {

class DocumentView;

// Frame strip: playback (tag direction, repeats and per-frame durations via
// AnimationPlayer), insert/duplicate/delete, drag or Alt+arrow reordering,
// durations, and tag ranges tinted in their colour.
class TimelinePanel : public QWidget {
    Q_OBJECT
public:
    explicit TimelinePanel(QWidget* parent = nullptr);
    void setView(DocumentView* view);
    // Adds a toolbar button bound to an action owned elsewhere (onion skin).
    void addActionButton(QAction* action);
    bool isPlaying() const;

public Q_SLOTS:
    void togglePlayback();
    void stepFrame(int delta);
    // Moves the selected frames (or the current one) one step left (-1) or right (+1).
    void moveCurrentFrame(int delta);
    void duplicateSelected();
    void deleteSelected();

Q_SIGNALS:
    void tagsRequested();

private:
    void rebuild();
    void syncSelection(int frame);
    void scheduleNext();
    void stop();
    void showContextMenu(const QPoint& pos);
    void editDuration(int frame);
    void editAllDurations();
    void editSelectedDurations();
    // Selected frame block [first, last]; the current frame alone when
    // nothing else is selected.
    std::pair<int, int> selectedRange() const;
    void setRange(int first, int last, int current);
    void onSelectionChanged();

    QPointer<DocumentView> m_view;
    QListWidget* m_frames = nullptr;
    QLabel* m_info = nullptr;
    QLabel* m_tags = nullptr;
    QToolButton* m_play = nullptr;
    QHBoxLayout* m_controls = nullptr;
    int m_actionInsert = 0;
    AnimationPlayer m_player;
    QTimer* m_timer = nullptr;
    bool m_syncing = false;
    int m_rangeFirst = -1, m_rangeLast = -1;   // contiguous multi-frame selection
    bool m_dropPending = false;                 // a drag of several frames is being applied
};

} // namespace acestudio::ui
