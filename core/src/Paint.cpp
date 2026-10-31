#include "acestudio/core/Paint.h"
#include "acestudio/core/PngCodec.h"
#include "acestudio/core/FileUtil.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace acestudio {

namespace {

// One painting undo step: the exact original cel (when its geometry had to
// be normalised to canvas size) or a before/after patch of the changed rect.
class PaintCommand final : public Command {
public:
    PaintCommand(std::string label, LayerId layer, int frame, bool normalized, std::optional<Cel> originalCel,
                 IntRect rect, Image before, Image after, int canvasW, int canvasH, PaintTarget target)
        : m_label(std::move(label)), m_layer(layer), m_frame(frame), m_normalized(normalized),
          m_originalCel(std::move(originalCel)), m_rect(rect), m_before(std::move(before)),
          m_after(std::move(after)), m_w(canvasW), m_h(canvasH), m_target(target) {}

    std::string label() const override { return m_label; }

    Status redo(Document& d) override {
        if (m_normalized) {
            const bool mask = m_target == PaintTarget::Mask;
            Image canvas(m_w, m_h, mask ? Rgba8{255, 255, 255, 255} : Rgba8::transparent());
            if (m_originalCel) canvas.blit(m_originalCel->image, m_originalCel->x, m_originalCel->y);
            canvas.blit(m_after, m_rect.x, m_rect.y);
            return mask ? d.setMaskCel(m_layer, m_frame, Cel{std::move(canvas), 0, 0})
                        : d.setCel(m_layer, m_frame, Cel{std::move(canvas), 0, 0});
        }
        Cel* c = m_target == PaintTarget::Mask ? d.maskCel(m_layer, m_frame) : d.cel(m_layer, m_frame);
        if (!c) return Error{ErrorCode::Internal, "Cel missing on redo"};
        c->image.blit(m_after, m_rect.x - c->x, m_rect.y - c->y);
        return {};
    }

    void undo(Document& d) override {
        const bool mask = m_target == PaintTarget::Mask;
        if (m_normalized) {
            if (m_originalCel) (void)(mask ? d.setMaskCel(m_layer, m_frame, *m_originalCel) : d.setCel(m_layer, m_frame, *m_originalCel));
            else if (mask) d.clearMaskCel(m_layer, m_frame);
            else d.clearCel(m_layer, m_frame);
            return;
        }
        if (Cel* c = mask ? d.maskCel(m_layer, m_frame) : d.cel(m_layer, m_frame))
            c->image.blit(m_before, m_rect.x - c->x, m_rect.y - c->y);
    }

    std::size_t memoryBytes() const override {
        return m_before.byteSize() + m_after.byteSize() + (m_originalCel ? m_originalCel->image.byteSize() : 0) + 128;
    }

private:
    std::string m_label;
    LayerId m_layer;
    int m_frame;
    bool m_normalized;
    std::optional<Cel> m_originalCel;
    IntRect m_rect;
    Image m_before, m_after;
    int m_w, m_h;
    PaintTarget m_target;
};

} // namespace

// ------------------------------------------------------------------ session

Result<std::unique_ptr<PaintSession>> PaintSession::begin(Editor& editor, LayerId layer, int frame, std::string label,
                                                          PaintTarget target) {
    const Document& d = editor.document();
    const Layer* l = d.layer(layer);
    if (!l) return Error{ErrorCode::NotFound, "No layer selected"};
    if (target == PaintTarget::Mask) {
        if (!l->hasMask) return Error{ErrorCode::InvalidArgument, "The layer has no mask"};
        for (const Layer* cur = l; cur; cur = cur->parent == kNoLayer ? nullptr : d.layer(cur->parent))
            if (cur->locked) return Error{ErrorCode::PermissionDenied, "The layer is locked"};
    } else {
        if (l->kind == LayerKind::Group) return Error{ErrorCode::InvalidArgument, "Groups cannot be painted on; select a layer"};
        if (l->kind == LayerKind::Adjustment)
            return Error{ErrorCode::InvalidArgument, "Adjustment layers have no pixels; paint on their mask"};
        if (!d.isLayerEditable(layer)) return Error{ErrorCode::PermissionDenied, "The layer is locked"};
    }
    if (!d.isLayerEffectivelyVisible(layer)) return Error{ErrorCode::PermissionDenied, "The layer is hidden"};
    if (frame < 0 || frame >= d.frameCount()) return Error{ErrorCode::InvalidArgument, "Frame out of range"};
    return std::unique_ptr<PaintSession>(new PaintSession(editor, layer, frame, std::move(label), target));
}

PaintSession::PaintSession(Editor& e, LayerId layer, int frame, std::string label, PaintTarget target)
    : m_editor(e), m_layer(layer), m_frame(frame), m_label(std::move(label)), m_target(target) {
    Document& d = m_editor.mutableDocument();
    const bool mask = target == PaintTarget::Mask;
    if (const Cel* c = currentCel()) m_originalCel = *c;
    m_original = mask ? d.renderMask(layer, frame) : d.renderLayer(layer, frame);
    m_alphaLocked = !mask && d.layer(layer)->alphaLocked;
    // Normalise to a canvas-sized cel at the origin so strokes can reach
    // any pixel. The original cel is restored exactly on undo/cancel.
    const Cel* c = currentCel();
    if (!c || c->x != 0 || c->y != 0 || c->image.width() != d.width() || c->image.height() != d.height())
        (void)(mask ? d.setMaskCel(layer, frame, Cel{m_original, 0, 0}) : d.setCel(layer, frame, Cel{m_original, 0, 0}));
}

const Cel* PaintSession::currentCel() const {
    const Document& d = m_editor.document();
    return m_target == PaintTarget::Mask ? d.maskCel(m_layer, m_frame) : d.cel(m_layer, m_frame);
}

PaintSession::~PaintSession() {
    if (!m_finished) cancel();
}

Image& PaintSession::work() {
    Document& d = m_editor.mutableDocument();
    return (m_target == PaintTarget::Mask ? d.maskCel(m_layer, m_frame) : d.cel(m_layer, m_frame))->image;
}
const Image& PaintSession::pixels() const { return currentCel()->image; }

bool PaintSession::wrap(int& x, int& y) const noexcept {
    const Document& d = m_editor.document();
    const int W = d.width(), H = d.height();
    if (wrapsX(d.tileMode())) x = ((x % W) + W) % W;
    if (wrapsY(d.tileMode())) y = ((y % H) + H) % H;
    return x >= 0 && y >= 0 && x < W && y < H;
}

namespace {
IntRect unite(const IntRect& a, const IntRect& b) noexcept {
    if (a.empty()) return b;
    if (b.empty()) return a;
    const int x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
    const int x1 = std::max(a.x + a.w, b.x + b.w), y1 = std::max(a.y + a.h, b.y + b.h);
    return {x0, y0, x1 - x0, y1 - y0};
}
} // namespace

void PaintSession::touch(int x, int y) noexcept {
    const IntRect p{x, y, 1, 1};
    m_dirty = unite(m_dirty, p);
    if (m_touched.empty()) { m_touched = p; return; }
    const int x0 = std::min(m_touched.x, x), y0 = std::min(m_touched.y, y);
    const int x1 = std::max(m_touched.x + m_touched.w, x + 1), y1 = std::max(m_touched.y + m_touched.h, y + 1);
    m_touched = {x0, y0, x1 - x0, y1 - y0};
}

Rgba8 PaintSession::constrain(Rgba8 c) const {
    const Document& d = m_editor.document();
    if (m_target == PaintTarget::Mask) {
        // Mask value = luma scaled by alpha; transparent (the eraser) hides.
        const auto v = static_cast<std::uint8_t>((c.luma() * c.a + 127) / 255);
        return {v, v, v, 255};
    }
    if (c.a == 0) return Rgba8::transparent();
    switch (d.colorMode()) {
    case ColorMode::Rgba: return c;
    case ColorMode::Grayscale: { const auto l = c.luma(); return {l, l, l, c.a}; }
    case ColorMode::Indexed: {
        // Nearest opaque palette entry; the palette defines the allowed colours.
        const auto& cols = d.palette().colors();
        std::optional<std::size_t> best;
        long bestDist = 0;
        for (std::size_t i = 0; i < cols.size(); ++i) {
            if (cols[i].a == 0) continue;
            const long dr = long{cols[i].r} - c.r, dg = long{cols[i].g} - c.g, db = long{cols[i].b} - c.b;
            const long dist = dr * dr + dg * dg + db * db;
            if (!best || dist < bestDist) { best = i; bestDist = dist; }
        }
        return best ? Rgba8{cols[*best].r, cols[*best].g, cols[*best].b, 255} : c;
    }
    }
    return c;
}

void PaintSession::writePixel(int x, int y, Rgba8 color) { put(x, y, color, m_brush.ink); }
void PaintSession::writeExact(int x, int y, Rgba8 color) { put(x, y, color, Ink::Simple); }

namespace {
// Straight-alpha source-over.
Rgba8 over(Rgba8 src, Rgba8 dst) noexcept {
    if (src.a == 255 || dst.a == 0) return src;
    if (src.a == 0) return dst;
    const unsigned sa = src.a, da = dst.a * (255u - sa) / 255u, oa = sa + da;
    auto ch = [&](std::uint8_t s, std::uint8_t d) {
        return static_cast<std::uint8_t>((s * sa + d * da + oa / 2) / oa);
    };
    return {ch(src.r, dst.r), ch(src.g, dst.g), ch(src.b, dst.b), static_cast<std::uint8_t>(oa)};
}
} // namespace

void PaintSession::put(int x, int y, Rgba8 color, Ink ink) {
    if (!wrap(x, y)) return;
    Image& img = work();
    if (!img.contains(x, y)) return;
    if (m_selection && !m_selection->isNull() && !m_selection->get(x, y)) return;
    const Rgba8 cur = img.pixel(x, y);
    if (ink == Ink::Shading && m_target == PaintTarget::Pixels) {
        if (m_shaded.isNull()) m_shaded = Mask(img.width(), img.height());
        if (m_shaded.get(x, y)) return;
        const Rgba8 base = m_original.pixel(x, y);
        if (base.a == 0) return;
        auto it = std::find_if(m_ramp.begin(), m_ramp.end(), [&](Rgba8 r) { return r.r == base.r && r.g == base.g && r.b == base.b; });
        if (it == m_ramp.end()) return;
        const long idx = (it - m_ramp.begin()) + m_shadeStep;
        if (idx < 0 || idx >= static_cast<long>(m_ramp.size())) return;
        m_shaded.set(x, y, true);
        color = m_ramp[static_cast<std::size_t>(idx)];
        color.a = base.a;
    } else if (ink == Ink::Blend && m_target == PaintTarget::Pixels && color.a > 0) {
        color = over(color, cur);
    }
    Rgba8 out = constrain(color);
    if (m_alphaLocked) {
        if (cur.a == 0 || out.a == 0) return;     // alpha lock: never create or erase coverage
        out.a = cur.a;
    }
    if (out == cur) return;
    img.setPixel(x, y, out);
    touch(x, y);
}

void PaintSession::restorePixel(int x, int y) {
    if (!wrap(x, y)) return;
    Image& img = work();
    if (!img.contains(x, y)) return;
    img.setPixel(x, y, m_original.pixel(x, y));
    touch(x, y);
}

void PaintSession::footprint(int cx, int cy, std::vector<Point>& out) const {
    if (m_brush.shape == BrushShape::Custom && m_brush.tip && !m_brush.tip->isNull()) {
        const Image& t = *m_brush.tip;
        const int ox = cx - (t.width() - 1) / 2, oy = cy - (t.height() - 1) / 2;
        for (int y = 0; y < t.height(); ++y)
            for (int x = 0; x < t.width(); ++x)
                if (t.pixel(x, y).a > 0) out.push_back({ox + x, oy + y});
        return;
    }
    const int n = std::clamp(m_brush.size, 1, 64);
    const int off = (n - 1) / 2;
    const double c = (n - 1) / 2.0, r2 = (n / 2.0) * (n / 2.0) + 0.25;
    for (int oy = 0; oy < n; ++oy)
        for (int ox = 0; ox < n; ++ox) {
            if (m_brush.shape == BrushShape::Circle && n > 2) {
                const double dx = ox - c, dy = oy - c;
                if (dx * dx + dy * dy > r2) continue;
            }
            out.push_back({cx - off + ox, cy - off + oy});
        }
}

void PaintSession::stamp(int x, int y, Rgba8 color) {
    const int W = m_editor.document().width(), H = m_editor.document().height();
    std::vector<Point> pts;
    footprint(x, y, pts);
    // Tip colours: each footprint point takes its own colour (erasing keeps
    // the eraser's transparent colour).
    const bool tipColors = m_brush.shape == BrushShape::Custom && m_brush.tip && m_brush.tipColors && color.a > 0;
    std::vector<Rgba8> cols;
    if (tipColors) {
        const Image& t = *m_brush.tip;
        const int ox = x - (t.width() - 1) / 2, oy = y - (t.height() - 1) / 2;
        for (const Point& p : pts) cols.push_back(t.pixel(p.x - ox, p.y - oy));
    }
    if (m_brush.ditherDensity < 16) {
        const int d = std::max(1, m_brush.ditherDensity);
        std::vector<Point> kept;
        std::vector<Rgba8> keptCols;
        for (std::size_t i = 0; i < pts.size(); ++i)
            if (bayer4(pts[i].x, pts[i].y) < d) {
                kept.push_back(pts[i]);
                if (tipColors) keptCols.push_back(cols[i]);
            }
        pts = std::move(kept);
        cols = std::move(keptCols);
    }
    const std::size_t base = pts.size();
    for (std::size_t i = 0; i < base; ++i) {
        const Point p = pts[i];
        const Rgba8 c = tipColors ? cols[i] : color;
        auto add = [&](Point q) { pts.push_back(q); if (tipColors) cols.push_back(c); };
        if (m_brush.mirror == MirrorMode::Horizontal || m_brush.mirror == MirrorMode::Both) add({W - 1 - p.x, p.y});
        if (m_brush.mirror == MirrorMode::Vertical || m_brush.mirror == MirrorMode::Both) add({p.x, H - 1 - p.y});
        if (m_brush.mirror == MirrorMode::Both) add({W - 1 - p.x, H - 1 - p.y});
    }
    for (std::size_t i = 0; i < pts.size(); ++i) writePixel(pts[i].x, pts[i].y, tipColors ? cols[i] : color);
}

void PaintSession::eraseStamp(int x, int y) {
    const Ink ink = m_brush.ink;
    m_brush.ink = Ink::Simple;   // erasing is always exact
    stamp(x, y, Rgba8::transparent());
    m_brush.ink = ink;
}

void PaintSession::revertAll() {
    if (m_touched.empty()) return;
    work().blit(m_original.copy(m_touched), m_touched.x, m_touched.y);
    m_dirty = unite(m_dirty, m_touched);
    m_touched = {};
}

bool PaintSession::commit() {
    if (m_finished) return false;
    m_finished = true;
    Document& d = m_editor.mutableDocument();
    const Image& cur = currentCel()->image;
    const bool mask = m_target == PaintTarget::Mask;

    // Exact changed rectangle within the touched area.
    int minX = std::numeric_limits<int>::max(), minY = minX, maxX = -1, maxY = -1;
    for (int y = m_touched.y; y < m_touched.y + m_touched.h; ++y)
        for (int x = m_touched.x; x < m_touched.x + m_touched.w; ++x)
            if (cur.pixel(x, y) != m_original.pixel(x, y)) {
                minX = std::min(minX, x); maxX = std::max(maxX, x);
                minY = std::min(minY, y); maxY = std::max(maxY, y);
            }
    const bool normalized = !m_originalCel || m_originalCel->x != 0 || m_originalCel->y != 0 ||
                            m_originalCel->image.width() != d.width() || m_originalCel->image.height() != d.height();
    if (maxX < 0) {   // nothing changed: leave the document exactly as it was
        if (normalized) {
            if (m_originalCel) (void)(mask ? d.setMaskCel(m_layer, m_frame, *m_originalCel) : d.setCel(m_layer, m_frame, *m_originalCel));
            else if (mask) d.clearMaskCel(m_layer, m_frame);
            else d.clearCel(m_layer, m_frame);
        }
        m_editor.notifyChanged();
        return false;
    }
    const IntRect rect{minX, minY, maxX - minX + 1, maxY - minY + 1};
    m_editor.recordApplied(std::make_unique<PaintCommand>(m_label, m_layer, m_frame, normalized, m_originalCel, rect,
                                                          m_original.copy(rect), cur.copy(rect), d.width(), d.height(), m_target));
    return true;
}

void PaintSession::cancel() {
    if (m_finished) return;
    m_finished = true;
    Document& d = m_editor.mutableDocument();
    const bool mask = m_target == PaintTarget::Mask;
    if (m_originalCel) (void)(mask ? d.setMaskCel(m_layer, m_frame, *m_originalCel) : d.setCel(m_layer, m_frame, *m_originalCel));
    else if (mask) d.clearMaskCel(m_layer, m_frame);
    else d.clearCel(m_layer, m_frame);
    m_editor.notifyChanged();
}

// ------------------------------------------------------------------ tools

FreehandStroke::FreehandStroke(PaintSession& s, Rgba8 color, bool erase, bool pixelPerfect)
    : m_s(s), m_color(color), m_erase(erase), m_pixelPerfect(pixelPerfect) {}

void FreehandStroke::put(Point p) {
    if (m_erase) m_s.eraseStamp(p.x, p.y);
    else m_s.stamp(p.x, p.y, m_color);
    if (!m_pixelPerfect || m_s.brush().size != 1) return;
    m_path.push_back(p);
    const int drop = pixelPerfectDropIndex(m_path);
    if (drop < 0) return;
    const Point q = m_path[static_cast<std::size_t>(drop)];
    m_path.erase(m_path.begin() + drop);
    // Do not undo a pixel the stroke legitimately painted earlier.
    if (std::find(m_path.begin(), m_path.end(), q) != m_path.end()) return;
    const int W = m_s.original().width(), H = m_s.original().height();
    const MirrorMode m = m_s.brush().mirror;
    m_s.restorePixel(q.x, q.y);
    if (m == MirrorMode::Horizontal || m == MirrorMode::Both) m_s.restorePixel(W - 1 - q.x, q.y);
    if (m == MirrorMode::Vertical || m == MirrorMode::Both) m_s.restorePixel(q.x, H - 1 - q.y);
    if (m == MirrorMode::Both) m_s.restorePixel(W - 1 - q.x, H - 1 - q.y);
}

void FreehandStroke::moveTo(Point p) {
    if (!m_started) {
        m_started = true;
        m_lastRaw = p;
        put(p);
        return;
    }
    if (p == *m_lastRaw) return;
    const auto pts = linePoints(*m_lastRaw, p);
    for (std::size_t i = 1; i < pts.size(); ++i) put(pts[i]);
    m_lastRaw = p;
}

void drawLine(PaintSession& s, Point a, Point b, Rgba8 color) {
    for (const Point& p : linePoints(a, b)) s.stamp(p.x, p.y, color);
}

namespace {
// Fills point sets pixel by pixel while still honouring mirror mode.
void stampPixels(PaintSession& s, const std::vector<Point>& pts, Rgba8 color) {
    BrushSettings b = s.brush();
    const BrushSettings saved = b;
    b.size = 1;
    s.setBrush(b);
    for (const Point& p : pts) s.stamp(p.x, p.y, color);
    s.setBrush(saved);
}
} // namespace

void drawRectangle(PaintSession& s, Point a, Point b, bool filled, Rgba8 color) {
    const IntRect r = rectFromCorners(a, b);
    if (filled) {
        std::vector<Point> pts;
        pts.reserve(static_cast<std::size_t>(r.w) * static_cast<std::size_t>(r.h));
        for (int y = r.y; y < r.y + r.h; ++y)
            for (int x = r.x; x < r.x + r.w; ++x) pts.push_back({x, y});
        stampPixels(s, pts, color);
        return;
    }
    for (const Point& p : rectOutlinePoints(r)) s.stamp(p.x, p.y, color);
}

void drawEllipse(PaintSession& s, Point a, Point b, bool filled, Rgba8 color) {
    const IntRect r = rectFromCorners(a, b);
    if (filled) stampPixels(s, ellipsePoints(r, true), color);
    else for (const Point& p : ellipsePoints(r, false)) s.stamp(p.x, p.y, color);
}

void drawGradient(PaintSession& s, Point a, Point b, Rgba8 c0, Rgba8 c1, GradientShape shape, GradientDither dither) {
    const Image& img = s.original();
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double len2 = dx * dx + dy * dy;
    const double radius = std::sqrt(len2);
    auto lerp = [](std::uint8_t p, std::uint8_t q, double t) {
        return static_cast<std::uint8_t>(std::lround(p + (q - p) * t));
    };
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) {
            double t = 0.0;
            if (len2 > 0) {
                if (shape == GradientShape::Linear) t = ((x - a.x) * dx + (y - a.y) * dy) / len2;
                else t = std::sqrt((x - a.x) * (x - a.x) + double(y - a.y) * (y - a.y)) / radius;
            }
            t = std::clamp(t, 0.0, 1.0);
            Rgba8 c;
            if (dither == GradientDither::Ordered) {
                // 17 levels: compare against the Bayer threshold.
                c = (t * 16.0 > bayer4(x, y) + 0.5) ? c1 : c0;
            } else {
                c = {lerp(c0.r, c1.r, t), lerp(c0.g, c1.g, t), lerp(c0.b, c1.b, t), lerp(c0.a, c1.a, t)};
            }
            s.writePixel(x, y, c);
        }
}

void fillRegion(PaintSession& s, Point seed, Rgba8 color, bool contiguous, int tolerance) {
    const Mask region = floodRegion(s.original(), seed, contiguous, tolerance);
    for (int y = 0; y < region.height(); ++y)
        for (int x = 0; x < region.width(); ++x)
            if (region.get(x, y)) s.writePixel(x, y, color);
}

// ------------------------------------------------------------------ clone, curve, polygon

CloneStroke::CloneStroke(PaintSession& s, Image source, Point offset)
    : m_s(s), m_source(std::move(source)), m_offset(offset) {}

void CloneStroke::put(Point p) {
    std::vector<Point> pts;
    m_s.footprintAt(p.x, p.y, pts);
    const int W = m_source.width(), H = m_source.height();
    for (const Point& q : pts) {
        int sx = q.x - m_offset.x, sy = q.y - m_offset.y;
        if (sx < 0 || sy < 0 || sx >= W || sy >= H) continue;   // nothing to copy from outside the canvas
        m_s.writePixel(q.x, q.y, m_source.pixel(sx, sy));
    }
}

void CloneStroke::moveTo(Point p) {
    if (!m_last) { put(p); m_last = p; return; }
    if (p == *m_last) return;
    const auto line = linePoints(*m_last, p);
    for (std::size_t i = 1; i < line.size(); ++i) put(line[i]);
    m_last = p;
}

void drawCurve(PaintSession& s, Point a, Point b, Point c1, Point c2, Rgba8 color) {
    for (const Point& p : curvePoints(a, b, c1, c2)) s.stamp(p.x, p.y, color);
}

void drawPolygon(PaintSession& s, const std::vector<Point>& points, bool filled, Rgba8 color) {
    if (points.empty()) return;
    if (points.size() == 1) { s.stamp(points[0].x, points[0].y, color); return; }
    if (filled && points.size() >= 3) {
        const Mask m = polygonMask(s.original().width(), s.original().height(), points);
        std::vector<Point> pts;
        for (int y = 0; y < m.height(); ++y)
            for (int x = 0; x < m.width(); ++x)
                if (m.get(x, y)) pts.push_back({x, y});
        stampPixels(s, pts, color);
        return;
    }
    for (std::size_t i = 0; i < points.size(); ++i) {
        const Point a = points[i], b = points[(i + 1) % points.size()];
        if (i + 1 == points.size() && points.size() == 2) break;   // a two-point polygon is a line
        for (const Point& p : linePoints(a, b)) s.stamp(p.x, p.y, color);
    }
}

// ------------------------------------------------------------------ brush tips

Result<Image> loadBrushTip(const std::filesystem::path& file) {
    auto bytes = readFileBytes(file);
    if (!bytes) return bytes.error();
    auto img = decodePng(bytes.value());
    if (!img) return img.error();
    if (img.value().width() > 256 || img.value().height() > 256)
        return Error{ErrorCode::InvalidArgument, "Brush tips can be at most 256x256 pixels"};
    if (img.value().opaqueBounds().empty()) return Error{ErrorCode::InvalidArgument, "The brush tip is empty"};
    return img;
}

Status saveBrushTip(const Image& tip, const std::filesystem::path& file) {
    auto png = encodePng(tip);
    if (!png) return png.error();
    return writeFileAtomic(file, png.value());
}

Result<Image> brushTipFromSelection(const Image& img, const Mask& selection) {
    IntRect r = selection.isNull() ? img.opaqueBounds() : selection.bounds();
    if (r.empty()) return Error{ErrorCode::InvalidArgument, "Nothing selected"};
    Image tip(r.w, r.h);
    for (int y = 0; y < r.h; ++y)
        for (int x = 0; x < r.w; ++x)
            if (selection.isNull() || selection.get(r.x + x, r.y + y)) tip.setPixel(x, y, img.pixel(r.x + x, r.y + y));
    const IntRect used = tip.opaqueBounds();
    if (used.empty()) return Error{ErrorCode::InvalidArgument, "The selection has no visible pixels"};
    if (used.w > 256 || used.h > 256) return Error{ErrorCode::InvalidArgument, "Brush tips can be at most 256x256 pixels"};
    return tip.copy(used);
}

Rgba8 sampleColor(const Document& doc, int frame, Point p, LayerId layerOrNone) {
    if (layerOrNone == kNoLayer) return doc.composite(frame).pixel(p.x, p.y);
    const Cel* c = doc.cel(layerOrNone, frame);
    return c ? c->image.pixel(p.x - c->x, p.y - c->y) : Rgba8::transparent();
}

} // namespace acestudio
