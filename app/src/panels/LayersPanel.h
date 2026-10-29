#pragma once

#include "acestudio/core/Document.h"

#include <QPointer>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QSlider;
class QLabel;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace acestudio::ui {

class DocumentView;

// Layer stack (top layer first). Every change goes through the Editor and
// is undoable: visibility, rename (double-click), opacity, blend, locks,
// add/duplicate/merge/delete and reordering.
class LayersPanel : public QWidget {
    Q_OBJECT
public:
    explicit LayersPanel(QWidget* parent = nullptr);
    void setView(DocumentView* view);

private:
    void rebuild();
    void syncControls();
    void onItemChanged(QTreeWidgetItem* item, int column);
    void report(const Status& st);
    void moveCurrent(int delta);

    QPointer<DocumentView> m_view;
    QTreeWidget* m_tree = nullptr;
    QSlider* m_opacity = nullptr;
    QLabel* m_opacityLabel = nullptr;
    QComboBox* m_blend = nullptr;
    QCheckBox* m_lock = nullptr;
    QCheckBox* m_alphaLock = nullptr;
    QList<QToolButton*> m_layerButtons;
    bool m_rebuilding = false;
};

} // namespace acestudio::ui
