#pragma once

#include "acestudio/core/Autotile.h"

#include <QDialog>
#include <QPointer>

#include <functional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;
class QSpinBox;

namespace acestudio::ui {

class AiImageJob;
class AiService;
class DocumentView;
class PixelView;

// Tilesets › Tileset Generator: two seamless textures (from the document, a
// file or SDXL) become a complete autotile set, checked for seams.
class TilesetDialog : public QDialog {
    Q_OBJECT
public:
    using OpenImageFn = std::function<void(const Image&, const QString&)>;
    TilesetDialog(AiService& ai, DocumentView* view, OpenImageFn openImage, QWidget* parent = nullptr);

private:
    struct Source {
        Image texture;          // tile-sized
        PixelView* view = nullptr;
        QLabel* status = nullptr;
        QLineEdit* prompt = nullptr;
        QPushButton* generate = nullptr;
        AiImageJob* job = nullptr;
    };
    QWidget* buildSource(Source& s, const QString& title, const QString& defaultPrompt);
    void setTexture(Source& s, const Image& img);
    void generateTexture(Source& s);
    void rebuild();
    void exportFiles();
    void openSandbox();
    AutotileScheme scheme() const;
    int tileSize() const;

    AiService& m_ai;
    QPointer<DocumentView> m_view;
    OpenImageFn m_openImage;
    Source m_terrain, m_base;
    std::optional<Tileset> m_tileset;

    QComboBox* m_scheme = nullptr;
    QSpinBox* m_tileSize = nullptr;
    QSlider* m_roundness = nullptr;
    QSlider* m_noise = nullptr;
    QSpinBox* m_noiseScale = nullptr;
    QCheckBox* m_outline = nullptr;
    QSpinBox* m_seed = nullptr;
    PixelView* m_preview = nullptr;
    PixelView* m_mapPreview = nullptr;
    QLabel* m_seams = nullptr;
    QPushButton* m_export = nullptr;
    QPushButton* m_sandbox = nullptr;
    QPushButton* m_open = nullptr;
};

// Tilesets › Terrain Sandbox: paint terrain cells and see the autotiled
// result live (left button paints, right button erases).
class TerrainSandbox : public QDialog {
    Q_OBJECT
public:
    TerrainSandbox(Tileset tileset, QWidget* parent = nullptr);

private:
    void paintAt(QPoint imagePos, bool terrain);
    void redraw();
    void exportMap();

    Tileset m_tileset;
    TerrainMap m_map;
    PixelView* m_view = nullptr;
    QSpinBox* m_w = nullptr;
    QSpinBox* m_h = nullptr;
    QSpinBox* m_brush = nullptr;
    QLabel* m_status = nullptr;
};

// Tilesets › Check Tileset Seams: the current document as a tileset.
void checkDocumentSeams(DocumentView* view, QWidget* parent);

} // namespace acestudio::ui
