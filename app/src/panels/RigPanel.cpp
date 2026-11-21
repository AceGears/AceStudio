#include "RigPanel.h"

#include "CanvasWidget.h"
#include "DocumentView.h"
#include "QtBridge.h"
#include "Theme.h"

#include "acestudio/core/RigTools.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <map>

namespace acestudio::ui {

namespace {

constexpr double kPi = 3.14159265358979323846;
double toDeg(double r) { return r * 180.0 / kPi; }
double toRad(double d) { return d * kPi / 180.0; }

// Undo merge keys: consecutive changes of one field of one bone merge.
constexpr long long kGeometryKey = 1'000'000;
constexpr long long kLimitsKey = 2'000'000;

QDoubleSpinBox* spin(double lo, double hi, double step, int decimals, const QString& suffix, const QString& name) {
    auto* s = new QDoubleSpinBox;
    s->setRange(lo, hi);
    s->setSingleStep(step);
    s->setDecimals(decimals);
    s->setSuffix(suffix);
    s->setKeyboardTracking(false);   // one edit per committed value, not per keystroke
    s->setAccessibleName(name);
    return s;
}

QPushButton* button(const QString& text, const QString& tip) {
    auto* b = new QPushButton(text);
    b->setToolTip(tip);
    return b;
}

} // namespace

RigPanel::RigPanel(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 6, 8, 6);
    root->setSpacing(6);

    // ---- Rig actions
    auto* top = new QHBoxLayout;
    m_autoRig = button(tr("Auto-Rig"), tr("Cut the current layer into part layers with bones and leg IK"));
    m_addBone = button(tr("Add Bone"), tr("Add a bone under the selected one (or a new root)"));
    m_removeBone = button(tr("Remove"), tr("Remove the selected bone; its children and parts go to its parent"));
    m_rigAs = new QComboBox;
    m_rigAs->setAccessibleName(tr("Auto-rig cuts"));
    m_rigAs->setToolTip(tr("What Auto-Rig cuts: legs, wings or fins (Auto picks from the sprite; a winged creature with legs needs Wings)"));
    m_rigAs->addItem(tr("Auto"), static_cast<int>(rigs::AutoRigBody::Auto));
    m_rigAs->addItem(tr("Legs"), static_cast<int>(rigs::AutoRigBody::Legs));
    m_rigAs->addItem(tr("Wings"), static_cast<int>(rigs::AutoRigBody::Wings));
    m_rigAs->addItem(tr("Fins"), static_cast<int>(rigs::AutoRigBody::Fins));
    top->addWidget(m_autoRig);
    top->addWidget(m_rigAs);
    top->addWidget(m_addBone);
    top->addWidget(m_removeBone);
    root->addLayout(top);

    m_tree = new QTreeWidget;
    m_tree->setHeaderHidden(true);
    m_tree->setAccessibleName(tr("Bones"));
    m_tree->setMinimumHeight(110);
    root->addWidget(m_tree, 1);

    // ---- Selected bone
    auto* boneGroup = new QGroupBox(tr("Bone"));
    m_boneBox = boneGroup;
    auto* form = new QFormLayout(boneGroup);
    form->setLabelAlignment(Qt::AlignRight);
    m_name = new QLineEdit;
    m_name->setAccessibleName(tr("Bone name"));
    m_name->setMaxLength(128);
    form->addRow(tr("Name"), m_name);
    m_parent = new QComboBox;
    m_parent->setAccessibleName(tr("Parent bone"));
    form->addRow(tr("Parent"), m_parent);
    m_x = spin(-32768, 32768, 1, 1, tr(" px"), tr("Head X"));
    m_y = spin(-32768, 32768, 1, 1, tr(" px"), tr("Head Y"));
    auto* head = new QHBoxLayout;
    head->addWidget(m_x);
    head->addWidget(m_y);
    form->addRow(tr("Head"), head);
    m_length = spin(0, 32768, 1, 1, tr(" px"), tr("Length"));
    form->addRow(tr("Length"), m_length);
    m_angle = spin(-360, 360, 5, 1, QStringLiteral("°"), tr("Angle"));
    m_angle->setToolTip(tr("Rest direction, clockwise from pointing right"));
    form->addRow(tr("Angle"), m_angle);
    m_moveChildren = new QCheckBox(tr("Children follow"));
    m_moveChildren->setChecked(true);
    form->addRow(QString(), m_moveChildren);
    m_limited = new QCheckBox(tr("Limit turns"));
    form->addRow(QString(), m_limited);
    m_min = spin(-360, 360, 5, 1, QStringLiteral("°"), tr("Smallest turn"));
    m_max = spin(-360, 360, 5, 1, QStringLiteral("°"), tr("Largest turn"));
    auto* limits = new QHBoxLayout;
    limits->addWidget(m_min);
    limits->addWidget(m_max);
    form->addRow(tr("Turns"), limits);
    m_locked = new QCheckBox(tr("Locked (posing leaves it at rest)"));
    form->addRow(QString(), m_locked);
    m_parts = new QListWidget;
    m_parts->setAccessibleName(tr("Parts moved by the bone"));
    m_parts->setMaximumHeight(70);
    form->addRow(tr("Parts"), m_parts);
    auto* bindRow = new QHBoxLayout;
    m_bind = button(tr("Bind Layer"), tr("The current layer moves with this bone"));
    m_unbind = button(tr("Unbind Layer"), tr("The current layer no longer moves with a bone"));
    bindRow->addWidget(m_bind);
    bindRow->addWidget(m_unbind);
    form->addRow(QString(), bindRow);
    // A part drawn over two bones bends at the joint with a child of its bone.
    m_bendWith = new QComboBox;
    m_bendWith->setAccessibleName(tr("Current layer bends with"));
    m_bendWith->setToolTip(tr("Bend the current layer's part across the joint with a child of its bone, "
                              "so a one-piece arm or leg bends instead of turning stiffly"));
    form->addRow(tr("Bends with"), m_bendWith);
    root->addWidget(boneGroup);

    // ---- IK chains
    auto* ikGroup = new QGroupBox(tr("IK"));
    auto* ik = new QFormLayout(ikGroup);
    m_chains = new QListWidget;
    m_chains->setAccessibleName(tr("IK chains"));
    m_chains->setMaximumHeight(60);
    ik->addRow(m_chains);
    auto* chainRow = new QHBoxLayout;
    m_addChain = button(tr("Add Chain"), tr("Two-bone IK ending at the selected bone"));
    m_removeChain = button(tr("Remove Chain"), tr("Remove the selected IK chain"));
    chainRow->addWidget(m_addChain);
    chainRow->addWidget(m_removeChain);
    ik->addRow(chainRow);
    m_bend = new QComboBox;
    m_bend->addItem(tr("Clockwise"), 1);
    m_bend->addItem(tr("Anticlockwise"), -1);
    m_bend->setAccessibleName(tr("Joint bends"));
    ik->addRow(tr("Bends"), m_bend);
    m_targetX = spin(-32768, 32768, 1, 1, tr(" px"), tr("Target X"));
    m_targetY = spin(-32768, 32768, 1, 1, tr(" px"), tr("Target Y"));
    auto* target = new QHBoxLayout;
    target->addWidget(m_targetX);
    target->addWidget(m_targetY);
    ik->addRow(tr("Target"), target);
    m_reach = button(tr("Reach"), tr("Pose the chain so its end reaches the target"));
    ik->addRow(QString(), m_reach);
    root->addWidget(ikGroup);

    // ---- Pose
    auto* poseGroup = new QGroupBox(tr("Pose"));
    auto* pose = new QFormLayout(poseGroup);
    m_turn = spin(-360, 360, 5, 1, QStringLiteral("°"), tr("Turn of the selected bone"));
    pose->addRow(tr("Turn"), m_turn);
    m_moveX = spin(-32768, 32768, 1, 0, tr(" px"), tr("Move X"));
    m_moveY = spin(-32768, 32768, 1, 0, tr(" px"), tr("Move Y"));
    auto* move = new QHBoxLayout;
    move->addWidget(m_moveX);
    move->addWidget(m_moveY);
    pose->addRow(tr("Move"), move);
    m_showBones = new QCheckBox(tr("Show bones on the canvas"));
    m_showBones->setChecked(true);
    pose->addRow(QString(), m_showBones);
    m_dragBones = new QCheckBox(tr("Drag bones on the canvas"));
    m_dragBones->setChecked(true);
    m_dragBones->setToolTip(tr("Drag a bone's tip to turn it (IK chains reach), a root's joint to move the figure; "
                               "Ctrl+drag edits the rest bones"));
    pose->addRow(QString(), m_dragBones);
    m_restFrame = new QSpinBox;
    m_restFrame->setRange(1, 1);
    m_restFrame->setKeyboardTracking(false);
    m_restFrame->setAccessibleName(tr("Rest frame"));
    m_restFrame->setToolTip(tr("The frame whose parts are drawn at rest (saved with the rig); poses and keys are drawn from it"));
    pose->addRow(tr("Rest frame"), m_restFrame);
    auto* poseRow = new QHBoxLayout;
    m_resetPose = button(tr("Rest Pose"), tr("Back to the rest pose (nothing is drawn until you bake)"));
    m_bake = button(tr("Bake to Frame"), tr("Draw the pose into the current frame's part layers (one undo step)"));
    poseRow->addWidget(m_resetPose);
    poseRow->addWidget(m_bake);
    pose->addRow(poseRow);
    m_removeRig = button(tr("Remove Rig"), tr("Remove the bones and IK; the part layers stay"));
    pose->addRow(m_removeRig);
    root->addWidget(poseGroup);

    // ---- Keys
    auto* keyGroup = new QGroupBox(tr("Keys"));
    auto* keys = new QFormLayout(keyGroup);
    m_keys = new QListWidget;
    m_keys->setAccessibleName(tr("Pose keys"));
    m_keys->setToolTip(tr("Keyed frames; activate one to go to its frame"));
    m_keys->setMaximumHeight(80);
    keys->addRow(m_keys);
    m_curve = new QComboBox;
    m_curve->setAccessibleName(tr("Key curve"));
    m_curve->setToolTip(tr("How the frames after a key move toward the next key"));
    m_curve->addItem(tr("Linear"), static_cast<int>(CurveType::Linear));
    m_curve->addItem(tr("Ease in"), static_cast<int>(CurveType::EaseIn));
    m_curve->addItem(tr("Ease out"), static_cast<int>(CurveType::EaseOut));
    m_curve->addItem(tr("Ease in and out"), static_cast<int>(CurveType::EaseInOut));
    m_curve->addItem(tr("Stepped (hold)"), static_cast<int>(CurveType::Stepped));
    m_curve->addItem(tr("Bezier"), static_cast<int>(CurveType::Bezier));
    keys->addRow(tr("Curve"), m_curve);
    auto* keyRow = new QHBoxLayout;
    m_setKey = button(tr("Set Key"), tr("Key the working pose on the current frame"));
    m_deleteKey = button(tr("Delete Key"), tr("Remove the selected key"));
    m_bakeKeys = button(tr("Bake Keys"), tr("Draw every frame from the first key to the last from the rest frame (one undo step)"));
    keyRow->addWidget(m_setKey);
    keyRow->addWidget(m_deleteKey);
    keyRow->addWidget(m_bakeKeys);
    keys->addRow(keyRow);
    root->addWidget(keyGroup);

    m_status = new QLabel;
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(m_status);

    // ---- Wiring
    connect(m_autoRig, &QPushButton::clicked, this, &RigPanel::autoRigCurrentLayer);
    connect(m_addBone, &QPushButton::clicked, this, &RigPanel::addBone);
    connect(m_removeBone, &QPushButton::clicked, this, &RigPanel::removeBone);
    connect(m_tree, &QTreeWidget::currentItemChanged, this, [this] {
        if (!m_filling) showBone();
        updateOverlay();
    });
    connect(m_name, &QLineEdit::editingFinished, this, [this] {
        const BoneId id = selectedBone();
        const Rig* r = m_view && m_view->document().rig() ? &*m_view->document().rig() : nullptr;
        const Bone* bone = r ? r->bone(id) : nullptr;
        if (m_filling || !bone || bone->name == m_name->text().trimmed().toStdString()) return;
        edit(tr("Rename Bone"), [&](Rig& rig) { return rigs::renameBone(rig, id, m_name->text().trimmed().toStdString()); });
    });
    connect(m_parent, &QComboBox::activated, this, [this] {
        const BoneId id = selectedBone();
        if (m_filling || !id) return;
        const auto parent = static_cast<BoneId>(m_parent->currentData().toUInt());
        edit(tr("Reparent Bone"), [&](Rig& rig) { return rigs::reparentBone(rig, id, parent); });
    });
    for (QDoubleSpinBox* s : {m_x, m_y, m_length, m_angle})
        connect(s, &QDoubleSpinBox::valueChanged, this, &RigPanel::applyGeometry);
    for (QDoubleSpinBox* s : {m_min, m_max}) connect(s, &QDoubleSpinBox::valueChanged, this, &RigPanel::applyLimits);
    connect(m_limited, &QCheckBox::toggled, this, &RigPanel::applyLimits);
    connect(m_locked, &QCheckBox::toggled, this, [this](bool on) {
        const BoneId id = selectedBone();
        if (m_filling || !id) return;
        edit(on ? tr("Lock Bone") : tr("Unlock Bone"), [&](Rig& rig) { return rigs::setLocked(rig, id, on); });
    });
    connect(m_bind, &QPushButton::clicked, this, [this] {
        const BoneId id = selectedBone();
        if (!m_view || !id) return;
        const LayerId layer = m_view->currentLayer();
        edit(tr("Bind Layer to Bone"), [&](Rig& rig) { return rigs::bindLayer(rig, layer, id); });
    });
    connect(m_unbind, &QPushButton::clicked, this, [this] {
        if (!m_view || !m_view->document().rig()) return;
        const LayerId layer = m_view->currentLayer();
        edit(tr("Unbind Layer"), [&](Rig& rig) { return rigs::bindLayer(rig, layer, kNoBone); });
    });
    connect(m_bendWith, &QComboBox::currentIndexChanged, this, [this] {
        if (m_filling || !m_view || !m_view->document().rig()) return;
        const LayerId layer = m_view->currentLayer();
        const BoneId child = static_cast<BoneId>(m_bendWith->currentData().toUInt());
        edit(child ? tr("Bend Part Across Joint") : tr("Make Part Rigid"), [&](Rig& rig) { return rigs::skinLayer(rig, layer, child); });
    });
    connect(m_parts, &QListWidget::itemActivated, this, [this](QListWidgetItem* it) {
        if (m_view) m_view->setCurrentLayer(static_cast<LayerId>(it->data(Qt::UserRole).toUInt()));
    });
    connect(m_chains, &QListWidget::currentRowChanged, this, [this] {
        if (!m_filling) showChain();
    });
    connect(m_addChain, &QPushButton::clicked, this, [this] {
        const BoneId id = selectedBone();
        const Rig* r = m_view && m_view->document().rig() ? &*m_view->document().rig() : nullptr;
        const Bone* tip = r ? r->bone(id) : nullptr;
        if (!tip) return;
        const std::string name = tip->name + " IK";
        edit(tr("Add IK Chain"), [&](Rig& rig) { return rigs::addChain(rig, IkChain{name, id, m_bend->currentData().toInt()}); });
    });
    connect(m_removeChain, &QPushButton::clicked, this, [this] {
        const int c = selectedChain();
        if (c < 0) return;
        edit(tr("Remove IK Chain"), [&](Rig& rig) { return rigs::removeChain(rig, static_cast<std::size_t>(c)); });
    });
    connect(m_bend, &QComboBox::activated, this, [this] {
        const int c = selectedChain();
        if (m_filling || c < 0) return;
        const int bend = m_bend->currentData().toInt();
        edit(tr("Change IK Bend"), [&](Rig& rig) -> Status {
            rig.chains[static_cast<std::size_t>(c)].bend = bend;
            return rigs::validate(rig);
        });
    });
    connect(m_reach, &QPushButton::clicked, this, [this] {
        const int c = selectedChain();
        if (!m_view || !m_view->document().rig() || c < 0) return;
        if (auto st = rigs::reach(*m_view->document().rig(), m_pose, static_cast<std::size_t>(c), m_targetX->value(), m_targetY->value()); !st)
            return report(tr("Reach"), st.error());
        showBone();
        updateOverlay();
    });
    connect(m_turn, &QDoubleSpinBox::valueChanged, this, [this](double deg) {
        const BoneId id = selectedBone();
        if (m_filling || !id) return;
        m_pose.turn[id] = toRad(deg);
        updateOverlay();
    });
    for (QDoubleSpinBox* s : {m_moveX, m_moveY})
        connect(s, &QDoubleSpinBox::valueChanged, this, [this] {
            if (m_filling) return;
            m_pose.dx = m_moveX->value();
            m_pose.dy = m_moveY->value();
            updateOverlay();
        });
    connect(m_showBones, &QCheckBox::toggled, this, &RigPanel::updateOverlay);
    connect(m_dragBones, &QCheckBox::toggled, this, &RigPanel::updateOverlay);
    connect(m_resetPose, &QPushButton::clicked, this, [this] {
        m_pose = {};
        showBone();
        updateOverlay();
    });
    connect(m_bake, &QPushButton::clicked, this, &RigPanel::bakePose);
    connect(m_restFrame, &QSpinBox::valueChanged, this, [this](int v) {
        if (m_filling || !m_view || !m_view->document().rig() || m_view->document().rig()->restFrame == v - 1) return;
        edit(tr("Set Rest Frame"), [&](Rig& r) { return rigs::setRestFrame(r, v - 1); });
    });
    connect(m_setKey, &QPushButton::clicked, this, &RigPanel::setKey);
    connect(m_deleteKey, &QPushButton::clicked, this, &RigPanel::deleteKey);
    connect(m_bakeKeys, &QPushButton::clicked, this, &RigPanel::bakeKeys);
    connect(m_keys, &QListWidget::currentRowChanged, this, [this] {
        if (!m_filling) showKeys();
    });
    connect(m_keys, &QListWidget::itemActivated, this, [this](QListWidgetItem* it) {
        if (m_view) m_view->setCurrentFrame(it->data(Qt::UserRole).toInt());
    });
    connect(m_curve, &QComboBox::activated, this, [this] {
        const int frame = selectedKeyFrame();
        if (m_filling || frame < 0) return;
        const auto type = static_cast<CurveType>(m_curve->currentData().toInt());
        edit(tr("Change Key Curve"), [&](Rig& r) -> Status {
            for (RigKey& k : r.keys)
                if (k.frame == frame) k.curve.type = type;
            return rigs::validate(r);
        });
    });
    connect(m_removeRig, &QPushButton::clicked, this, [this] {
        if (!m_view || !m_view->document().rig()) return;
        if (QMessageBox::question(this, tr("Remove Rig"), tr("Remove every bone and IK chain? The part layers stay. You can undo this.")) !=
            QMessageBox::Yes)
            return;
        if (auto st = m_view->editor().setRig(std::nullopt, tr("Remove Rig").toStdString()); !st) report(tr("Remove Rig"), st.error());
        m_pose = {};
    });
    rebuild();
}

void RigPanel::setView(DocumentView* view) {
    if (m_view) {
        disconnect(m_view, nullptr, this, nullptr);
        disconnect(m_view->canvas(), nullptr, this, nullptr);
        m_view->canvas()->setBonesInteractive(false);
        m_view->canvas()->setBones({});
    }
    m_view = view;
    m_pose = {};
    m_dragging = false;
    if (m_view) {
        connect(m_view->canvas(), &CanvasWidget::boneHandlePressed, this, &RigPanel::boneGrabbed);
        connect(m_view->canvas(), &CanvasWidget::boneHandleDragged, this, [this](QPointF at) { boneDragged(at); });
        connect(m_view->canvas(), &CanvasWidget::boneHandleReleased, this, &RigPanel::boneReleased);
        connect(m_view, &DocumentView::documentChanged, this, &RigPanel::rebuild);
        connect(m_view, &DocumentView::currentLayerChanged, this, &RigPanel::rebuild);
        connect(m_view, &DocumentView::currentFrameChanged, this, [this] {
            m_bake->setText(tr("Bake to Frame %1").arg(m_view ? m_view->currentFrame() + 1 : 1));
            loadKeyedPose();
        });
    }
    m_status->clear();
    rebuild();
}

BoneId RigPanel::selectedBone() const {
    const QTreeWidgetItem* it = m_tree->currentItem();
    return it ? static_cast<BoneId>(it->data(0, Qt::UserRole).toUInt()) : kNoBone;
}

int RigPanel::selectedChain() const {
    const Rig* r = m_view && m_view->document().rig() ? &*m_view->document().rig() : nullptr;
    const int row = m_chains->currentRow();
    return r && row >= 0 && row < static_cast<int>(r->chains.size()) ? row : -1;
}

void RigPanel::selectBone(BoneId id) {
    for (QTreeWidgetItemIterator it(m_tree); *it; ++it)
        if ((*it)->data(0, Qt::UserRole).toUInt() == id) {
            m_tree->setCurrentItem(*it);
            return;
        }
}

void RigPanel::rebuild() {
    const bool open = m_view != nullptr;
    const Rig* rig = open && m_view->document().rig() ? &*m_view->document().rig() : nullptr;
    // Forget pose turns of bones that are gone.
    if (!rig) m_pose = {};
    else
    {
        for (auto it = m_pose.turn.begin(); it != m_pose.turn.end();) it = rig->bone(it->first) ? std::next(it) : m_pose.turn.erase(it);
        for (auto it = m_pose.stretch.begin(); it != m_pose.stretch.end();) it = rig->bone(it->first) ? std::next(it) : m_pose.stretch.erase(it);
    }

    const BoneId keep = selectedBone();
    const int keepChain = m_chains->currentRow();
    {
        m_filling = true;
        const QSignalBlocker b1(m_tree), b2(m_chains);
        m_tree->clear();
        m_chains->clear();
        if (rig) {
            std::map<BoneId, QTreeWidgetItem*> items;
            for (const Bone& b : rig->bones) {
                QTreeWidgetItem* parent = b.parent == kNoBone ? nullptr : items[b.parent];
                auto* it = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_tree);
                QString label = qs(b.name);
                const auto parts = rig->parts(b.id);
                if (!parts.empty()) label += tr("  (%n part(s))", nullptr, static_cast<int>(parts.size()));
                if (b.locked) label += tr("  locked");
                it->setText(0, label);
                it->setData(0, Qt::UserRole, b.id);
                items[b.id] = it;
            }
            m_tree->expandAll();
            for (const IkChain& c : rig->chains) {
                const Bone* t = rig->bone(c.tip);
                m_chains->addItem(QStringLiteral("%1 → %2").arg(qs(c.name), t ? qs(t->name) : QStringLiteral("?")));
            }
        }
        m_filling = false;
    }
    if (keep) selectBone(keep);
    if (!m_tree->currentItem() && m_tree->topLevelItemCount() > 0) m_tree->setCurrentItem(m_tree->topLevelItem(0));
    if (keepChain >= 0 && keepChain < m_chains->count()) m_chains->setCurrentRow(keepChain);
    else if (m_chains->count() > 0) m_chains->setCurrentRow(0);

    m_autoRig->setEnabled(open && !rig);
    m_rigAs->setEnabled(open && !rig);
    m_addBone->setEnabled(open);
    m_removeBone->setEnabled(rig && selectedBone());
    m_removeRig->setEnabled(rig != nullptr);
    m_resetPose->setEnabled(rig != nullptr);
    m_bake->setEnabled(rig && !rig->bindings.empty());
    m_moveX->setEnabled(rig != nullptr);
    m_moveY->setEnabled(rig != nullptr);
    m_showBones->setEnabled(rig != nullptr);
    m_dragBones->setEnabled(rig != nullptr);
    m_restFrame->setEnabled(rig != nullptr);
    {
        const QSignalBlocker b(m_restFrame);
        m_restFrame->setRange(1, open ? m_view->document().frameCount() : 1);
        m_restFrame->setValue(rig ? rig->restFrame + 1 : 1);
    }
    if (open) m_bake->setText(tr("Bake to Frame %1").arg(m_view->currentFrame() + 1));
    showKeys();
    showBone();
    showChain();
    updateOverlay();
}

void RigPanel::showBone() {
    const Rig* rig = m_view && m_view->document().rig() ? &*m_view->document().rig() : nullptr;
    const BoneId id = selectedBone();
    const Bone* b = rig && id ? rig->bone(id) : nullptr;
    m_filling = true;   // every field's handler ignores changes made here
    for (QWidget* w : std::initializer_list<QWidget*>{m_name, m_parent, m_x, m_y, m_length, m_angle, m_moveChildren, m_limited, m_locked,
                                                      m_parts, m_bind, m_turn, m_addChain})
        w->setEnabled(b != nullptr);
    m_unbind->setEnabled(m_view && rig && rig->bindings.count(m_view->currentLayer()));
    // The current layer's bend: offered when it is bound and its bone has children.
    m_bendWith->clear();
    m_bendWith->addItem(tr("(none: rigid)"), 0u);
    if (m_view && rig) {
        const LayerId cur = m_view->currentLayer();
        if (const auto bound = rig->bindings.find(cur); bound != rig->bindings.end()) {
            for (const Bone& o : rig->bones)
                if (o.parent == bound->second) m_bendWith->addItem(qs(o.name), o.id);
            const auto sk = rig->skins.find(cur);
            m_bendWith->setCurrentIndex(std::max(0, m_bendWith->findData(sk == rig->skins.end() ? 0u : sk->second.bone)));
        }
    }
    m_bendWith->setEnabled(m_bendWith->count() > 1);
    m_parent->clear();
    m_parts->clear();
    if (b) {
        m_name->setText(qs(b->name));
        m_parent->addItem(tr("(none: a root)"), 0u);
        const auto below = rig->subtree(id);
        for (const Bone& o : rig->bones)
            if (std::find(below.begin(), below.end(), o.id) == below.end()) m_parent->addItem(qs(o.name), o.id);
        m_parent->setCurrentIndex(std::max(0, m_parent->findData(b->parent)));
        m_x->setValue(b->x);
        m_y->setValue(b->y);
        m_length->setValue(b->length);
        m_angle->setValue(toDeg(b->angle));
        m_limited->setChecked(b->limited);
        m_min->setValue(toDeg(b->minAngle));
        m_max->setValue(toDeg(b->maxAngle));
        m_locked->setChecked(b->locked);
        for (LayerId l : rig->parts(id)) {
            const Layer* layer = m_view->document().layer(l);
            QString label = layer ? qs(layer->name) : tr("Layer %1").arg(l);
            if (const auto sk = rig->skins.find(l); sk != rig->skins.end())
                if (const Bone* c = rig->bone(sk->second.bone)) label += tr(" (bends with %1)").arg(qs(c->name));
            auto* it = new QListWidgetItem(label);
            it->setData(Qt::UserRole, l);
            m_parts->addItem(it);
        }
        const auto t = m_pose.turn.find(id);
        m_turn->setValue(toDeg(rigs::clampTurn(*b, t == m_pose.turn.end() ? 0.0 : t->second)));
    } else {
        m_name->clear();
        m_turn->setValue(0);
    }
    m_min->setEnabled(b && b->limited);
    m_max->setEnabled(b && b->limited);
    m_moveX->setValue(m_pose.dx);
    m_moveY->setValue(m_pose.dy);
    m_filling = false;
}

void RigPanel::showChain() {
    const Rig* rig = m_view && m_view->document().rig() ? &*m_view->document().rig() : nullptr;
    const int c = selectedChain();
    const bool has = rig && c >= 0;
    for (QWidget* w : std::initializer_list<QWidget*>{m_removeChain, m_targetX, m_targetY, m_reach}) w->setEnabled(has);
    if (!has) return;
    m_filling = true;
    const IkChain& chain = rig->chains[static_cast<std::size_t>(c)];
    m_bend->setCurrentIndex(chain.bend > 0 ? 0 : 1);
    // Start the target where the chain's end is now.
    const auto world = rigs::solve(*rig, m_pose);
    for (std::size_t i = 0; i < rig->bones.size(); ++i)
        if (rig->bones[i].id == chain.tip) {
            const QSignalBlocker bx(m_targetX), by(m_targetY);
            m_targetX->setValue(world[i].tailX);
            m_targetY->setValue(world[i].tailY);
        }
    m_filling = false;
}

int RigPanel::selectedKeyFrame() const {
    const QListWidgetItem* it = m_keys->currentItem();
    return it ? it->data(Qt::UserRole).toInt() : -1;
}

void RigPanel::showKeys() {
    const Rig* rig = m_view && m_view->document().rig() ? &*m_view->document().rig() : nullptr;
    const int frame = m_view ? m_view->currentFrame() : -1;
    int keep = selectedKeyFrame();
    m_filling = true;
    {
        const QSignalBlocker b(m_keys);
        // Rebuild only when the keys changed, so the selection survives.
        bool same = rig && m_keys->count() == static_cast<int>(rig->keys.size());
        for (int i = 0; same && i < m_keys->count(); ++i) {
            const RigKey& k = rig->keys[static_cast<std::size_t>(i)];
            same = m_keys->item(i)->data(Qt::UserRole).toInt() == k.frame &&
                   m_keys->item(i)->data(Qt::UserRole + 1).toInt() == static_cast<int>(k.curve.type) &&
                   m_keys->item(i)->data(Qt::UserRole + 2).toInt() == rig->restFrame;
        }
        if (!same) {
            m_keys->clear();
            if (rig)
                for (const RigKey& k : rig->keys) {
                    QString text = tr("Frame %1 · %2").arg(k.frame + 1).arg(m_curve->itemText(m_curve->findData(static_cast<int>(k.curve.type))));
                    if (k.frame == rig->restFrame) text += tr("  (rest frame)");
                    auto* it = new QListWidgetItem(text);
                    it->setData(Qt::UserRole, k.frame);
                    it->setData(Qt::UserRole + 1, static_cast<int>(k.curve.type));
                    it->setData(Qt::UserRole + 2, rig->restFrame);
                    m_keys->addItem(it);
                }
            if (keep < 0) keep = frame;
            for (int i = 0; i < m_keys->count(); ++i)
                if (m_keys->item(i)->data(Qt::UserRole).toInt() == keep) m_keys->setCurrentRow(i);
        }
    }
    const int sel = selectedKeyFrame();
    if (rig && sel >= 0)
        for (const RigKey& k : rig->keys)
            if (k.frame == sel) m_curve->setCurrentIndex(std::max(0, m_curve->findData(static_cast<int>(k.curve.type))));
    m_filling = false;
    m_setKey->setEnabled(rig != nullptr);
    m_setKey->setText(m_view ? tr("Set Key on %1").arg(frame + 1) : tr("Set Key"));
    m_curve->setEnabled(rig != nullptr);
    m_deleteKey->setEnabled(rig && sel >= 0);
    m_bakeKeys->setEnabled(rig && !rig->keys.empty() && !rig->bindings.empty());
}

void RigPanel::setKey() {
    if (!m_view || !m_view->document().rig()) return;
    const int frame = m_view->currentFrame();
    const auto type = static_cast<CurveType>(m_curve->currentData().toInt());
    if (edit(tr("Set Key"), [&](Rig& r) {
            Curve curve;
            for (const RigKey& k : r.keys)
                if (k.frame == frame) curve = k.curve;   // keeps custom Bezier points
            curve.type = type;
            return rigs::setKey(r, frame, m_pose, curve);
        })) {
        m_status->setStyleSheet(QString());
        m_status->setText(tr("Keyed frame %1.").arg(frame + 1));
    }
    showKeys();
}

void RigPanel::deleteKey() {
    const int frame = selectedKeyFrame();
    if (frame < 0) return;
    edit(tr("Delete Key"), [&](Rig& r) { return rigs::removeKey(r, frame); });
}

void RigPanel::loadKeyedPose() {
    const Rig* rig = m_view && m_view->document().rig() ? &*m_view->document().rig() : nullptr;
    if (rig && !rig->keys.empty()) {
        m_pose = rigs::poseAt(*rig, m_view->currentFrame());
        showBone();
        showChain();
        updateOverlay();
    }
    {
        const QSignalBlocker b(m_keys);
        m_keys->setCurrentRow(-1);
        for (int i = 0; i < m_keys->count(); ++i)
            if (m_view && m_keys->item(i)->data(Qt::UserRole).toInt() == m_view->currentFrame()) m_keys->setCurrentRow(i);
    }
    showKeys();
}

void RigPanel::bakeKeys() {
    if (!m_view || !m_view->document().rig()) return;
    const Rig rig = *m_view->document().rig();
    QApplication::setOverrideCursor(Qt::WaitCursor);
    auto baked = rigs::bakeKeys(m_view->document());
    QApplication::restoreOverrideCursor();
    if (!baked) return report(tr("Bake Keys"), baked.error());
    if (auto st = m_view->editor().replaceDocument(std::move(baked).value(), tr("Bake Keys").toStdString()); !st)
        return report(tr("Bake Keys"), st.error());
    m_status->setStyleSheet(QString());
    m_status->setText(tr("Baked frames %1–%2 from rest frame %3.")
                          .arg(rig.keys.front().frame + 1)
                          .arg(rig.keys.back().frame + 1)
                          .arg(rig.restFrame + 1));
}

void RigPanel::updateOverlay() {
    if (!m_view) return;
    const Rig* rig = m_view->document().rig() ? &*m_view->document().rig() : nullptr;
    m_view->canvas()->setBonesInteractive(rig && m_showBones->isChecked() && m_dragBones->isChecked());
    if (!rig || !m_showBones->isChecked()) return m_view->canvas()->setBones({});
    const auto world = rigs::solve(*rig, m_pose);
    const BoneId sel = selectedBone();
    std::vector<BoneMark> marks;
    marks.reserve(world.size());
    for (std::size_t i = 0; i < world.size(); ++i)
        marks.push_back({QPointF(world[i].headX, world[i].headY), QPointF(world[i].tailX, world[i].tailY), rig->bones[i].id == sel});
    m_view->canvas()->setBones(std::move(marks));
}

void RigPanel::boneGrabbed(int index, bool tail, QPointF at, Qt::KeyboardModifiers mods) {
    const Rig* rig = m_view && m_view->document().rig() ? &*m_view->document().rig() : nullptr;
    m_dragging = false;
    if (!rig || index < 0 || index >= static_cast<int>(rig->bones.size())) return;
    const Bone& b = rig->bones[static_cast<std::size_t>(index)];
    m_dragBone = b.id;
    m_dragTail = tail;
    m_dragStart = at;
    m_dragPose = m_pose;
    m_dragRest = mods.testFlag(Qt::ControlModifier);
    selectBone(b.id);
    if (m_dragRest) {
        bool atRest = m_pose.dx == 0 && m_pose.dy == 0;
        for (const auto& kv : m_pose.turn) atRest = atRest && kv.second == 0;
        for (const auto& kv : m_pose.stretch) atRest = atRest && kv.second == 1;
        if (!atRest) {
            report(tr("Edit Bone"), Error{ErrorCode::InvalidArgument, tr("press Rest Pose before Ctrl+dragging the rest bones").toStdString()});
            return;
        }
    }
    m_dragging = true;
}

void RigPanel::boneDragged(QPointF at) {
    const Rig* rig = m_view && m_view->document().rig() ? &*m_view->document().rig() : nullptr;
    const Bone* b = rig && m_dragging ? rig->bone(m_dragBone) : nullptr;
    if (!b) return;
    const BoneId id = b->id;
    if (m_dragRest) {
        // Rest geometry, snapped to half pixels; one undo step per bone and field.
        auto snap = [](double v) { return std::round(v * 2) / 2; };
        double x = b->x, y = b->y, length = b->length, angle = b->angle;
        if (m_dragTail) {
            const double dx = snap(at.x()) - x, dy = snap(at.y()) - y;
            length = std::hypot(dx, dy);
            if (length > 0) angle = std::atan2(dy, dx);
        } else {
            x = snap(at.x());
            y = snap(at.y());
        }
        const bool children = m_moveChildren->isChecked();
        edit(tr("Edit Bone"), [&](Rig& r) { return rigs::setBoneGeometry(r, id, x, y, length, angle, children); }, kGeometryKey + id);
        return;
    }
    if (!m_dragTail) {
        // A root's joint moves the whole figure, by whole pixels as baking does.
        if (b->parent != kNoBone) return;
        m_pose.dx = m_dragPose.dx + std::round(at.x() - m_dragStart.x());
        m_pose.dy = m_dragPose.dy + std::round(at.y() - m_dragStart.y());
    } else {
        std::optional<std::size_t> chain;
        for (std::size_t c = 0; c < rig->chains.size(); ++c)
            if (rig->chains[c].tip == id) chain = c;
        if (chain) {
            if (auto st = rigs::reach(*rig, m_pose, *chain, at.x(), at.y()); !st) return report(tr("Reach"), st.error());
        } else {
            // Turn the bone so its tip points at the pointer.
            const auto world = rigs::solve(*rig, m_pose);
            std::size_t i = 0, p = rig->bones.size();
            for (std::size_t k = 0; k < rig->bones.size(); ++k) {
                if (rig->bones[k].id == id) i = k;
                if (rig->bones[k].id == b->parent) p = k;
            }
            const double want = std::atan2(at.y() - world[i].headY, at.x() - world[i].headX);
            const double parentTurn = p < world.size() ? world[p].turn : 0.0;
            const auto cur = m_pose.turn.find(id);
            const double now = cur == m_pose.turn.end() ? 0.0 : cur->second;
            const double turn = now + std::remainder(want - b->angle - parentTurn - now, 2 * kPi);
            m_pose.turn[id] = rigs::clampTurn(*b, turn);
        }
    }
    showBone();
    showChain();
    updateOverlay();
}

void RigPanel::boneReleased() {
    if (m_dragging && !m_dragRest) {
        m_status->setStyleSheet(QString());
        m_status->setText(tr("Posed. Set a key or bake to keep the pose."));
    }
    m_dragging = false;
}

bool RigPanel::edit(const QString& label, const std::function<Status(Rig&)>& fn, long long mergeKey) {
    if (!m_view) return false;
    Rig rig = m_view->document().rig() ? *m_view->document().rig() : Rig{};
    if (auto st = fn(rig); !st) {
        report(label, st.error());
        rebuild();   // put the fields back
        return false;
    }
    if (auto st = m_view->editor().setRig(std::move(rig), label.toStdString(), mergeKey); !st) {
        report(label, st.error());
        rebuild();
        return false;
    }
    m_status->clear();
    return true;
}

void RigPanel::report(const QString& what, const Error& e) {
    m_status->setStyleSheet(QStringLiteral("color: %1;").arg(themeTokens().danger.name()));
    m_status->setText(QStringLiteral("%1: %2").arg(what, qs(e.message)));
    Q_EMIT statusMessage(m_status->text());
}

void RigPanel::addBone() {
    if (!m_view) return;
    const Rig* rig = m_view->document().rig() ? &*m_view->document().rig() : nullptr;
    const BoneId parent = selectedBone();
    Bone b;
    b.parent = parent;
    b.length = std::max(4, std::min(m_view->document().width(), m_view->document().height()) / 6);
    b.angle = -kPi / 2;   // pointing up
    if (const Bone* p = rig && parent ? rig->bone(parent) : nullptr) {
        // From the parent's tail, carrying on in its direction.
        b.x = p->tailX();
        b.y = p->tailY();
        b.angle = p->angle;
    } else {
        b.x = m_view->document().width() / 2.0;
        b.y = m_view->document().height() * 0.75;
    }
    int n = rig ? static_cast<int>(rig->bones.size()) + 1 : 1;
    std::string name;
    do name = "bone" + std::to_string(n++);
    while (rig && rig->bone(name));
    b.name = name;
    BoneId made = kNoBone;
    if (edit(tr("Add Bone"), [&](Rig& r) -> Status {
            auto id = rigs::addBone(r, b);
            if (!id) return id.error();
            made = id.value();
            return {};
        }))
        selectBone(made);
}

void RigPanel::removeBone() {
    const BoneId id = selectedBone();
    if (!id) return;
    edit(tr("Remove Bone"), [&](Rig& r) { return rigs::removeBone(r, id); });
}

void RigPanel::applyGeometry() {
    const BoneId id = selectedBone();
    if (m_filling || !id) return;
    edit(tr("Edit Bone"),
         [&](Rig& r) {
             return rigs::setBoneGeometry(r, id, m_x->value(), m_y->value(), m_length->value(), toRad(m_angle->value()), m_moveChildren->isChecked());
         },
         kGeometryKey + id);
}

void RigPanel::applyLimits() {
    const BoneId id = selectedBone();
    if (m_filling || !id) return;
    m_min->setEnabled(m_limited->isChecked());
    m_max->setEnabled(m_limited->isChecked());
    edit(tr("Bone Limits"), [&](Rig& r) { return rigs::setLimits(r, id, m_limited->isChecked(), toRad(m_min->value()), toRad(m_max->value())); },
         kLimitsKey + id);
}

void RigPanel::autoRigCurrentLayer() {
    if (!m_view) {
        QMessageBox::information(this, tr("Auto-Rig"), tr("Open a sprite first."));
        return;
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    auto r = rigs::autoRig(m_view->document(), m_view->currentLayer(), m_view->currentFrame(),
                           static_cast<rigs::AutoRigBody>(m_rigAs->currentData().toInt()));
    QApplication::restoreOverrideCursor();
    if (!r) {
        report(tr("Auto-Rig"), r.error());
        QMessageBox::warning(this, tr("Auto-Rig"), qs(r.error().message));
        return;
    }
    QStringList notes;
    for (const auto& n : r.value().notes) notes << qs(n);
    if (auto st = m_view->editor().replaceDocument(std::move(r.value().document), tr("Auto-Rig").toStdString()); !st) {
        report(tr("Auto-Rig"), st.error());
        return;
    }
    m_pose = {};
    m_status->setStyleSheet(QString());
    m_status->setText(notes.join(QLatin1Char('\n')));
    Q_EMIT statusMessage(tr("Auto-rigged: %1").arg(notes.value(1)));
}

void RigPanel::bakePose() {
    if (!m_view || !m_view->document().rig()) return;
    const int rest = m_view->document().rig()->restFrame;
    if (rest == m_view->currentFrame() &&
        QMessageBox::question(this, tr("Bake Pose"),
                              tr("This is the rest frame: baking here redraws the parts every later pose starts from. Bake anyway?")) !=
            QMessageBox::Yes)
        return;
    auto baked = rigs::bakePose(m_view->document(), rest, m_view->currentFrame(), m_pose);
    if (!baked) return report(tr("Bake Pose"), baked.error());
    if (auto st = m_view->editor().replaceDocument(std::move(baked).value(), tr("Bake Pose").toStdString()); !st)
        return report(tr("Bake Pose"), st.error());
    m_status->setStyleSheet(QString());
    m_status->setText(tr("Pose baked into frame %1 from rest frame %2.").arg(m_view->currentFrame() + 1).arg(rest + 1));
}

} // namespace acestudio::ui
