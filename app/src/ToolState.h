#pragma once

#include "acestudio/core/Paint.h"

#include <QColor>
#include <QObject>
#include <QString>

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

namespace acestudio::ui {

enum class Tool { Pencil, Eraser, Line, Rectangle, Ellipse, Fill, Gradient, Eyedropper, Select, Lasso, MagicWand, Hand,
                  Curve, Polygon, Clone, Move };

constexpr Tool kAllTools[] = {Tool::Pencil, Tool::Eraser, Tool::Line,    Tool::Curve,     Tool::Rectangle, Tool::Ellipse,
                              Tool::Polygon, Tool::Fill,  Tool::Gradient, Tool::Clone,    Tool::Eyedropper, Tool::Select,
                              Tool::Lasso,  Tool::MagicWand, Tool::Move, Tool::Hand};
inline bool isBrushTool(Tool t) {
    return t == Tool::Pencil || t == Tool::Eraser || t == Tool::Line || t == Tool::Curve || t == Tool::Rectangle ||
           t == Tool::Ellipse || t == Tool::Polygon || t == Tool::Clone;
}
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

    // Custom brush tips (shape = Custom uses the current one).
    struct BrushTip {
        QString name;
        std::shared_ptr<const Image> image;
    };
    const std::vector<BrushTip>& tips() const { return m_tips; }
    int currentTip() const { return m_currentTip; }
    // Adds (or replaces by name) a tip and makes it current.
    void addTip(const QString& name, Image image) {
        auto ptr = std::make_shared<const Image>(std::move(image));
        auto it = std::find_if(m_tips.begin(), m_tips.end(), [&](const BrushTip& t) { return t.name == name; });
        if (it != m_tips.end()) it->image = ptr;
        else it = m_tips.insert(m_tips.end(), BrushTip{name, ptr});
        m_currentTip = static_cast<int>(it - m_tips.begin());
        syncTip();
        Q_EMIT changed();
        Q_EMIT tipsChanged();
    }
    void removeTip(int index) {
        if (index < 0 || index >= static_cast<int>(m_tips.size())) return;
        m_tips.erase(m_tips.begin() + index);
        m_currentTip = m_tips.empty() ? -1 : std::min(m_currentTip, static_cast<int>(m_tips.size()) - 1);
        if (m_tips.empty() && m_brush.shape == BrushShape::Custom) m_brush.shape = BrushShape::Square;
        syncTip();
        Q_EMIT changed();
        Q_EMIT tipsChanged();
    }

public Q_SLOTS:
    void setTool(acestudio::ui::Tool t) { if (t != m_tool) { m_tool = t; Q_EMIT changed(); } }
    void setPrimary(const QColor& c) { if (c != m_primary) { m_primary = c; Q_EMIT changed(); } }
    void setSecondary(const QColor& c) { if (c != m_secondary) { m_secondary = c; Q_EMIT changed(); } }
    void swapColors() { std::swap(m_primary, m_secondary); Q_EMIT changed(); }
    void setBrushSize(int s) { m_brush.size = std::clamp(s, 1, 64); Q_EMIT changed(); }
    void setBrushShape(BrushShape s) {
        if (s == BrushShape::Custom && m_tips.empty()) return;
        m_brush.shape = s;
        syncTip();
        Q_EMIT changed();
    }
    void setCurrentTip(int i) {
        if (i < 0 || i >= static_cast<int>(m_tips.size())) return;
        m_currentTip = i;
        syncTip();
        Q_EMIT changed();
    }
    void setTipColors(bool on) { m_brush.tipColors = on; Q_EMIT changed(); }
    void setInk(Ink ink) { m_brush.ink = ink; Q_EMIT changed(); }
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
    void tipsChanged();

private:
    void syncTip() {
        m_brush.tip = (m_currentTip >= 0 && m_currentTip < static_cast<int>(m_tips.size())) ? m_tips[static_cast<std::size_t>(m_currentTip)].image
                                                                                             : nullptr;
    }
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
    std::vector<BrushTip> m_tips;
    int m_currentTip = -1;
};

} // namespace acestudio::ui
