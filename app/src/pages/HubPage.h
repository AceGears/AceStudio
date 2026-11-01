#pragma once
// A workspace landing page: a heading and cards, each describing a tool and
// opening it. Cards that need an open document are disabled (with the reason
// in their tooltip) until one is open.

#include "../ui/Icons.h"

#include <QWidget>

#include <functional>
#include <vector>

class QGridLayout;
class QPushButton;
class QVBoxLayout;

namespace acestudio::ui {

struct HubCard {
    Glyph glyph;
    QString title;
    QString description;
    QString action;               // button text
    std::function<void()> run;
    bool needsDocument = false;
};

class HubPage : public QWidget {
    Q_OBJECT
public:
    HubPage(const QString& eyebrow, const QString& title, const QString& subtitle, QWidget* parent = nullptr);
    void addSection(const QString& title);
    void addCard(const HubCard& card);
    // Extra content under the cards (e.g. an embedded panel).
    void addWidget(QWidget* w, int stretch = 0);
    void setDocumentOpen(bool open);

private:
    QVBoxLayout* m_col = nullptr;
    QGridLayout* m_grid = nullptr;
    int m_count = 0;
    std::vector<std::pair<QPushButton*, bool>> m_buttons;
};

} // namespace acestudio::ui
