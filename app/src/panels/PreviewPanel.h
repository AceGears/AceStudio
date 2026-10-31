#pragma once

#include "acestudio/core/Animation.h"

#include <QPointer>
#include <QWidget>

class QComboBox;
class QLabel;
class QSpinBox;
class QTimer;
class QToolButton;

namespace acestudio::ui {

class DocumentView;

// Plays the animation independently of the editing canvas, so artists can
// keep drawing on one frame while watching the loop. Honours tag direction,
// repeat count and per-frame durations via AnimationPlayer.
class PreviewPanel : public QWidget {
    Q_OBJECT
public:
    explicit PreviewPanel(QWidget* parent = nullptr);
    void setView(DocumentView* view);
    void setZoom(int zoom);
    int zoom() const;

Q_SIGNALS:
    void zoomChanged(int zoom);

protected:
    void showEvent(QShowEvent* e) override;

private:
    bool m_stale = false;   // a frame was skipped while hidden
    void rebuildTags();
    void restart();
    void showFrame();
    void tick();
    void setPlaying(bool on);

    QPointer<DocumentView> m_view;
    AnimationPlayer m_player;
    QComboBox* m_source = nullptr;
    QSpinBox* m_zoom = nullptr;
    QToolButton* m_play = nullptr;
    QLabel* m_image = nullptr;
    QLabel* m_status = nullptr;
    QTimer* m_timer = nullptr;
    bool m_playing = true;
};

} // namespace acestudio::ui
