#pragma once
// Motion Studio: 3D skeletal motion (BVH, glTF/GLB) onto a 2D sprite (acestudio::mocap).
//
// Left: the source skeleton in 3D (drag to turn the camera, wheel to zoom).
// Right: the sprite posed by the retargeted motion, on the same clock. The
// camera (turn, tilt, roll, perspective) is shared: it decides how the 3D
// motion is seen, and so what the 2D sprite does. It never turns the art; a
// camera far from the view the sprite was drawn in is flagged.

#include "acestudio/core/Document.h"
#include "acestudio/core/Mocap.h"

#include <QDialog>
#include <QWidget>

#include <functional>
#include <optional>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSlider;
class QSpinBox;
class QTimer;
class QTreeWidget;

namespace acestudio::ui {

// The 3D source view: the projected skeleton on a floor grid.
class SkeletonView : public QWidget {
    Q_OBJECT
public:
    explicit SkeletonView(QWidget* parent = nullptr);
    void setMotion(const ::acestudio::mocap::Motion* m, const ::acestudio::mocap::BoneMap* map);
    void setCamera(const ::acestudio::mocap::Camera& c);
    void setTime(double seconds, bool inPlace);
    QSize sizeHint() const override { return {320, 320}; }

Q_SIGNALS:
    // Dragging turns (sideways) and tilts (up and down) the camera.
    void cameraDragged(double turn, double tilt);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;

private:
    const ::acestudio::mocap::Motion* m_motion = nullptr;
    const ::acestudio::mocap::BoneMap* m_map = nullptr;
    ::acestudio::mocap::Camera m_camera;
    double m_time = 0;
    bool m_inPlace = true;
    double m_zoom = 1;
    QPoint m_dragFrom;
    double m_dragTurn = 0, m_dragTilt = 0;
};

class MotionStudioDialog : public QDialog {
    Q_OBJECT
public:
    using OpenFn = std::function<void(const std::vector<Image>& frames, const std::vector<int>& durations,
                                      const std::vector<AnimationTag>& tags, const QString& name)>;
    MotionStudioDialog(const Image& sprite, const QString& name, OpenFn open, QWidget* parent = nullptr);
    ~MotionStudioDialog() override;

private:
    void loadMotion();
    void loadMotionFile(const QString& path);
    // The motion library (MotionLibrary.h).
    void refreshPresets(const QString& select = QString());
    void libraryChosen();
    void saveAsPreset();
    void removeSavedPreset();
    void useMotion(::acestudio::mocap::Motion motion, const QString& name, bool oneShot);
    void presetChosen();
    void cameraEdited();
    void scheduleRetarget();
    void retarget();
    void tick();
    void showFrame();
    void openDocument();
    ::acestudio::mocap::Camera camera() const;

    Image m_sprite;
    bool m_fourLegged = false;   // the sprite is four-legged: four-legged motion (legs found by shape)
    QString m_name;
    OpenFn m_open;
    std::optional<::acestudio::mocap::Motion> m_motion;
    ::acestudio::mocap::BoneMap m_map;
    QString m_motionName;
    bool m_oneShot = false;   // a built-in one-shot preset: retargeted without looping
    std::optional<::acestudio::mocap::RetargetResult> m_result;
    int m_frame = 0;
    bool m_syncing = false;

    QLabel* m_motionLabel = nullptr;
    QComboBox* m_presets = nullptr;
    QPushButton* m_savePreset = nullptr;
    QPushButton* m_removePreset = nullptr;
    QComboBox* m_view = nullptr;
    QSlider* m_turn = nullptr;
    QSlider* m_tilt = nullptr;
    QSlider* m_roll = nullptr;
    QLabel* m_turnValue = nullptr;
    QLabel* m_tiltValue = nullptr;
    QLabel* m_rollValue = nullptr;
    QCheckBox* m_perspective = nullptr;
    QDoubleSpinBox* m_start = nullptr;
    QDoubleSpinBox* m_length = nullptr;
    QSpinBox* m_fps = nullptr;
    QDoubleSpinBox* m_amount = nullptr;
    QCheckBox* m_inPlace = nullptr;
    SkeletonView* m_source = nullptr;
    QLabel* m_preview = nullptr;
    QSlider* m_scrub = nullptr;
    QPushButton* m_play = nullptr;
    QLabel* m_notes = nullptr;
    QTreeWidget* m_bones = nullptr;
    QPushButton* m_openButton = nullptr;
    QTimer* m_timer = nullptr;
    QTimer* m_debounce = nullptr;
};

} // namespace acestudio::ui
