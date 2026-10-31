#include "PalettePanel.h"
#include "DocumentView.h"
#include "QtBridge.h"
#include "ToolState.h"

#include <QColorDialog>
#include <QFileDialog>
#include <QMessageBox>
#include "acestudio/core/PaletteIO.h"
#include <QHBoxLayout>
#include <QPushButton>
#include <QFileInfo>

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
    void selected(int index, Qt::MouseButton button);
    void activated(int index);

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
        if (i >= 0) { m_sel = i; update(); Q_EMIT selected(i, e->button()); }
    }
    void mouseDoubleClickEvent(QMouseEvent* e) override {
        const int i = indexAt(e->position().toPoint());
        if (i >= 0 && e->button() == Qt::LeftButton) Q_EMIT activated(i);
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

PalettePanel::PalettePanel(ToolState* tools, QWidget* parent) : QWidget(parent), m_tools(tools) {
    m_name = new QLabel(this);
    m_grid = new SwatchGrid(this);
    m_selected = new QLabel(tr("Left-click: primary · Right-click: secondary · Double-click: edit"), this);
    m_selected->setWordWrap(true);
    m_selected->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_selected->setEnabled(false);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->addWidget(m_name);
    layout->addWidget(m_grid);
    layout->addWidget(m_selected);
    auto* row = new QHBoxLayout;
    auto* add = new QPushButton(tr("Add Primary"), this);
    auto* remove = new QPushButton(tr("Remove"), this);
    row->addWidget(add);
    row->addWidget(remove);
    row->addStretch();
    layout->addLayout(row);
    auto* fileRow = new QHBoxLayout;
    auto* load = new QPushButton(tr("Load…"), this);
    auto* saveBtn = new QPushButton(tr("Save…"), this);
    auto* extract = new QPushButton(tr("From Image"), this);
    extract->setToolTip(tr("Replace the palette with the colours of the current frame (median cut above 256)"));
    fileRow->addWidget(load);
    fileRow->addWidget(saveBtn);
    fileRow->addWidget(extract);
    fileRow->addStretch();
    layout->addLayout(fileRow);
    const QString filter = tr("Palettes (*.gpl *.pal *.hex *.txt *.ase);;GIMP (*.gpl);;JASC-PAL (*.pal);;Hex list (*.hex);;Paint.NET (*.txt);;Adobe Swatch Exchange (*.ase)");
    connect(load, &QPushButton::clicked, this, [this, filter] {
        if (!m_view) return;
        const QString path = QFileDialog::getOpenFileName(this, tr("Load Palette"), QString(), filter);
        if (path.isEmpty()) return;
        auto p = loadPaletteFile(toPath(path));
        if (!p) { QMessageBox::warning(this, tr("Load Palette"), qs(p.error().message)); return; }
        auto st = m_view->editor().replacePalette(std::move(p).value(), "Load Palette");
        if (!st) Q_EMIT m_view->statusMessage(qs(st.error().message));
    });
    connect(saveBtn, &QPushButton::clicked, this, [this, filter] {
        if (!m_view) return;
        QString path = QFileDialog::getSaveFileName(this, tr("Save Palette"), qs(m_view->document().palette().name()) + QStringLiteral(".gpl"), filter);
        if (path.isEmpty()) return;
        if (QFileInfo(path).suffix().isEmpty()) path += QStringLiteral(".gpl");
        if (auto st = savePaletteFile(m_view->document().palette(), toPath(path)); !st)
            QMessageBox::warning(this, tr("Save Palette"), qs(st.error().message));
    });
    connect(extract, &QPushButton::clicked, this, [this] {
        if (!m_view) return;
        Palette p = extractPalette(m_view->document().composite(m_view->currentFrame()), 256);
        if (p.empty()) { Q_EMIT m_view->statusMessage(tr("The frame has no opaque pixels")); return; }
        auto st = m_view->editor().replacePalette(std::move(p), "Palette from Image");
        if (!st) Q_EMIT m_view->statusMessage(qs(st.error().message));
    });
    layout->addStretch();

    connect(add, &QPushButton::clicked, this, [this] {
        if (!m_view) return;
        const QColor c = m_tools->primary();
        auto st = m_view->editor().addPaletteColor({static_cast<std::uint8_t>(c.red()), static_cast<std::uint8_t>(c.green()),
                                                     static_cast<std::uint8_t>(c.blue()), static_cast<std::uint8_t>(c.alpha())});
        if (!st) Q_EMIT m_view->statusMessage(qs(st.error().message));
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        if (!m_view || m_selectedIndex < 0) return;
        auto st = m_view->editor().removePaletteColor(static_cast<std::size_t>(m_selectedIndex));
        if (!st) Q_EMIT m_view->statusMessage(qs(st.error().message));
        m_selectedIndex = -1;
    });
    connect(m_grid, &SwatchGrid::activated, this, [this](int i) {
        if (!m_view) return;
        const Rgba8 old = m_view->document().palette().at(static_cast<std::size_t>(i));
        const QColor c = QColorDialog::getColor(toQColor(old), this, tr("Edit Palette Color"), QColorDialog::ShowAlphaChannel);
        if (!c.isValid()) return;
        auto st = m_view->editor().setPaletteColor(static_cast<std::size_t>(i),
            {static_cast<std::uint8_t>(c.red()), static_cast<std::uint8_t>(c.green()),
             static_cast<std::uint8_t>(c.blue()), static_cast<std::uint8_t>(c.alpha())});
        if (!st) Q_EMIT m_view->statusMessage(qs(st.error().message));
    });

    connect(m_grid, &SwatchGrid::selected, this, [this](int i, Qt::MouseButton button) {
        if (!m_view) return;
        m_selectedIndex = i;
        const Rgba8 c = m_view->document().palette().at(static_cast<std::size_t>(i));
        if (button == Qt::RightButton) m_tools->setSecondary(toQColor(c));
        else m_tools->setPrimary(toQColor(c));
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
