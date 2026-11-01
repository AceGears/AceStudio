#pragma once
// World Builder workspace: a map canvas. Tile layers painted from tile sheets,
// props, a collision grid, parallax backgrounds and lights with a live
// preview; saved as .acemap and exported to Tiled (.tmx) for Godot, Unity
// (SuperTiled2Unity) and other engines.

#include "acestudio/core/WorldMap.h"

#include <QWidget>

#include <functional>
#include <optional>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class QScrollArea;
class QSlider;
class QSpinBox;
class QToolButton;

namespace acestudio::ui {

class AppContext;
class MapCanvas;

class WorldBuilderPage : public QWidget {
    Q_OBJECT
public:
    // `currentImage` returns the open document's current frame (and its name), if any.
    using ImageSource = std::function<std::optional<std::pair<Image, QString>>()>;
    WorldBuilderPage(AppContext& ctx, ImageSource currentImage, QWidget* parent = nullptr);
    bool openMap(const QString& path);

    enum class Tool { Paint, Erase, Fill, Rect, Terrain, Prop, Collision, Light };

Q_SIGNALS:
    void statusMessage(const QString& text);

private:
    friend class MapCanvas;
    void newMap();
    void openMapDialog();
    bool saveMap(bool askPath);
    void exportTiled();
    void addSheet(bool fromDocument);
    void addProp(bool fromDocument);
    void addBackground(bool fromDocument);
    void refreshAll();
    void refreshLayers();
    void refreshPalette();
    void refreshProps();
    void rerender();
    void pushUndo();
    void undo();
    void redo();
    bool confirmDiscard();
    std::int32_t brushCell() const;
    int currentLayer() const;

    // Canvas callbacks (map pixel coordinates).
    void pressAt(int px, int py, bool primary);
    void dragTo(int px, int py, bool primary);
    void releaseAt(int px, int py, bool primary);

    AppContext& m_ctx;
    ImageSource m_currentImage;
    world::WorldMap m_map;
    QString m_file;
    bool m_dirty = false;
    std::vector<world::WorldMap> m_undo, m_redo;
    Tool m_tool = Tool::Paint;
    void selectTool(Tool t);
    std::optional<std::pair<int, int>> m_rectStart;
    std::optional<std::size_t> m_dragProp;
    int m_dragDx = 0, m_dragDy = 0;

    MapCanvas* m_canvas = nullptr;
    QScrollArea* m_scroll = nullptr;
    QComboBox* m_sheet = nullptr;
    QListWidget* m_palette = nullptr;
    QListWidget* m_layers = nullptr;
    QListWidget* m_propList = nullptr;
    QCheckBox* m_propSolid = nullptr;
    QCheckBox* m_showGrid = nullptr;
    QCheckBox* m_showCollision = nullptr;
    QCheckBox* m_showLighting = nullptr;
    QCheckBox* m_cameraPreview = nullptr;
    QSlider* m_camera = nullptr;
    QSpinBox* m_viewWidth = nullptr;
    QSpinBox* m_zoom = nullptr;
    QSpinBox* m_lightRadius = nullptr;
    QLabel* m_info = nullptr;
    std::vector<QToolButton*> m_toolButtons;
};

} // namespace acestudio::ui
