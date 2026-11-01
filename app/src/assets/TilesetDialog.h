#pragma once

#include "acestudio/core/Autotile.h"

#include <QDialog>
#include <QPointer>

#include <functional>
#include <memory>
#include <vector>

class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;
class QSpinBox;
class QVBoxLayout;

namespace acestudio::ui {

class AiImageJob;
class AiService;
class DocumentView;
class PixelView;

// Assets › Tileset Generator: seamless textures (from the document, a file or
// SDXL) become complete autotile sets, checked for seams. One base and one to
// three terrains stacked as levels (e.g. water, sand, grass); textures can be
// up to 4x4 tiles, giving each tile phase variants that keep the detail.
class TilesetDialog : public QDialog {
    Q_OBJECT
public:
    using OpenImageFn = std::function<void(const Image&, const QString&)>;
    TilesetDialog(AiService& ai, DocumentView* view, OpenImageFn openImage, QWidget* parent = nullptr);
    ~TilesetDialog() override;

private:
    struct Source {
        Image original;         // as chosen, before fitting to the texture size
        Image texture;          // phases*tileSize square
        QGroupBox* box = nullptr;
        PixelView* view = nullptr;
        QLabel* status = nullptr;
        QLineEdit* prompt = nullptr;
        QPushButton* generate = nullptr;
        AiImageJob* job = nullptr;
    };
    Source& addSource(const QString& defaultPrompt);
    void retitleSources();
    void setTexture(Source& s, const Image& img);
    void generateTexture(Source& s);
    void rebuild();
    void exportFiles();
    void openSandbox();
    AutotileScheme scheme() const;
    int tileSize() const;
    int phases() const;

    AiService& m_ai;
    QPointer<DocumentView> m_view;
    OpenImageFn m_openImage;
    std::vector<std::unique_ptr<Source>> m_sources;   // [0] = base, then terrain levels 1..3
    std::vector<Tileset> m_levels;

    QVBoxLayout* m_sourceLayout = nullptr;
    QPushButton* m_addLevel = nullptr;
    QPushButton* m_removeLevel = nullptr;
    QComboBox* m_scheme = nullptr;
    QSpinBox* m_tileSize = nullptr;
    QComboBox* m_phases = nullptr;
    QSlider* m_roundness = nullptr;
    QSlider* m_noise = nullptr;
    QSpinBox* m_noiseScale = nullptr;
    QCheckBox* m_outline = nullptr;
    QSpinBox* m_seed = nullptr;
    PixelView* m_preview = nullptr;
    QComboBox* m_previewLevel = nullptr;
    PixelView* m_mapPreview = nullptr;
    QLabel* m_seams = nullptr;
    QPushButton* m_export = nullptr;
    class EngineChoice* m_engines = nullptr;
    QPushButton* m_sandbox = nullptr;
    QPushButton* m_open = nullptr;
};

// Assets › Terrain Sandbox: paint terrain levels and see the autotiled result
// live (left button paints the chosen level, right button paints the base).
class TerrainSandbox : public QDialog {
    Q_OBJECT
public:
    TerrainSandbox(std::vector<Tileset> levels, QWidget* parent = nullptr);

private:
    void paintAt(QPoint imagePos, bool erase);
    void redraw();
    void exportMap();

    std::vector<Tileset> m_levels;
    LevelMap m_map;
    PixelView* m_view = nullptr;
    QSpinBox* m_w = nullptr;
    QSpinBox* m_h = nullptr;
    QSpinBox* m_brush = nullptr;
    QComboBox* m_level = nullptr;
    QCheckBox* m_margins = nullptr;
    QLabel* m_status = nullptr;
};

// Assets › Check Tileset Seams: the current document as a tileset.
void checkDocumentSeams(DocumentView* view, QWidget* parent);

} // namespace acestudio::ui
