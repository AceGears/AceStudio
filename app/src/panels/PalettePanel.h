#pragma once

#include <QPointer>
#include <QWidget>

class QCheckBox;
class QLabel;
class QPushButton;

namespace acestudio::ui {

class DocumentView;
class SwatchGrid;
class ToolState;

// Document palette. Left-click a swatch: primary colour; right-click:
// secondary. Double-click edits the entry; "Add" appends the primary
// colour; "Remove" deletes the selected entry. All edits are undoable.
// "Lock palette" (ADR-ANIM-006) refuses palette edits and makes new pixels
// snap to the palette; "Conform" snaps every cel to it.
class PalettePanel : public QWidget {
    Q_OBJECT
public:
    PalettePanel(ToolState* tools, QWidget* parent = nullptr);
    void setView(DocumentView* view);

private:
    void rebuild();
    void toggleLock(bool locked);
    void conform();

    QPointer<DocumentView> m_view;
    ToolState* m_tools;
    int m_selectedIndex = -1;
    QLabel* m_name = nullptr;
    QLabel* m_selected = nullptr;
    SwatchGrid* m_grid = nullptr;
    QCheckBox* m_lock = nullptr;
    QPushButton* m_conform = nullptr;
    QList<QPushButton*> m_editButtons;   // disabled while the palette is locked
};

} // namespace acestudio::ui
