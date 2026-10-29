#pragma once

#include <QPointer>
#include <QWidget>

class QLabel;

namespace acestudio::ui {

class DocumentView;
class SwatchGrid;

// Shows the document palette. Selecting a swatch shows its value;
// palette editing arrives in Phase 2 (Color & Palette Management).
class PalettePanel : public QWidget {
    Q_OBJECT
public:
    explicit PalettePanel(QWidget* parent = nullptr);
    void setView(DocumentView* view);

private:
    void rebuild();

    QPointer<DocumentView> m_view;
    QLabel* m_name = nullptr;
    QLabel* m_selected = nullptr;
    SwatchGrid* m_grid = nullptr;
};

} // namespace acestudio::ui
