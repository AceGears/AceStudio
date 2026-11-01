#pragma once
// The vertical workspace switcher on the left of the main window: the logo,
// one button per workspace (icon over label), and an accent bar that glides
// to the selected workspace. The buttons are ordinary focusable, checkable
// tool buttons, so screen readers and the keyboard (Tab, Space, Ctrl+1..0)
// work like everywhere else.

#include "Icons.h"

#include <QWidget>

#include <vector>

class QButtonGroup;
class QToolButton;
class QVariantAnimation;
class QVBoxLayout;

namespace acestudio::ui {

class WorkspaceRail : public QWidget {
    Q_OBJECT
public:
    explicit WorkspaceRail(QWidget* parent = nullptr);

    // Adds a workspace button; returns its index.
    int addWorkspace(Glyph glyph, const QString& label, const QString& toolTip);
    void addStretch();
    int currentIndex() const { return m_current; }
    void setCurrentIndex(int index);
    QToolButton* button(int index) const;

Q_SIGNALS:
    void currentChanged(int index);

protected:
    void paintEvent(QPaintEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    void moveIndicator(bool animate);

    QVBoxLayout* m_layout = nullptr;
    QButtonGroup* m_group = nullptr;
    std::vector<QToolButton*> m_buttons;
    int m_current = -1;
    qreal m_indicatorY = -1;
    qreal m_indicatorH = 0;
    QVariantAnimation* m_anim = nullptr;
};

} // namespace acestudio::ui
