#pragma once

#include <QPointer>
#include <QWidget>

class QLabel;

namespace acestudio::ui {

class DocumentView;
class SwatchGrid;
class ToolState;

// Document palette. Left-click a swatch: primary colour; right-click:
// secondary. Double-click edits the entry; "Add" appends the primary
// colour; "Remove" deletes the selected entry. All edits are undoable.
class PalettePanel : public QWidget {
    Q_OBJECT
public:
    PalettePanel(ToolState* tools, QWidget* parent = nullptr);
    void setView(DocumentView* view);

private:
    void rebuild();

    QPointer<DocumentView> m_view;
    ToolState* m_tools;
    int m_selectedIndex = -1;
    QLabel* m_name = nullptr;
    QLabel* m_selected = nullptr;
    SwatchGrid* m_grid = nullptr;
};

} // namespace acestudio::ui
