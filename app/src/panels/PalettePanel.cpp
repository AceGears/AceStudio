#include "PalettePanel.h"
#include "DocumentView.h"
#include "QtBridge.h"

#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QVBoxLayout>

namespace acestudio::ui {

class SwatchGrid : public QWidget {
    Q_OBJECT
public:
    explicit SwatchGrid(QWidget* parent) : QWidget(parent) {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        setMouseTracking(true);
    }
    void setColors(QList<QColor> colors) { m_colors = std::move(colors); m_sel = -1; updateGeometry(); update(); }

    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override {
        const int perRow = std::max(1, (w - 2) / kCell);
        const int rows = (static_cast<int>(m_colors.size()) + perRow - 1) / perRow;
        return std::max(kCell, rows * kCell + 2);
    }
    QSize sizeHint() const override { return {8 * kCell, heightForWidth(8 * kCell)}; }

Q_SIGNALS:
    void selected(int index);

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        const int perRow = std::max(1, (width() - 2) / kCell);
        for (int i = 0; i < m_colors.size(); ++i) {
            const QRect r(1 + (i % perRow) * kCell, 1 + (i / perRow) * kCell, kCell - 2, kCell - 2);
            if (m_colors[i].alpha() < 255) {   // checker under translucent swatches
                p.fillRect(r, QColor("#777"));
                p.fillRect(QRect(r.topLeft(), r.size() / 2), QColor("#BBB"));
                p.fillRect(QRect(r.center() + QPoint(1, 1), r.size() / 2), QColor("#BBB"));
            }
            p.fillRect(r, m_colors[i]);
            p.setPen(i == m_sel ? palette().color(QPalette::Highlight) : palette().color(QPalette::Mid));
            p.drawRect(r.adjusted(0, 0, -1, -1));
        }
    }
    void mousePressEvent(QMouseEvent* e) override {
        const int i = indexAt(e->position().toPoint());
        if (i >= 0) { m_sel = i; update(); Q_EMIT selected(i); }
    }
    bool event(QEvent* e) override {
        if (e->type() == QEvent::ToolTip) {
            auto* he = static_cast<QHelpEvent*>(e);
            const int i = indexAt(he->pos());
            setToolTip(i >= 0 ? QStringLiteral("#%1  %2").arg(i).arg(m_colors[i].name(QColor::HexArgb).toUpper())
                              : QString());
        }
        return QWidget::event(e);
    }

private:
    int indexAt(QPoint pt) const {
        const int perRow = std::max(1, (width() - 2) / kCell);
        const int col = (pt.x() - 1) / kCell, row = (pt.y() - 1) / kCell;
        if (pt.x() < 1 || pt.y() < 1 || col >= perRow) return -1;
        const int i = row * perRow + col;
        return i < m_colors.size() ? i : -1;
    }
    static constexpr int kCell = 20;
    QList<QColor> m_colors;
    int m_sel = -1;
};

PalettePanel::PalettePanel(QWidget* parent) : QWidget(parent) {
    m_name = new QLabel(this);
    m_grid = new SwatchGrid(this);
    m_selected = new QLabel(tr("Click a swatch to inspect it."), this);
    m_selected->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_selected->setEnabled(false);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->addWidget(m_name);
    layout->addWidget(m_grid);
    layout->addWidget(m_selected);
    layout->addStretch();

    connect(m_grid, &SwatchGrid::selected, this, [this](int i) {
        if (!m_view) return;
        const Rgba8 c = m_view->document().palette().at(static_cast<std::size_t>(i));
        m_selected->setEnabled(true);
        m_selected->setText(tr("Index %1   %2   RGBA(%3, %4, %5, %6)")
                                .arg(i).arg(qs(c.toHex(true))).arg(c.r).arg(c.g).arg(c.b).arg(c.a));
    });
    rebuild();
}

void PalettePanel::setView(DocumentView* view) {
    if (m_view) disconnect(m_view, nullptr, this, nullptr);
    m_view = view;
    if (m_view) connect(m_view, &DocumentView::documentChanged, this, &PalettePanel::rebuild);
    rebuild();
}

void PalettePanel::rebuild() {
    QList<QColor> colors;
    if (m_view) {
        const auto& pal = m_view->document().palette();
        for (const auto& c : pal.colors()) colors << toQColor(c);
        m_name->setText(QStringLiteral("<b>%1</b> — %2").arg(qs(pal.name()).toHtmlEscaped())
                            .arg(tr("%n color(s)", nullptr, static_cast<int>(pal.size()))));
    } else {
        m_name->setText(tr("No document open"));
    }
    m_grid->setColors(colors);
    setEnabled(m_view != nullptr);
}

} // namespace acestudio::ui

#include "PalettePanel.moc"
