#include "WorldBuilderPage.h"

#include "../AppContext.h"
#include "../QtBridge.h"

#include "acestudio/core/PngCodec.h"
#include <nlohmann/json.hpp>
#include "acestudio/core/FileUtil.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QShortcut>
#include <QSlider>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace acestudio::ui {

using namespace acestudio::world;

// ------------------------------------------------------------------ canvas

class MapCanvas : public QWidget {
public:
    explicit MapCanvas(WorldBuilderPage& page, QWidget* parent) : QWidget(parent), m_page(page) {
        setMouseTracking(true);
        setFocusPolicy(Qt::ClickFocus);
        setAccessibleName(QObject::tr("Map canvas"));
    }
    void setImage(QImage img, int zoom, int cameraOffset) {
        m_image = std::move(img);
        m_zoom = std::max(1, zoom);
        m_cameraOffset = cameraOffset;
        setFixedSize(m_image.width() * m_zoom, m_image.height() * m_zoom);
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        // Checkerboard under transparent areas.
        const int s = 8;
        for (int y = 0; y < height(); y += s)
            for (int x = 0; x < width(); x += s)
                p.fillRect(x, y, s, s, ((x / s + y / s) % 2) ? QColor(58, 58, 64) : QColor(48, 48, 54));
        p.drawImage(rect(), m_image);
        if (m_hover.x() >= 0) {
            const int tw = m_page.m_map.tileW() * m_zoom, th = m_page.m_map.tileH() * m_zoom;
            const QPoint mp = mapPoint(m_hover);
            p.setPen(QPen(QColor(10, 108, 245), 1));
            p.drawRect((mp.x() / m_page.m_map.tileW()) * tw - m_cameraOffset * m_zoom, (mp.y() / m_page.m_map.tileH()) * th, tw - 1, th - 1);
        }
    }
    void mousePressEvent(QMouseEvent* e) override {
        const QPoint mp = mapPoint(e->position().toPoint());
        m_button = e->button();
        m_page.pressAt(mp.x(), mp.y(), m_button == Qt::LeftButton);
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        m_hover = e->position().toPoint();
        if (e->buttons() & (Qt::LeftButton | Qt::RightButton)) {
            const QPoint mp = mapPoint(m_hover);
            m_page.dragTo(mp.x(), mp.y(), m_button == Qt::LeftButton);
        }
        update();
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        const QPoint mp = mapPoint(e->position().toPoint());
        m_page.releaseAt(mp.x(), mp.y(), m_button == Qt::LeftButton);
    }
    void leaveEvent(QEvent*) override {
        m_hover = QPoint(-1, -1);
        update();
    }

private:
    QPoint mapPoint(QPoint w) const { return {w.x() / m_zoom + m_cameraOffset, w.y() / m_zoom}; }
    WorldBuilderPage& m_page;
    QImage m_image;
    int m_zoom = 2;
    int m_cameraOffset = 0;
    QPoint m_hover{-1, -1};
    Qt::MouseButton m_button = Qt::NoButton;
};

// ------------------------------------------------------------------ page

namespace {
QIcon tileIcon(const Image& img, int size) {
    const QImage q = toQImage(img);
    return QIcon(QPixmap::fromImage(q.scaled(size, size, Qt::KeepAspectRatio, Qt::FastTransformation)));
}
} // namespace

WorldBuilderPage::WorldBuilderPage(AppContext& ctx, ImageSource currentImage, QWidget* parent)
    : QWidget(parent), m_ctx(ctx), m_currentImage(std::move(currentImage)), m_map(WorldMap::create(40, 23, 16, 16).value()) {
    m_map.addLayer("Ground");
    m_map.addLayer("Detail");

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 10, 12, 10);

    // File and asset commands.
    auto* bar = new QHBoxLayout;
    auto button = [&](const QString& text, auto fn) {
        auto* b = new QPushButton(text, this);
        connect(b, &QPushButton::clicked, this, fn);
        bar->addWidget(b);
        return b;
    };
    button(tr("New Map…"), [this] { newMap(); });
    button(tr("Open…"), [this] { openMapDialog(); });
    button(tr("Save"), [this] { saveMap(false); });
    button(tr("Save As…"), [this] { saveMap(true); });
    button(tr("Export to Tiled…"), [this] { exportTiled(); })->setToolTip(
        tr("A .tmx map with its tile sheets and props, a collision layer and a preview PNG. Godot (with a Tiled importer), "
           "Unity (SuperTiled2Unity), GameMaker and Phaser read Tiled maps."));
    auto* add = new QToolButton(this);
    add->setText(tr("Add"));
    add->setPopupMode(QToolButton::InstantPopup);
    auto* addMenu = new QMenu(add);
    addMenu->addAction(tr("Tile Sheet from File…"), this, [this] { addSheet(false); });
    addMenu->addAction(tr("Tile Sheet from Open Document"), this, [this] { addSheet(true); });
    addMenu->addSeparator();
    addMenu->addAction(tr("Props from Files…"), this, [this] { addProp(false); });
    addMenu->addAction(tr("Prop from Open Document"), this, [this] { addProp(true); });
    addMenu->addSeparator();
    addMenu->addAction(tr("Parallax Background from File…"), this, [this] { addBackground(false); });
    addMenu->addAction(tr("Parallax Background from Open Document"), this, [this] { addBackground(true); });
    add->setMenu(addMenu);
    add->setAccessibleName(tr("Add assets"));
    bar->addWidget(add);
    bar->addStretch();
    root->addLayout(bar);

    auto* body = new QHBoxLayout;
    root->addLayout(body, 1);

    // Left: tools, layers, tiles, props.
    auto* left = new QVBoxLayout;
    body->addLayout(left, 0);
    auto* tools = new QHBoxLayout;
    auto* group = new QButtonGroup(this);
    const std::vector<std::pair<Tool, QString>> toolList = {
        {Tool::Paint, tr("Paint")},     {Tool::Erase, tr("Erase")}, {Tool::Fill, tr("Fill")},       {Tool::Rect, tr("Rect")},
        {Tool::Terrain, tr("Terrain")}, {Tool::Prop, tr("Prop")},   {Tool::Collision, tr("Solid")}, {Tool::Light, tr("Light")}};
    auto* toolGrid = new QGridLayout;
    for (std::size_t i = 0; i < toolList.size(); ++i) {
        auto* b = new QToolButton(this);
        b->setText(toolList[i].second);
        b->setCheckable(true);
        b->setAutoRaise(false);
        b->setToolButtonStyle(Qt::ToolButtonTextOnly);
        b->setMinimumWidth(56);
        group->addButton(b);
        toolGrid->addWidget(b, static_cast<int>(i) / 4, static_cast<int>(i) % 4);
        const Tool t = toolList[i].first;
        connect(b, &QToolButton::clicked, this, [this, t] { m_tool = t; m_rectStart.reset(); });
        m_toolButtons.push_back(b);
    }
    m_toolButtons.front()->setChecked(true);
    m_toolButtons[0]->setToolTip(tr("Paint the selected tile (right button erases)"));
    m_toolButtons[3]->setToolTip(tr("Drag a rectangle of the selected tile"));
    m_toolButtons[4]->setToolTip(tr("Paint terrain with a Tileset Generator sheet: the edge tiles are chosen for you "
                                     "(right button removes terrain)"));
    m_toolButtons[5]->setToolTip(tr("Place the selected prop; drag a prop to move it, right-click to remove it"));
    m_toolButtons[6]->setToolTip(tr("Mark cells solid for collision (right button clears)"));
    m_toolButtons[7]->setToolTip(tr("Add a light for the lighting preview (right-click removes the nearest)"));
    tools->addLayout(toolGrid);
    left->addLayout(tools);

    auto* layersLabel = new QLabel(tr("&Layers (top first):"), this);
    m_layers = new QListWidget(this);
    m_layers->setMaximumHeight(110);
    layersLabel->setBuddy(m_layers);
    left->addWidget(layersLabel);
    left->addWidget(m_layers);
    auto* layerButtons = new QHBoxLayout;
    auto lbtn = [&](const QString& text, const QString& tip, auto fn) {
        auto* b = new QToolButton(this);
        b->setText(text);
        b->setToolTip(tip);
        b->setAccessibleName(tip);
        connect(b, &QToolButton::clicked, this, fn);
        layerButtons->addWidget(b);
    };
    lbtn(tr("+"), tr("Add layer"), [this] {
        pushUndo();
        m_map.addLayer(tr("Layer %1").arg(m_map.layers.size() + 1).toStdString());
        refreshLayers();
        m_layers->setCurrentRow(0);
        rerender();
    });
    lbtn(tr("−"), tr("Delete layer"), [this] {
        const int l = currentLayer();
        if (l < 0 || m_map.layers.size() <= 1) return;
        pushUndo();
        m_map.layers.erase(m_map.layers.begin() + l);
        refreshLayers();
        rerender();
    });
    lbtn(tr("↑"), tr("Move layer up"), [this] {
        const int l = currentLayer();
        if (l < 0 || l + 1 >= static_cast<int>(m_map.layers.size())) return;
        pushUndo();
        std::swap(m_map.layers[static_cast<std::size_t>(l)], m_map.layers[static_cast<std::size_t>(l + 1)]);
        refreshLayers();
        m_layers->setCurrentRow(static_cast<int>(m_map.layers.size()) - 2 - l);
        rerender();
    });
    lbtn(tr("↓"), tr("Move layer down"), [this] {
        const int l = currentLayer();
        if (l <= 0) return;
        pushUndo();
        std::swap(m_map.layers[static_cast<std::size_t>(l)], m_map.layers[static_cast<std::size_t>(l - 1)]);
        refreshLayers();
        m_layers->setCurrentRow(static_cast<int>(m_map.layers.size()) - l);
        rerender();
    });
    lbtn(tr("Solid"), tr("Make every tile of this layer solid"), [this] {
        if (currentLayer() < 0) return;
        pushUndo();
        m_map.solidFromLayer(currentLayer());
        rerender();
    });
    layerButtons->addStretch();
    left->addLayout(layerButtons);
    connect(m_layers, &QListWidget::itemChanged, this, [this](QListWidgetItem* it) {
        const int l = static_cast<int>(m_map.layers.size()) - 1 - m_layers->row(it);
        if (l < 0) return;
        auto& layer = m_map.layers[static_cast<std::size_t>(l)];
        layer.visible = it->checkState() == Qt::Checked;
        const std::string name = it->text().toStdString();
        if (!name.empty()) layer.name = name;
        m_dirty = true;
        rerender();
    });

    auto* sheetLabel = new QLabel(tr("&Tiles:"), this);
    m_sheet = new QComboBox(this);
    sheetLabel->setBuddy(m_sheet);
    left->addWidget(sheetLabel);
    left->addWidget(m_sheet);
    m_palette = new QListWidget(this);
    m_palette->setViewMode(QListView::IconMode);
    m_palette->setIconSize(QSize(32, 32));
    m_palette->setGridSize(QSize(38, 38));
    m_palette->setMovement(QListView::Static);
    m_palette->setResizeMode(QListView::Adjust);
    m_palette->setMinimumWidth(240);
    m_palette->setAccessibleName(tr("Tile palette"));
    left->addWidget(m_palette, 2);
    connect(m_sheet, &QComboBox::currentIndexChanged, this, &WorldBuilderPage::refreshPalette);
    connect(m_palette, &QListWidget::currentRowChanged, this, [this] {
        if (m_tool != Tool::Paint && m_tool != Tool::Fill && m_tool != Tool::Rect && m_tool != Tool::Terrain) selectTool(Tool::Paint);
    });

    auto* propsLabel = new QLabel(tr("&Props:"), this);
    m_propList = new QListWidget(this);
    m_propList->setViewMode(QListView::IconMode);
    m_propList->setIconSize(QSize(40, 40));
    m_propList->setGridSize(QSize(64, 60));
    m_propList->setMovement(QListView::Static);
    m_propList->setResizeMode(QListView::Adjust);
    propsLabel->setBuddy(m_propList);
    left->addWidget(propsLabel);
    left->addWidget(m_propList, 1);
    m_propSolid = new QCheckBox(tr("New props are solid"), this);
    left->addWidget(m_propSolid);
    connect(m_propList, &QListWidget::currentRowChanged, this, [this](int r) {
        if (r >= 0) {
            selectTool(Tool::Prop);
        }
    });

    // Centre: canvas.
    m_canvas = new MapCanvas(*this, this);
    m_scroll = new QScrollArea(this);
    m_scroll->setWidget(m_canvas);
    m_scroll->setAlignment(Qt::AlignCenter);
    m_scroll->setAccessibleName(tr("Map"));
    body->addWidget(m_scroll, 1);

    // Right: view options.
    auto* right = new QFormLayout;
    body->addLayout(right, 0);
    m_zoom = new QSpinBox(this);
    m_zoom->setRange(1, 8);
    m_zoom->setValue(2);
    m_zoom->setSuffix(QStringLiteral("×"));
    right->addRow(tr("&Zoom:"), m_zoom);
    m_showGrid = new QCheckBox(tr("&Grid"), this);
    m_showGrid->setChecked(true);
    right->addRow(m_showGrid);
    m_showCollision = new QCheckBox(tr("Show &collision"), this);
    m_showCollision->setChecked(true);
    right->addRow(m_showCollision);
    m_showLighting = new QCheckBox(tr("&Lighting preview"), this);
    right->addRow(m_showLighting);
    auto* ambient = new QPushButton(tr("Ambient Light…"), this);
    right->addRow(ambient);
    connect(ambient, &QPushButton::clicked, this, [this] {
        const QColor c = QColorDialog::getColor(toQColor(m_map.ambient), this, tr("Ambient Light"));
        if (!c.isValid()) return;
        pushUndo();
        m_map.ambient = Rgba8{static_cast<std::uint8_t>(c.red()), static_cast<std::uint8_t>(c.green()), static_cast<std::uint8_t>(c.blue()), 255};
        m_showLighting->setChecked(true);
        rerender();
    });
    m_lightRadius = new QSpinBox(this);
    m_lightRadius->setRange(4, 1024);
    m_lightRadius->setValue(64);
    m_lightRadius->setSuffix(tr(" px"));
    right->addRow(tr("Light &radius:"), m_lightRadius);
    m_cameraPreview = new QCheckBox(tr("Camera / &parallax preview"), this);
    right->addRow(m_cameraPreview);
    m_viewWidth = new QSpinBox(this);
    m_viewWidth->setRange(64, 4096);
    m_viewWidth->setValue(320);
    m_viewWidth->setSuffix(tr(" px"));
    right->addRow(tr("&View width:"), m_viewWidth);
    m_camera = new QSlider(Qt::Horizontal, this);
    m_camera->setAccessibleName(tr("Camera position"));
    right->addRow(tr("Ca&mera:"), m_camera);
    m_info = new QLabel(this);
    m_info->setWordWrap(true);
    m_info->setProperty("role", "muted");
    m_info->setMinimumWidth(200);
    right->addRow(m_info);
    for (QCheckBox* c : {m_showGrid, m_showCollision, m_showLighting, m_cameraPreview}) connect(c, &QCheckBox::toggled, this, &WorldBuilderPage::rerender);
    for (QSpinBox* s : {m_zoom, m_viewWidth}) connect(s, &QSpinBox::valueChanged, this, &WorldBuilderPage::rerender);
    connect(m_camera, &QSlider::valueChanged, this, &WorldBuilderPage::rerender);

    auto* undoKey = new QShortcut(QKeySequence::Undo, this, [this] { undo(); });
    undoKey->setContext(Qt::WidgetWithChildrenShortcut);
    auto* redoKey = new QShortcut(QKeySequence::Redo, this, [this] { redo(); });
    redoKey->setContext(Qt::WidgetWithChildrenShortcut);
    refreshAll();
}

int WorldBuilderPage::currentLayer() const {
    const int row = m_layers->currentRow();
    if (row < 0) return m_map.layers.empty() ? -1 : 0;
    return static_cast<int>(m_map.layers.size()) - 1 - row;
}

void WorldBuilderPage::selectTool(Tool t) {
    // The buttons are in the enum's order.
    m_tool = t;
    m_rectStart.reset();
    m_toolButtons[static_cast<std::size_t>(t)]->setChecked(true);
}

std::int32_t WorldBuilderPage::brushCell() const {
    const int s = m_sheet->currentIndex(), t = m_palette->currentRow();
    if (s < 0 || t < 0) return kEmpty;
    return makeCell(s, t);
}

void WorldBuilderPage::refreshAll() {
    refreshLayers();
    const int s = m_sheet->currentIndex();
    m_sheet->blockSignals(true);
    m_sheet->clear();
    for (const TileSheet& sh : m_map.sheets) m_sheet->addItem(qs(sh.name));
    m_sheet->setCurrentIndex(std::clamp(s, 0, std::max(0, m_sheet->count() - 1)));
    m_sheet->blockSignals(false);
    refreshPalette();
    refreshProps();
    rerender();
}

void WorldBuilderPage::refreshLayers() {
    const int row = m_layers->currentRow();
    m_layers->blockSignals(true);
    m_layers->clear();
    for (auto it = m_map.layers.rbegin(); it != m_map.layers.rend(); ++it) {
        auto* item = new QListWidgetItem(qs(it->name), m_layers);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsEditable);
        item->setCheckState(it->visible ? Qt::Checked : Qt::Unchecked);
    }
    m_layers->setCurrentRow(std::clamp(row, 0, m_layers->count() - 1));
    m_layers->blockSignals(false);
}

void WorldBuilderPage::refreshPalette() {
    const int row = m_palette->currentRow();
    m_palette->clear();
    const int s = m_sheet->currentIndex();
    if (s < 0 || s >= static_cast<int>(m_map.sheets.size())) return;
    const TileSheet& sh = m_map.sheets[static_cast<std::size_t>(s)];
    for (int i = 0; i < sh.count(); ++i) {
        auto* it = new QListWidgetItem(tileIcon(sh.image.copy(sh.tileRect(i)), 32), QString(), m_palette);
        it->setToolTip(tr("Tile %1").arg(i));
    }
    m_palette->setCurrentRow(std::clamp(row, 0, m_palette->count() - 1));
}

void WorldBuilderPage::refreshProps() {
    const int row = m_propList->currentRow();
    m_propList->blockSignals(true);
    m_propList->clear();
    for (const PropDef& d : m_map.propDefs) new QListWidgetItem(tileIcon(d.image, 40), qs(d.name).left(10), m_propList);
    m_propList->setCurrentRow(std::min(row, m_propList->count() - 1));
    m_propList->blockSignals(false);
}

void WorldBuilderPage::rerender() {
    RenderOptions o;
    o.grid = m_showGrid->isChecked();
    o.collision = m_showCollision->isChecked();
    o.lighting = m_showLighting->isChecked();
    int offset = 0;
    m_camera->setEnabled(m_cameraPreview->isChecked());
    m_viewWidth->setEnabled(m_cameraPreview->isChecked());
    if (m_cameraPreview->isChecked()) {
        o.viewWidth = std::min(m_viewWidth->value(), m_map.pixelWidth());
        m_camera->blockSignals(true);
        m_camera->setRange(0, std::max(0, m_map.pixelWidth() - o.viewWidth));
        m_camera->blockSignals(false);
        o.cameraX = m_camera->value();
        offset = o.cameraX;
    }
    m_canvas->setImage(toQImage(world::render(m_map, o)), m_zoom->value(), offset);
    m_info->setText(tr("%1 × %2 cells of %3 × %4 px · %5 layer(s) · %6 prop(s) · %7 light(s)%8")
                        .arg(m_map.width())
                        .arg(m_map.height())
                        .arg(m_map.tileW())
                        .arg(m_map.tileH())
                        .arg(m_map.layers.size())
                        .arg(m_map.props.size())
                        .arg(m_map.lights.size())
                        .arg(m_file.isEmpty() ? QString() : QStringLiteral("\n") + QFileInfo(m_file).fileName() + (m_dirty ? QStringLiteral(" *") : QString())));
}

void WorldBuilderPage::pushUndo() {
    m_undo.push_back(m_map);
    if (m_undo.size() > 60) m_undo.erase(m_undo.begin());
    m_redo.clear();
    m_dirty = true;
}

void WorldBuilderPage::undo() {
    if (m_undo.empty()) return;
    m_redo.push_back(std::move(m_map));
    m_map = std::move(m_undo.back());
    m_undo.pop_back();
    refreshAll();
}

void WorldBuilderPage::redo() {
    if (m_redo.empty()) return;
    m_undo.push_back(std::move(m_map));
    m_map = std::move(m_redo.back());
    m_redo.pop_back();
    refreshAll();
}

void WorldBuilderPage::pressAt(int px, int py, bool primary) {
    const int cx = px / m_map.tileW(), cy = py / m_map.tileH();
    const int layer = currentLayer();
    switch (m_tool) {
    case Tool::Paint:
    case Tool::Erase: {
        const std::int32_t v = (m_tool == Tool::Erase || !primary) ? kEmpty : brushCell();
        if (m_tool == Tool::Paint && primary && v == kEmpty)
            return void(Q_EMIT statusMessage(tr("Add a tile sheet first (Add › Tile Sheet…), then pick a tile.")));
        pushUndo();
        m_map.setCell(layer, cx, cy, v);
        break;
    }
    case Tool::Fill: {
        const std::int32_t v = primary ? brushCell() : kEmpty;
        pushUndo();
        m_map.floodFill(layer, cx, cy, v);
        break;
    }
    case Tool::Rect:
        m_rectStart = std::pair{cx, cy};
        return;
    case Tool::Terrain: {
        const int sheet = m_sheet->currentIndex();
        if (sheet < 0 || m_map.sheets[static_cast<std::size_t>(sheet)].terrain.empty())
            return void(Q_EMIT statusMessage(tr("Choose a terrain sheet: add a tileset made with the Tileset Generator "
                                                "(Add › Tile Sheet from File…, next to its .json).")));
        pushUndo();
        if (auto st = m_map.paintTerrain(layer, sheet, cx, cy, primary); !st) return void(Q_EMIT statusMessage(qs(st.error().message)));
        break;
    }
    case Tool::Collision:
        pushUndo();
        m_map.setSolid(cx, cy, primary);
        break;
    case Tool::Light:
        pushUndo();
        if (primary) {
            m_map.lights.push_back({px, py, m_lightRadius->value()});
            m_showLighting->setChecked(true);
        } else if (!m_map.lights.empty()) {
            auto nearest = std::min_element(m_map.lights.begin(), m_map.lights.end(), [&](const Light& a, const Light& b) {
                return std::hypot(a.x - px, a.y - py) < std::hypot(b.x - px, b.y - py);
            });
            m_map.lights.erase(nearest);
        }
        break;
    case Tool::Prop: {
        // A prop under the cursor (topmost first) is moved or removed.
        for (std::size_t i = m_map.props.size(); i-- > 0;) {
            const Prop& p = m_map.props[i];
            const Image& img = m_map.propDefs[static_cast<std::size_t>(p.def)].image;
            if (px >= p.x && py >= p.y && px < p.x + img.width() && py < p.y + img.height() && img.pixel(px - p.x, py - p.y).a > 0) {
                pushUndo();
                if (!primary) {
                    m_map.props.erase(m_map.props.begin() + static_cast<std::ptrdiff_t>(i));
                    rerender();
                    return;
                }
                m_dragProp = i;
                m_dragDx = px - p.x;
                m_dragDy = py - p.y;
                return;
            }
        }
        const int d = m_propList->currentRow();
        if (!primary || d < 0) {
            if (d < 0) Q_EMIT statusMessage(tr("Add a prop first (Add › Props…), then pick it."));
            return;
        }
        const Image& img = m_map.propDefs[static_cast<std::size_t>(d)].image;
        pushUndo();
        // Bottom centre at the cursor, snapped to whole pixels.
        m_map.props.push_back({d, px - img.width() / 2, py - img.height() + 1, m_propSolid->isChecked()});
        break;
    }
    }
    rerender();
}

void WorldBuilderPage::dragTo(int px, int py, bool primary) {
    const int cx = px / m_map.tileW(), cy = py / m_map.tileH();
    switch (m_tool) {
    case Tool::Paint:
    case Tool::Erase:
        m_map.setCell(currentLayer(), cx, cy, (m_tool == Tool::Erase || !primary) ? kEmpty : brushCell());
        break;
    case Tool::Collision: m_map.setSolid(cx, cy, primary); break;
    case Tool::Terrain: {
        const int sheet = m_sheet->currentIndex();
        if (sheet < 0 || !m_map.paintTerrain(currentLayer(), sheet, cx, cy, primary)) return;
        break;
    }
    case Tool::Prop:
        if (!m_dragProp) return;
        m_map.props[*m_dragProp].x = px - m_dragDx;
        m_map.props[*m_dragProp].y = py - m_dragDy;
        break;
    default: return;
    }
    rerender();
}

void WorldBuilderPage::releaseAt(int px, int py, bool primary) {
    if (m_tool == Tool::Rect && m_rectStart) {
        const int cx = px / m_map.tileW(), cy = py / m_map.tileH();
        const auto [sx, sy] = *m_rectStart;
        pushUndo();
        m_map.fillRect(currentLayer(), {std::min(sx, cx), std::min(sy, cy), std::abs(cx - sx) + 1, std::abs(cy - sy) + 1}, primary ? brushCell() : kEmpty);
        m_rectStart.reset();
        rerender();
    }
    m_dragProp.reset();
}

bool WorldBuilderPage::confirmDiscard() {
    if (!m_dirty) return true;
    const auto r = QMessageBox::question(this, tr("World Builder"), tr("Save the changes to the current map first?"),
                                         QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (r == QMessageBox::Cancel) return false;
    if (r == QMessageBox::Save) return saveMap(false);
    return true;
}

void WorldBuilderPage::newMap() {
    if (!confirmDiscard()) return;
    QDialog dlg(this);
    dlg.setWindowTitle(tr("New Map"));
    auto* form = new QFormLayout(&dlg);
    auto spin = [&](int lo, int hi, int v) {
        auto* s = new QSpinBox(&dlg);
        s->setRange(lo, hi);
        s->setValue(v);
        return s;
    };
    auto* w = spin(1, 4096, 40);
    auto* h = spin(1, 4096, 23);
    auto* tw = spin(1, 512, m_map.tileW());
    auto* th = spin(1, 512, m_map.tileH());
    form->addRow(tr("&Width (cells):"), w);
    form->addRow(tr("&Height (cells):"), h);
    form->addRow(tr("Tile w&idth (px):"), tw);
    form->addRow(tr("Tile h&eight (px):"), th);
    auto* keep = new QCheckBox(tr("&Keep tile sheets and props"), &dlg);
    keep->setChecked(true);
    form->addRow(keep);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);
    if (dlg.exec() != QDialog::Accepted) return;
    auto m = WorldMap::create(w->value(), h->value(), tw->value(), th->value());
    if (!m) return void(QMessageBox::warning(this, tr("New Map"), qs(m.error().message)));
    WorldMap fresh = std::move(m).value();
    if (keep->isChecked()) {
        for (TileSheet& s : m_map.sheets)
            if (s.tileW == fresh.tileW() && s.tileH == fresh.tileH()) (void)fresh.addSheet(s);
        fresh.propDefs = m_map.propDefs;
        fresh.backgrounds = m_map.backgrounds;
    }
    fresh.addLayer("Ground");
    fresh.addLayer("Detail");
    m_map = std::move(fresh);
    m_file.clear();
    m_dirty = false;
    m_undo.clear();
    m_redo.clear();
    refreshAll();
}

void WorldBuilderPage::openMapDialog() {
    if (!confirmDiscard()) return;
    const QString path = QFileDialog::getOpenFileName(this, tr("Open Map"), QString(), tr("AceStudio maps (*.acemap)"));
    if (!path.isEmpty()) openMap(path);
}

bool WorldBuilderPage::openMap(const QString& path) {
    auto m = world::load(toPath(path));
    if (!m) {
        QMessageBox::warning(this, tr("Open Map"), qs(m.error().message));
        return false;
    }
    m_map = std::move(m).value();
    m_file = path;
    m_dirty = false;
    m_undo.clear();
    m_redo.clear();
    refreshAll();
    return true;
}

bool WorldBuilderPage::saveMap(bool askPath) {
    QString path = m_file;
    if (askPath || path.isEmpty()) {
        path = QFileDialog::getSaveFileName(this, tr("Save Map"), path.isEmpty() ? tr("level.acemap") : path, tr("AceStudio maps (*.acemap)"));
        if (path.isEmpty()) return false;
        if (!path.endsWith(QStringLiteral(".acemap"), Qt::CaseInsensitive)) path += QStringLiteral(".acemap");
    }
    if (auto st = world::save(m_map, toPath(path)); !st) {
        QMessageBox::warning(this, tr("Save Map"), qs(st.error().message));
        return false;
    }
    m_file = path;
    m_dirty = false;
    rerender();
    Q_EMIT statusMessage(tr("Saved %1").arg(QFileInfo(path).fileName()));
    return true;
}

void WorldBuilderPage::exportTiled() {
    QString path = QFileDialog::getSaveFileName(this, tr("Export to Tiled"), m_file.isEmpty() ? tr("level.tmx") : QFileInfo(m_file).completeBaseName() + ".tmx",
                                                tr("Tiled maps (*.tmx)"));
    if (path.isEmpty()) return;
    if (!path.endsWith(QStringLiteral(".tmx"), Qt::CaseInsensitive)) path += QStringLiteral(".tmx");
    auto files = world::exportTiled(m_map, toPath(path));
    if (!files) return void(QMessageBox::warning(this, tr("Export to Tiled"), qs(files.error().message)));
    Q_EMIT statusMessage(tr("Exported %n file(s) next to %1", nullptr, static_cast<int>(files.value().size())).arg(QFileInfo(path).fileName()));
}

void WorldBuilderPage::addSheet(bool fromDocument) {
    Image img;
    QString name;
    if (fromDocument) {
        auto cur = m_currentImage ? m_currentImage() : std::nullopt;
        if (!cur) return void(QMessageBox::information(this, tr("Add Tile Sheet"), tr("Open a tile sheet document first.")));
        img = cur->first;
        name = cur->second;
    } else {
        const QString path = QFileDialog::getOpenFileName(this, tr("Add Tile Sheet"), QString(), tr("PNG images (*.png)"));
        if (path.isEmpty()) return;
        auto r = readPng(toPath(path));
        if (!r) return void(QMessageBox::warning(this, tr("Add Tile Sheet"), qs(r.error().message)));
        img = std::move(r).value();
        name = QFileInfo(path).completeBaseName();
        // A Tileset Generator export: its .json gives the terrain scheme, the
        // tile size and the base fill, so the Terrain tool can paint with it.
        const QString jsonPath = QFileInfo(path).dir().filePath(name + QStringLiteral(".json"));
        if (QFileInfo::exists(jsonPath)) {
            auto bytes = readFileBytes(toPath(jsonPath));
            const nlohmann::json meta = bytes ? nlohmann::json::parse(bytes.value().begin(), bytes.value().end(), nullptr, false) : nlohmann::json();
            const std::string scheme = meta.is_object() ? meta.value("scheme", std::string()) : std::string();
            if (scheme == "blob47" || scheme == "cardinal16") {
                const int ts = meta.value("tileSize", 0);
                TileSheet sheet{name.toStdString(), img, ts, ts};
                sheet.terrain = scheme;
                sheet.terrainPhases = std::clamp(meta.value("phases", 1), 1, 16);
                pushUndo();
                if (auto st = m_map.addSheet(sheet); !st) {
                    undo();
                    m_redo.clear();
                    return void(QMessageBox::warning(this, tr("Add Tile Sheet"), qs(st.error().message)));
                }
                const int terrainIndex = static_cast<int>(m_map.sheets.size()) - 1;
                QString note = tr("Terrain sheet added: paint it with the Terrain tool.");
                const std::string baseName = meta.value("base", std::string());
                if (!baseName.empty() && baseName.find('/') == std::string::npos && baseName.find('\\') == std::string::npos) {
                    if (auto base = readPng(toPath(QFileInfo(path).dir().filePath(QString::fromStdString(baseName))))) {
                        if (m_map.addSheet({name.toStdString() + " base", std::move(base).value(), ts, ts}))
                            note += QLatin1Char(' ') + tr("Fill the ground layer with “%1 base”.").arg(name);
                    }
                }
                refreshAll();
                m_sheet->setCurrentIndex(terrainIndex);
                selectTool(Tool::Terrain);
                Q_EMIT statusMessage(note);
                return;
            }
        }
    }
    bool ok = false;
    const QString size = QInputDialog::getText(this, tr("Add Tile Sheet"), tr("Tile size in the sheet (W×H):"), QLineEdit::Normal,
                                               QStringLiteral("%1x%2").arg(m_map.tileW()).arg(m_map.tileH()), &ok);
    if (!ok) return;
    const QStringList parts = size.toLower().split(QRegularExpression(QStringLiteral("[x×* ]")), Qt::SkipEmptyParts);
    const int tw = parts.value(0).toInt(), th = parts.value(1, parts.value(0)).toInt();
    pushUndo();
    if (auto st = m_map.addSheet({name.toStdString(), img, tw, th}); !st) {
        undo();
        m_redo.clear();
        return void(QMessageBox::warning(this, tr("Add Tile Sheet"), qs(st.error().message)));
    }
    if (tw != m_map.tileW() || th != m_map.tileH())
        Q_EMIT statusMessage(tr("This sheet's tiles are %1×%2 px; the map grid is %3×%4. Larger tiles grow upwards from their cell.")
                                 .arg(tw).arg(th).arg(m_map.tileW()).arg(m_map.tileH()));
    refreshAll();
    m_sheet->setCurrentIndex(m_sheet->count() - 1);
}

void WorldBuilderPage::addProp(bool fromDocument) {
    std::vector<std::pair<Image, QString>> items;
    if (fromDocument) {
        auto cur = m_currentImage ? m_currentImage() : std::nullopt;
        if (!cur) return void(QMessageBox::information(this, tr("Add Prop"), tr("Open a sprite document first.")));
        items.push_back(*cur);
    } else {
        for (const QString& path : QFileDialog::getOpenFileNames(this, tr("Add Props"), QString(), tr("PNG images (*.png)"))) {
            auto r = readPng(toPath(path));
            if (!r) { QMessageBox::warning(this, tr("Add Props"), qs(r.error().message)); continue; }
            items.push_back({std::move(r).value(), QFileInfo(path).completeBaseName()});
        }
    }
    if (items.empty()) return;
    pushUndo();
    for (auto& [img, name] : items) {
        const IntRect b = img.opaqueBounds();
        if (b.empty()) continue;
        m_map.propDefs.push_back({name.toStdString(), img.copy(b)});
    }
    refreshProps();
    m_propList->setCurrentRow(m_propList->count() - 1);
    rerender();
}

void WorldBuilderPage::addBackground(bool fromDocument) {
    Image img;
    QString name;
    if (fromDocument) {
        auto cur = m_currentImage ? m_currentImage() : std::nullopt;
        if (!cur) return void(QMessageBox::information(this, tr("Add Background"), tr("Open a background document first.")));
        img = cur->first;
        name = cur->second;
    } else {
        const QString path = QFileDialog::getOpenFileName(this, tr("Add Parallax Background"), QString(), tr("PNG images (*.png)"));
        if (path.isEmpty()) return;
        auto r = readPng(toPath(path));
        if (!r) return void(QMessageBox::warning(this, tr("Add Parallax Background"), qs(r.error().message)));
        img = std::move(r).value();
        name = QFileInfo(path).completeBaseName();
    }
    bool ok = false;
    const double factor = QInputDialog::getDouble(this, tr("Add Parallax Background"),
                                                  tr("Scroll speed relative to the map (0 = fixed, 1 = moves with the map):"), 0.3, 0.0, 2.0, 2, &ok);
    if (!ok) return;
    pushUndo();
    // Bottom-aligned with the map by default; layers added later are in front.
    m_map.backgrounds.push_back({name.toStdString(), img, factor, std::max(0, m_map.pixelHeight() - img.height())});
    m_cameraPreview->setChecked(true);
    rerender();
}

} // namespace acestudio::ui
