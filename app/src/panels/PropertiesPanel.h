#pragma once

#include <QPointer>
#include <QWidget>

class QFormLayout;

namespace acestudio::ui {

class DocumentView;

// Read-only summary of the active document.
class PropertiesPanel : public QWidget {
    Q_OBJECT
public:
    explicit PropertiesPanel(QWidget* parent = nullptr);
    void setView(DocumentView* view);

private:
    void rebuild();

    QPointer<DocumentView> m_view;
    QFormLayout* m_form = nullptr;
};

} // namespace acestudio::ui
