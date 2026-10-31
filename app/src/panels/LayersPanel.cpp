#include "LayersPanel.h"

#include <QTimer>
#include "DocumentView.h"
#include "QtBridge.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMap>
#include <QSlider>
#include <QEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace acestudio::ui {

namespace {
enum class LayerGlyph { NewLayer, NewGroup, Duplicate, Merge, Up, Down, Delete };

// Small monochrome glyphs drawn in code (16×16 logical px, 2× for high DPI).
QIcon layerIcon(LayerGlyph g, const QColor& fg) {
    QPixmap pm(32, 32);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(2, 2);
    p.setPen(QPen(fg, 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    switch (g) {
    case LayerGlyph::NewLayer:
        p.drawRect(QRectF(2.5, 3.5, 9, 9));
        p.drawLine(QPointF(12, 1.5), QPointF(12, 6.5));
        p.drawLine(QPointF(9.5, 4), QPointF(14.5, 4));
        break;
    case LayerGlyph::NewGroup: {
        QPainterPath folder;
        folder.moveTo(2, 4); folder.lineTo(6, 4); folder.lineTo(7.5, 5.5); folder.lineTo(14, 5.5);
        folder.lineTo(14, 13); folder.lineTo(2, 13); folder.closeSubpath();
        p.drawPath(folder);
        break;
    }
    case LayerGlyph::Duplicate:
        p.drawRect(QRectF(2.5, 5.5, 8, 8));
        p.drawPolyline(QPolygonF({QPointF(5.5, 5.5), QPointF(5.5, 2.5), QPointF(13.5, 2.5), QPointF(13.5, 10.5), QPointF(10.5, 10.5)}));
        break;
    case LayerGlyph::Merge:
        p.drawLine(QPointF(8, 2), QPointF(8, 10));
        p.drawPolyline(QPolygonF({QPointF(5, 7), QPointF(8, 10), QPointF(11, 7)}));
        p.drawLine(QPointF(3, 13.5), QPointF(13, 13.5));
        break;
    case LayerGlyph::Up: p.drawPolyline(QPolygonF({QPointF(4, 10), QPointF(8, 6), QPointF(12, 10)})); break;
    case LayerGlyph::Down: p.drawPolyline(QPolygonF({QPointF(4, 6), QPointF(8, 10), QPointF(12, 6)})); break;
    case LayerGlyph::Delete:
        p.drawLine(QPointF(3, 4.5), QPointF(13, 4.5));
        p.drawLine(QPointF(6.5, 4.5), QPointF(6.5, 2.5));
        p.drawLine(QPointF(6.5, 2.5), QPointF(9.5, 2.5));
        p.drawLine(QPointF(9.5, 2.5), QPointF(9.5, 4.5));
        p.drawPolyline(QPolygonF({QPointF(4.5, 4.5), QPointF(5.5, 13.5), QPointF(10.5, 13.5), QPointF(11.5, 4.5)}));
        break;
    }
    pm.setDevicePixelRatio(2.0);
    return QIcon(pm);
}
} // namespace

namespace {
constexpr int kIdRole = Qt::UserRole + 1;
const BlendMode kModes[] = {BlendMode::Normal,  BlendMode::Multiply, BlendMode::Screen,
                            BlendMode::Overlay, BlendMode::Darken,   BlendMode::Lighten,
                            BlendMode::Add,     BlendMode::Subtract, BlendMode::Difference};
} // namespace

LayersPanel::LayersPanel(QWidget* parent) : QWidget(parent) {
    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(2);
    m_tree->setHeaderLabels({tr("Layer"), tr("Mode")});
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->setUniformRowHeights(true);
    m_tree->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    connect(m_tree, &QTreeWidget::itemChanged, this, &LayersPanel::onItemChanged);
    connect(m_tree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* cur) {
        if (m_rebuilding || !m_view || !cur) return;
        m_view->setCurrentLayer(cur->data(0, kIdRole).toUInt());
    });

    // Properties of the current layer
    m_opacity = new QSlider(Qt::Horizontal, this);
    m_opacity->setRange(0, 255);
    m_opacityLabel = new QLabel(this);
    m_opacityLabel->setMinimumWidth(36);
    m_blend = new QComboBox(this);
    for (BlendMode m : kModes) {
        QString name = qs(std::string(toString(m)));
        name[0] = name[0].toUpper();
        m_blend->addItem(name, static_cast<int>(m));
    }
    m_lock = new QCheckBox(tr("Lock"), this);
    m_alphaLock = new QCheckBox(tr("Lock transparency"), this);

    connect(m_opacity, &QSlider::valueChanged, this, [this](int v) {
        m_opacityLabel->setText(QStringLiteral("%1%").arg(qRound(v * 100.0 / 255.0)));
        if (m_rebuilding || !m_view) return;
        report(m_view->editor().setLayerOpacity(m_view->currentLayer(), static_cast<std::uint8_t>(v), true));
    });
    connect(m_blend, &QComboBox::currentIndexChanged, this, [this] {
        if (m_rebuilding || !m_view) return;
        report(m_view->editor().setLayerBlend(m_view->currentLayer(), static_cast<BlendMode>(m_blend->currentData().toInt())));
    });
    connect(m_lock, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_rebuilding && m_view) report(m_view->editor().setLayerLocked(m_view->currentLayer(), on));
    });
    connect(m_alphaLock, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_rebuilding && m_view) report(m_view->editor().setLayerAlphaLocked(m_view->currentLayer(), on));
    });

    // Layer actions
    auto makeButton = [this](LayerGlyph glyph, const QString& tip, auto fn) {
        auto* b = new QToolButton(this);
        b->setIcon(layerIcon(glyph, palette().color(QPalette::WindowText)));
        b->setIconSize(QSize(16, 16));
        b->setProperty("glyph", static_cast<int>(glyph));
        b->setToolTip(tip);
        b->setAccessibleName(tip);
        b->setAutoRaise(true);
        connect(b, &QToolButton::clicked, this, fn);
        m_layerButtons << b;
        return b;
    };
    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(1);
    buttons->addWidget(makeButton(LayerGlyph::NewLayer, tr("New layer (Shift+Ctrl+N)"), [this] {
        if (!m_view) return;
        auto r = m_view->editor().addLayer("", LayerKind::Raster, m_view->currentLayer());
        if (r) m_view->setCurrentLayer(r.value()); else report(r.error());
    }));
    buttons->addWidget(makeButton(LayerGlyph::NewGroup, tr("New group"), [this] {
        if (!m_view) return;
        auto r = m_view->editor().addLayer("", LayerKind::Group, m_view->currentLayer());
        if (r) m_view->setCurrentLayer(r.value()); else report(r.error());
    }));
    buttons->addWidget(makeButton(LayerGlyph::Duplicate, tr("Duplicate layer (Ctrl+J)"), [this] {
        if (!m_view) return;
        auto r = m_view->editor().duplicateLayer(m_view->currentLayer());
        if (r) m_view->setCurrentLayer(r.value()); else report(r.error());
    }));
    m_mergeButton = makeButton(LayerGlyph::Merge, tr("Merge down into the layer below (Ctrl+E)"), [this] {
        if (m_view) report(m_view->editor().mergeDown(m_view->currentLayer()));
    });
    buttons->addWidget(m_mergeButton);
    buttons->addWidget(makeButton(LayerGlyph::Up, tr("Move up"), [this] { moveCurrent(+1); }));
    buttons->addWidget(makeButton(LayerGlyph::Down, tr("Move down"), [this] { moveCurrent(-1); }));
    buttons->addWidget(makeButton(LayerGlyph::Delete, tr("Delete layer"), [this] {
        if (m_view) report(m_view->editor().removeLayer(m_view->currentLayer()));
    }));

    auto* opacityRow = new QHBoxLayout;
    opacityRow->addWidget(new QLabel(tr("Opacity"), this));
    opacityRow->addWidget(m_opacity, 1);
    opacityRow->addWidget(m_opacityLabel);
    auto* blendRow = new QHBoxLayout;
    blendRow->addWidget(new QLabel(tr("Blend"), this));
    blendRow->addWidget(m_blend, 1);
    auto* lockRow = new QHBoxLayout;
    lockRow->addWidget(m_lock);
    lockRow->addWidget(m_alphaLock);
    lockRow->addStretch();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(4);
    layout->addLayout(opacityRow);
    layout->addLayout(blendRow);
    layout->addLayout(lockRow);
    layout->addWidget(m_tree, 1);
    layout->addLayout(buttons);
    buttons->addStretch();
    rebuild();
}

void LayersPanel::setView(DocumentView* view) {
    if (m_view) disconnect(m_view, nullptr, this, nullptr);
    m_view = view;
    if (m_view) {
        // Deferred: these fire from inside the tree's own signals (current
        // item changed, check box toggled); clearing the tree there deleted
        // the item Qt was still delivering the signal for (use-after-free).
        connect(m_view, &DocumentView::documentChanged, this, &LayersPanel::scheduleRebuild);
        connect(m_view, &DocumentView::currentLayerChanged, this, &LayersPanel::scheduleRebuild);
        connect(m_view, &DocumentView::editingMaskChanged, this, &LayersPanel::scheduleRebuild);
    }
    rebuild();
}

void LayersPanel::scheduleRebuild() {
    if (m_rebuildPending) return;
    m_rebuildPending = true;
    QTimer::singleShot(0, this, [this] {
        m_rebuildPending = false;
        rebuild();
    });
}

void LayersPanel::report(const Status& st) {
    if (!st && m_view) Q_EMIT m_view->statusMessage(qs(st.error().message));
}

void LayersPanel::moveCurrent(int delta) {
    if (!m_view) return;
    const auto idx = m_view->document().layerIndex(m_view->currentLayer());
    if (!idx) return;
    const long target = static_cast<long>(*idx) + delta;
    if (target < 0 || target >= static_cast<long>(m_view->document().layers().size())) return;
    report(m_view->editor().moveLayer(m_view->currentLayer(), static_cast<std::size_t>(target)));
}

void LayersPanel::rebuild() {
    m_rebuilding = true;
    m_tree->clear();
    QTreeWidgetItem* currentItem = nullptr;
    if (m_view) {
        const auto& layers = m_view->document().layers();
        QMap<LayerId, QTreeWidgetItem*> items;
        for (const Layer& l : layers) {
            auto* item = new QTreeWidgetItem;
            item->setText(0, qs(l.name));
            QString mode = l.kind == LayerKind::Adjustment ? tr("adjust") : qs(std::string(toString(l.blend)));
            if (l.opacity != 255) mode += QStringLiteral(" · %1%").arg(qRound(l.opacity * 100.0 / 255.0));
            if (l.locked) mode += tr(" · locked");
            QStringList flags;
            if (l.clipped) flags << tr("↳ clip");
            if (l.hasMask) {
                const bool editing = l.id == m_view->currentLayer() && m_view->editingMask();
                flags << (editing ? tr("■ MASK") : l.maskEnabled ? tr("□ mask") : tr("⊘ mask"));
            }
            if (!flags.isEmpty()) mode = flags.join(QStringLiteral(" ")) + QStringLiteral(" · ") + mode;
            item->setText(1, mode);
            if (l.kind == LayerKind::Adjustment) {
                QFont f = item->font(0);
                f.setItalic(true);
                item->setFont(0, f);
                item->setToolTip(1, tr("Adjustment layer: changes the layers below it. Layer › Adjustment Settings to edit."));
            }
            if (l.hasMask) item->setToolTip(1, tr("Layer mask: Layer › Layer Mask › Edit Mask (Ctrl+\\) to paint on it"));
            item->setData(0, kIdRole, l.id);
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable | Qt::ItemIsEditable);
            item->setCheckState(0, l.visible ? Qt::Checked : Qt::Unchecked);
            item->setToolTip(0, tr("Checkbox: show/hide · Double-click: rename"));
            if (l.kind == LayerKind::Group) { QFont f = item->font(0); f.setBold(true); item->setFont(0, f); }
            items.insert(l.id, item);
            if (l.id == m_view->currentLayer()) currentItem = item;
        }
        for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
            QTreeWidgetItem* item = items.value(it->id);
            if (it->parent != kNoLayer && items.contains(it->parent)) items.value(it->parent)->addChild(item);
            else m_tree->addTopLevelItem(item);
        }
        m_tree->expandAll();
        if (currentItem) m_tree->setCurrentItem(currentItem);
    }
    m_rebuilding = false;
    syncControls();
    setEnabled(m_view != nullptr);
}

void LayersPanel::syncControls() {
    m_rebuilding = true;
    const Layer* l = m_view ? m_view->document().layer(m_view->currentLayer()) : nullptr;
    for (QWidget* w : std::initializer_list<QWidget*>{m_opacity, m_blend, m_lock, m_alphaLock}) w->setEnabled(l != nullptr);
    if (l) {
        m_opacity->setValue(l->opacity);
        m_blend->setCurrentIndex(m_blend->findData(static_cast<int>(l->blend)));
        m_lock->setChecked(l->locked);
        m_alphaLock->setChecked(l->alphaLocked);
        m_alphaLock->setEnabled(l->kind == LayerKind::Raster);
    }
    m_opacityLabel->setText(QStringLiteral("%1%").arg(qRound(m_opacity->value() * 100.0 / 255.0)));
    std::string why;
    const bool canMerge = l && m_view->editor().canMergeDown(l->id, &why);
    m_mergeButton->setEnabled(canMerge);
    m_mergeButton->setToolTip(canMerge ? tr("Merge down into the layer below (Ctrl+E)") : QString::fromStdString(why));
    m_rebuilding = false;
}

void LayersPanel::onItemChanged(QTreeWidgetItem* item, int column) {
    if (m_rebuilding || !m_view || column != 0) return;
    const LayerId id = item->data(0, kIdRole).toUInt();
    const Layer* l = m_view->document().layer(id);
    if (!l) return;
    const bool visible = item->checkState(0) == Qt::Checked;
    if (l->visible != visible) { report(m_view->editor().setLayerVisible(id, visible)); return; }
    const std::string name = item->text(0).trimmed().toStdString();
    if (name != l->name) {
        const Status st = m_view->editor().renameLayer(id, name);
        if (!st) { report(st); rebuild(); }
    }
}

void LayersPanel::changeEvent(QEvent* e) {
    QWidget::changeEvent(e);
    if (e->type() == QEvent::PaletteChange)   // theme switch: redraw glyphs in the new text colour
        for (QToolButton* b : std::as_const(m_layerButtons))
            b->setIcon(layerIcon(static_cast<LayerGlyph>(b->property("glyph").toInt()), palette().color(QPalette::WindowText)));
}

} // namespace acestudio::ui
