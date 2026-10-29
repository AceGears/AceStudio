#pragma once

#include "acestudio/core/Paint.h"

#include <QColor>
#include <QObject>
#include <QString>

#include <algorithm>
#include <utility>

namespace acestudio::ui {

enum class Tool { Pencil, Eraser, Line, Rectangle, Ellipse, Fill, Gradient, Eyedropper, Select, Lasso, MagicWand, Hand };

constexpr Tool kAllTools[] = {Tool::Pencil, Tool::Eraser, Tool::Line, Tool::Rectangle, Tool::Ellipse, Tool::Fill,
                              Tool::Gradient, Tool::Eyedropper, Tool::Select, Tool::Lasso, Tool::MagicWand, Tool::Hand};
inline bool isSelectionTool(Tool t) { return t == Tool::Select || t == Tool::Lasso || t == Tool::MagicWand; }

QString toolName(Tool t);
QString toolShortcut(Tool t);   // single key, e.g. "B"

// Application-wide tool configuration shared by every open document.
class ToolState : public QObject {
    Q_OBJECT
public:
    explicit ToolState(QObject* parent = nullptr) : QObject(parent) {}

    Tool tool() const { return m_tool; }
    QColor primary() const { return m_primary; }
    QColor secondary() const { return m_secondary; }
    BrushSettings brush() const { return m_brush; }
    bool pixelPerfect() const { return m_pixelPerfect; }
    bool fillShapes() const { return m_fillShapes; }
    bool fillContiguous() const { return m_fillContiguous; }
    int fillTolerance() const { return m_fillTolerance; }
    bool sampleMerged() const { return m_sampleMerged; }
    SelectionOp selectionOp() const { return m_selectionOp; }
    GradientShape gradientShape() const { return m_gradientShape; }
    GradientDither gradientDither() const { return m_gradientDither; }

public Q_SLOTS:
    void setTool(acestudio::ui::Tool t) { if (t != m_tool) { m_tool = t; Q_EMIT changed(); } }
    void setPrimary(const QColor& c) { if (c != m_primary) { m_primary = c; Q_EMIT changed(); } }
    void setSecondary(const QColor& c) { if (c != m_secondary) { m_secondary = c; Q_EMIT changed(); } }
    void swapColors() { std::swap(m_primary, m_secondary); Q_EMIT changed(); }
    void setBrushSize(int s) { m_brush.size = std::clamp(s, 1, 64); Q_EMIT changed(); }
    void setBrushShape(BrushShape s) { m_brush.shape = s; Q_EMIT changed(); }
    void setMirror(MirrorMode m) { m_brush.mirror = m; Q_EMIT changed(); }
    void setPixelPerfect(bool on) { m_pixelPerfect = on; Q_EMIT changed(); }
    void setFillShapes(bool on) { m_fillShapes = on; Q_EMIT changed(); }
    void setFillContiguous(bool on) { m_fillContiguous = on; Q_EMIT changed(); }
    void setFillTolerance(int t) { m_fillTolerance = std::clamp(t, 0, 255); Q_EMIT changed(); }
    void setSampleMerged(bool on) { m_sampleMerged = on; Q_EMIT changed(); }
    void setDitherDensity(int d) { m_brush.ditherDensity = std::clamp(d, 1, 16); Q_EMIT changed(); }
    void setSelectionOp(SelectionOp op) { m_selectionOp = op; Q_EMIT changed(); }
    void setGradientShape(GradientShape s) { m_gradientShape = s; Q_EMIT changed(); }
    void setGradientDither(GradientDither d) { m_gradientDither = d; Q_EMIT changed(); }

Q_SIGNALS:
    void changed();

private:
    Tool m_tool = Tool::Pencil;
    QColor m_primary{29, 29, 36};
    QColor m_secondary{255, 255, 255};
    BrushSettings m_brush;
    bool m_pixelPerfect = true;
    bool m_fillShapes = false;
    bool m_fillContiguous = true;
    int m_fillTolerance = 0;
    bool m_sampleMerged = true;
    SelectionOp m_selectionOp = SelectionOp::Replace;
    GradientShape m_gradientShape = GradientShape::Linear;
    GradientDither m_gradientDither = GradientDither::Ordered;
};

} // namespace acestudio::ui
