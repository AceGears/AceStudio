#include "LayersPanel.h"
#include "DocumentView.h"
#include "QtBridge.h"

#include <QHeaderView>
#include <QLabel>
#include <QMap>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace acestudio::ui {

namespace {
constexpr int kIdRole = Qt::UserRole + 1;
}

LayersPanel::LayersPanel(QWidget* parent) : QWidget(parent) {
    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(2);
    m_tree->setHeaderLabels({tr("Layer"), tr("Mode")});
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->setRootIsDecorated(true);
    m_tree->setUniformRowHeights(true);
    connect(m_tree, &QTreeWidget::itemChanged, this, &LayersPanel::onItemChanged);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->addWidget(m_tree);
    rebuild();
}

void LayersPanel::setView(DocumentView* view) {
    if (m_view) disconnect(m_view, nullptr, this, nullptr);
    m_view = view;
    if (m_view) connect(m_view, &DocumentView::documentChanged, this, &LayersPanel::rebuild);
    rebuild();
}

void LayersPanel::rebuild() {
    m_rebuilding = true;
    m_tree->clear();
    if (m_view) {
        const auto& layers = m_view->document().layers();
        QMap<LayerId, QTreeWidgetItem*> items;
        // Create items first, then attach, so parents may appear in any order.
        for (const Layer& l : layers) {
            auto* item = new QTreeWidgetItem;
            item->setText(0, qs(l.name));
            QString mode = qs(std::string(toString(l.blend)));
            if (l.opacity != 255) mode += QStringLiteral(" · %1%").arg(qRound(l.opacity * 100.0 / 255.0));
            if (l.locked) mode += tr(" · locked");
            item->setText(1, mode);
            item->setData(0, kIdRole, l.id);
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
            item->setCheckState(0, l.visible ? Qt::Checked : Qt::Unchecked);
            item->setToolTip(0, tr("Toggle the checkbox to show or hide this layer"));
            if (l.kind == LayerKind::Group) { QFont f = item->font(0); f.setBold(true); item->setFont(0, f); }
            items.insert(l.id, item);
        }
        // Layers are stored bottom->top; display top->bottom.
        for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
            QTreeWidgetItem* item = items.value(it->id);
            if (it->parent != kNoLayer && items.contains(it->parent)) items.value(it->parent)->addChild(item);
            else m_tree->addTopLevelItem(item);
        }
        m_tree->expandAll();
        if (layers.empty()) {
            auto* item = new QTreeWidgetItem(QStringList{tr("No layers")});
            item->setFlags(Qt::NoItemFlags);
            m_tree->addTopLevelItem(item);
        }
    }
    setEnabled(m_view != nullptr);
    m_rebuilding = false;
}

void LayersPanel::onItemChanged(QTreeWidgetItem* item, int column) {
    if (m_rebuilding || !m_view || column != 0) return;
    const auto id = item->data(0, kIdRole).toUInt();
    Layer* l = m_view->document().layer(id);
    if (!l) return;
    const bool visible = item->checkState(0) == Qt::Checked;
    if (l->visible == visible) return;
    l->visible = visible;
    m_view->notifyDocumentChanged();
}

} // namespace acestudio::ui
