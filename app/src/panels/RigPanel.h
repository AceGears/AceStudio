#pragma once

#include "acestudio/core/Rig.h"

#include <QPointF>
#include <QPointer>
#include <QWidget>

#include <functional>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QTreeWidget;

namespace acestudio::ui {

class DocumentView;

// The document's rig (ADR-ANIM-004): the bone tree, the selected bone's rest
// geometry, limits, lock and parts, IK chains, a working pose that can be
// baked into the current frame, and pose keys with curves (rig version 2)
// that can be baked into every frame between them. Every rig edit is one
// undo step (drags of a value merge); the working pose is not part of the
// document until it is keyed or baked. On a frame change with keys, the
// working pose becomes the keyed pose of that frame. The canvas shows the
// bones of the working pose; with "Drag bones" on, dragging a bone's tip turns
// it (or reaches with its IK chain), dragging a root's joint moves the
// figure, and Ctrl+drag edits the rest bones (at the rest pose only).
class RigPanel : public QWidget {
    Q_OBJECT
public:
    explicit RigPanel(QWidget* parent = nullptr);
    void setView(DocumentView* view);

public Q_SLOTS:
    // Cuts the current layer into bound part layers with bones and leg IK.
    void autoRigCurrentLayer();
    // Draws the parts of the rest frame, in the working pose, into the
    // current frame's part cels.
    void bakePose();
    // Draws every frame from the first key to the last, posed by the keys.
    void bakeKeys();

Q_SIGNALS:
    void statusMessage(const QString& text);

private:
    void rebuild();
    void showBone();
    void showChain();
    void updateOverlay();
    BoneId selectedBone() const;
    int selectedChain() const;
    void selectBone(BoneId id);
    // Applies `fn` to a copy of the rig (an empty one when there is none) and
    // commits it as one undo step; returns false (and says why) on error.
    bool edit(const QString& label, const std::function<Status(Rig&)>& fn, long long mergeKey = -1);
    void report(const QString& what, const Error& e);
    void addBone();
    void removeBone();
    void applyGeometry();
    void applyLimits();
    void showKeys();
    void setKey();
    void deleteKey();
    void loadKeyedPose();
    int selectedKeyFrame() const;
    void boneGrabbed(int index, bool tail, QPointF at, Qt::KeyboardModifiers mods);
    void boneDragged(QPointF at);
    void boneReleased();

    QPointer<DocumentView> m_view;
    RigPose m_pose;
    bool m_filling = false;
    // The bone handle drag in progress on the canvas.
    bool m_dragging = false;
    bool m_dragTail = false;
    bool m_dragRest = false;
    BoneId m_dragBone = kNoBone;
    QPointF m_dragStart;
    RigPose m_dragPose;

    QTreeWidget* m_tree = nullptr;
    QPushButton* m_autoRig = nullptr;
    QComboBox* m_rigAs = nullptr;
    QPushButton* m_addBone = nullptr;
    QPushButton* m_removeBone = nullptr;
    QPushButton* m_removeRig = nullptr;
    QWidget* m_boneBox = nullptr;
    QLineEdit* m_name = nullptr;
    QComboBox* m_parent = nullptr;
    QDoubleSpinBox* m_x = nullptr;
    QDoubleSpinBox* m_y = nullptr;
    QDoubleSpinBox* m_length = nullptr;
    QDoubleSpinBox* m_angle = nullptr;
    QCheckBox* m_moveChildren = nullptr;
    QCheckBox* m_limited = nullptr;
    QDoubleSpinBox* m_min = nullptr;
    QDoubleSpinBox* m_max = nullptr;
    QCheckBox* m_locked = nullptr;
    QListWidget* m_parts = nullptr;
    QPushButton* m_bind = nullptr;
    QPushButton* m_unbind = nullptr;
    QComboBox* m_bendWith = nullptr;   // the current layer's part: rigid, or bent with a child of its bone
    QListWidget* m_chains = nullptr;
    QPushButton* m_addChain = nullptr;
    QPushButton* m_removeChain = nullptr;
    QComboBox* m_bend = nullptr;
    QDoubleSpinBox* m_targetX = nullptr;
    QDoubleSpinBox* m_targetY = nullptr;
    QPushButton* m_reach = nullptr;
    QDoubleSpinBox* m_turn = nullptr;
    QDoubleSpinBox* m_stretch = nullptr;   // the selected bone's drawn length, % of its own (RigPose::stretch)
    QDoubleSpinBox* m_moveX = nullptr;
    QDoubleSpinBox* m_moveY = nullptr;
    QCheckBox* m_showBones = nullptr;
    QCheckBox* m_dragBones = nullptr;
    QSpinBox* m_restFrame = nullptr;
    QPushButton* m_resetPose = nullptr;
    QPushButton* m_bake = nullptr;
    QListWidget* m_keys = nullptr;
    QComboBox* m_curve = nullptr;
    QPushButton* m_setKey = nullptr;
    QPushButton* m_deleteKey = nullptr;
    QPushButton* m_bakeKeys = nullptr;
    QLabel* m_status = nullptr;
};

} // namespace acestudio::ui
