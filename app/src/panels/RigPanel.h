#pragma once

#include "acestudio/core/Rig.h"

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
// geometry, limits, lock and parts, IK chains, and a working pose that can be
// baked into the current frame. Every rig edit is one undo step (drags of a
// value merge); the pose is not part of the document until it is baked.
// The canvas shows the bones of the working pose.
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

    QPointer<DocumentView> m_view;
    RigPose m_pose;
    bool m_filling = false;

    QTreeWidget* m_tree = nullptr;
    QPushButton* m_autoRig = nullptr;
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
    QListWidget* m_chains = nullptr;
    QPushButton* m_addChain = nullptr;
    QPushButton* m_removeChain = nullptr;
    QComboBox* m_bend = nullptr;
    QDoubleSpinBox* m_targetX = nullptr;
    QDoubleSpinBox* m_targetY = nullptr;
    QPushButton* m_reach = nullptr;
    QDoubleSpinBox* m_turn = nullptr;
    QDoubleSpinBox* m_moveX = nullptr;
    QDoubleSpinBox* m_moveY = nullptr;
    QCheckBox* m_showBones = nullptr;
    QSpinBox* m_restFrame = nullptr;
    QPushButton* m_resetPose = nullptr;
    QPushButton* m_bake = nullptr;
    QLabel* m_status = nullptr;
};

} // namespace acestudio::ui
