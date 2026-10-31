#pragma once

#include "acestudio/core/AssetTools.h"
#include "acestudio/core/Atlas.h"

#include <QDialog>
#include <QPointer>

#include <functional>
#include <optional>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QTimer;

namespace acestudio::ui {

class AiImageJob;
class AiService;
class DocumentView;
class PixelView;

using OpenImageFn = std::function<void(const Image&, const QString&)>;
using OpenFramesFn = std::function<void(const std::vector<Image>&, const std::vector<int>&, const QString&)>;

// Assets › Parallax Background: layers (from the document, files or SDXL
// with horizontal wrap) with scroll factors, an animated preview, checks for
// seams and gaps, and export as PNGs + JSON.
class ParallaxDialog : public QDialog {
    Q_OBJECT
public:
    ParallaxDialog(AiService& ai, DocumentView* view, QWidget* parent = nullptr);

private:
    void addLayer(Image img, const QString& name);
    void refreshList();
    void layerSelected();
    void layerEdited();
    void redraw();
    void generate();
    void makeSeamless();
    void exportFiles();
    int current() const;

    AiService& m_ai;
    QPointer<DocumentView> m_view;
    ParallaxSet m_set;
    double m_camera = 0;
    bool m_updating = false;

    QSpinBox* m_viewW = nullptr;
    QSpinBox* m_viewH = nullptr;
    QListWidget* m_list = nullptr;
    QDoubleSpinBox* m_scroll = nullptr;
    QSpinBox* m_y = nullptr;
    QCheckBox* m_repeat = nullptr;
    QSpinBox* m_overlap = nullptr;
    QSpinBox* m_skyTolerance = nullptr;
    QCheckBox* m_skyEverywhere = nullptr;
    QLineEdit* m_prompt = nullptr;
    QPushButton* m_generate = nullptr;
    QLabel* m_genStatus = nullptr;
    AiImageJob* m_job = nullptr;
    PixelView* m_preview = nullptr;
    QCheckBox* m_play = nullptr;
    QSpinBox* m_speed = nullptr;
    QLabel* m_issues = nullptr;
    QTimer* m_timer = nullptr;
};

// Assets › UI Kit Generator: a themed set of panels, buttons, checkboxes,
// sliders... with exact 9-slice borders, exported as atlas PNG + JSON.
class UiKitDialog : public QDialog {
    Q_OBJECT
public:
    UiKitDialog(DocumentView* view, OpenImageFn openImage, QWidget* parent = nullptr);

private:
    void rebuild();
    void exportFiles();

    QPointer<DocumentView> m_view;
    OpenImageFn m_openImage;
    UiTheme m_theme;
    std::vector<UiElement> m_kit;
    std::optional<UiKitFiles> m_files;
    std::vector<QPushButton*> m_colors;
    QSpinBox* m_scale = nullptr;
    QCheckBox* m_rounded = nullptr;
    QCheckBox* m_texture = nullptr;
    QComboBox* m_element = nullptr;
    QSpinBox* m_stretchW = nullptr;
    QSpinBox* m_stretchH = nullptr;
    PixelView* m_atlasView = nullptr;
    PixelView* m_stretchView = nullptr;
    QLabel* m_info = nullptr;
};

// Assets › Smart Slicer: sprites on a sheet found by connected regions or by
// a detected grid, exported as JSON / PNGs or opened as animation frames.
class SliceDialog : public QDialog {
    Q_OBJECT
public:
    SliceDialog(DocumentView* view, OpenFramesFn openFrames, QWidget* parent = nullptr);

private:
    void detectGridNow();
    void reslice();
    void exportJson();
    void exportPngs();
    void openFrames();
    SliceOptions options() const;
    GridGuess grid() const;

    Image m_sheet;
    QString m_name;
    OpenFramesFn m_openFrames;
    std::vector<SlicedSprite> m_slices;
    bool m_updating = false;

    QComboBox* m_mode = nullptr;
    QSpinBox* m_tolerance = nullptr;
    QSpinBox* m_merge = nullptr;
    QSpinBox* m_minPixels = nullptr;
    QSpinBox* m_cellW = nullptr;
    QSpinBox* m_cellH = nullptr;
    QSpinBox* m_offX = nullptr;
    QSpinBox* m_offY = nullptr;
    QSpinBox* m_spaceX = nullptr;
    QSpinBox* m_spaceY = nullptr;
    QCheckBox* m_skipEmpty = nullptr;
    QWidget* m_gridBox = nullptr;
    QWidget* m_autoBox = nullptr;
    PixelView* m_preview = nullptr;
    QLabel* m_info = nullptr;
};

// Assets › Pack Sprite Atlas: PNG files and/or document frames packed into
// deterministic pages with TexturePacker-compatible JSON.
class AtlasDialog : public QDialog {
    Q_OBJECT
public:
    AtlasDialog(DocumentView* view, QWidget* parent = nullptr);

private:
    void repack();
    void exportFiles();
    void refreshList();

    QPointer<DocumentView> m_view;
    std::vector<AtlasItem> m_items;
    std::optional<Atlas> m_atlas;
    QListWidget* m_list = nullptr;
    QComboBox* m_maxSize = nullptr;
    QSpinBox* m_padding = nullptr;
    QSpinBox* m_border = nullptr;
    QSpinBox* m_extrude = nullptr;
    QCheckBox* m_trim = nullptr;
    QCheckBox* m_merge = nullptr;
    QCheckBox* m_pot = nullptr;
    QSpinBox* m_page = nullptr;
    PixelView* m_preview = nullptr;
    QLabel* m_info = nullptr;
    QPushButton* m_export = nullptr;
};

// Assets › New Tileset Template: a guide image for drawing a tileset by hand.
void newTilesetTemplate(OpenImageFn openImage, QWidget* parent);

} // namespace acestudio::ui
