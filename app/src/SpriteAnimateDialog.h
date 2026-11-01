#pragma once
// Animate Sprite: turns one still sprite into a set of animations without AI
// (acestudio::motion). The subject is measured (two legs, four legs, blob,
// object), the chosen actions are previewed live, checked, and opened as one
// document with a tag per action or written as strips.

#include "acestudio/core/Document.h"
#include "acestudio/core/SpriteMotion.h"

#include <QDialog>

#include <functional>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QSlider;
class QSpinBox;
class QTimer;
class QWidget;

namespace acestudio::ui {

class SpriteAnimateDialog : public QDialog {
    Q_OBJECT
public:
    using OpenFn = std::function<void(const std::vector<Image>& frames, const std::vector<int>& durations,
                                      const std::vector<AnimationTag>& tags, const QString& name)>;
    SpriteAnimateDialog(const Image& sprite, const QString& name, OpenFn open, QWidget* parent = nullptr, const Mask& wingMask = {});

private:
    ::acestudio::motion::Options options() const;
    std::vector<::acestudio::motion::Action> checkedActions() const;
    void rebuild();
    void showFrame();
    void openDocument();
    void exportStrips();

    Image m_sprite;
    Mask m_wingMask;
    QString m_name;
    OpenFn m_open;
    ::acestudio::motion::Subject m_subject;
    std::vector<::acestudio::motion::Clip> m_clips;
    int m_previewClip = 0;
    int m_previewFrame = 0;

    QListWidget* m_actions = nullptr;
    QComboBox* m_body = nullptr;
    QComboBox* m_face = nullptr;
    QSlider* m_amount = nullptr;
    QSpinBox* m_frames = nullptr;
    QSlider* m_wingLine = nullptr;
    QLabel* m_wingLabel = nullptr;
    QSpinBox* m_wingFrom = nullptr;
    QSpinBox* m_wingTo = nullptr;
    QLabel* m_spanLabel = nullptr;
    QWidget* m_customBox = nullptr;
    QDoubleSpinBox* m_bob = nullptr;
    QDoubleSpinBox* m_sway = nullptr;
    QDoubleSpinBox* m_squash = nullptr;
    QDoubleSpinBox* m_tilt = nullptr;
    QDoubleSpinBox* m_legSwing = nullptr;
    QLabel* m_preview = nullptr;
    QLabel* m_info = nullptr;
    QTimer* m_timer = nullptr;
};

} // namespace acestudio::ui
