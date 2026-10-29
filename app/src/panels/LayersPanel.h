#pragma once

#include <QPointer>
#include <QWidget>

class QTreeWidget;
class QTreeWidgetItem;

namespace acestudio::ui {

class DocumentView;

// Layer stack (top layer first). Visibility checkboxes are live; creating,
// renaming and reordering layers arrive with the Phase 2 editing commands.
class LayersPanel : public QWidget {
    Q_OBJECT
public:
    explicit LayersPanel(QWidget* parent = nullptr);
    void setView(DocumentView* view);

private:
    void rebuild();
    void onItemChanged(QTreeWidgetItem* item, int column);

    QPointer<DocumentView> m_view;
    QTreeWidget* m_tree = nullptr;
    bool m_rebuilding = false;
};

} // namespace acestudio::ui
