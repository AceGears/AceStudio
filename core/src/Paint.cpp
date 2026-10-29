#include "acestudio/core/Paint.h"

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
                 IntRect rect, Image before, Image after, int canvasW, int canvasH)
        : m_label(std::move(label)), m_layer(layer), m_frame(frame), m_normalized(normalized),
          m_originalCel(std::move(originalCel)), m_rect(rect), m_before(std::move(before)),
          m_after(std::move(after)), m_w(canvasW), m_h(canvasH) {}

    std::string label() const override { return m_label; }

    Status redo(Document& d) override {
        if (m_normalized) {
            Image canvas(m_w, m_h);
            if (m_originalCel) canvas.blit(m_originalCel->image, m_originalCel->x, m_originalCel->y);
            canvas.blit(m_after, m_rect.x, m_rect.y);
            return d.setCel(m_layer, m_frame, Cel{std::move(canvas), 0, 0});
        }
        Cel* c = d.cel(m_layer, m_frame);
        if (!c) return Error{ErrorCode::Internal, "Cel missing on redo"};
        c->image.blit(m_after, m_rect.x - c->x, m_rect.y - c->y);
        return {};
    }

    void undo(Document& d) override {
        if (m_normalized) {
            if (m_originalCel) (void)d.setCel(m_layer, m_frame, *m_originalCel);
            else d.clearCel(m_layer, m_frame);
            return;
        }
        if (Cel* c = d.cel(m_layer, m_frame)) c->image.blit(m_before, m_rect.x - c->x, m_rect.y - c->y);
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
};

} // namespace

// ------------------------------------------------------------------ session

Result<std::unique_ptr<PaintSession>> PaintSession::begin(Editor& editor, LayerId layer, int frame, std::string label) {
    const Document& d = editor.document();
    const Layer* l = d.layer(layer);
    if (!l) return Error{ErrorCode::NotFound, "No layer selected"};
    if (l->kind != LayerKind::Raster) return Error{ErrorCode::InvalidArgument, "Groups cannot be painted on; select a layer"};
    if (!d.isLayerEditable(layer)) return Error{ErrorCode::PermissionDenied, "The layer is locked"};
    if (!d.isLayerEffectivelyVisible(layer)) return Error{ErrorCode::PermissionDenied, "The layer is hidden"};
    if (frame < 0 || frame >= d.frameCount()) return Error{ErrorCode::InvalidArgument, "Frame out of range"};
    return std::unique_ptr<PaintSession>(new PaintSession(editor, layer, frame, std::move(label)));
}

PaintSession::PaintSession(Editor& e, LayerId layer, int frame, std::string label)
    : m_editor(e), m_layer(layer), m_frame(frame), m_label(std::move(label)) {
    Document& d = m_editor.mutableDocument();
    if (const Cel* c = d.cel(layer, frame)) m_originalCel = *c;
    m_original = d.renderLayer(layer, frame);
    m_alphaLocked = d.layer(layer)->alphaLocked;
    // Normalise to a canvas-sized cel at the origin so strokes can reach
    // any pixel. The original cel is restored exactly on undo/cancel.
    const Cel* c = d.cel(layer, frame);
    if (!c || c->x != 0 || c->y != 0 || c->image.width() != d.width() || c->image.height() != d.height())
        (void)d.setCel(layer, frame, Cel{m_original, 0, 0});
}

PaintSession::~PaintSession() {
    if (!m_finished) cancel();
}

Image& PaintSession::work() { return m_editor.mutableDocument().cel(m_layer, m_frame)->image; }
const Image& PaintSession::pixels() const { return m_editor.document().cel(m_layer, m_frame)->image; }

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

void PaintSession::writePixel(int x, int y, Rgba8 color) {
    Image& img = work();
    if (!img.contains(x, y)) return;
    if (m_selection && !m_selection->isNull() && !m_selection->get(x, y)) return;
    const Rgba8 cur = img.pixel(x, y);
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
    Image& img = work();
    if (!img.contains(x, y)) return;
    img.setPixel(x, y, m_original.pixel(x, y));
    touch(x, y);
}

void PaintSession::footprint(int cx, int cy, std::vector<Point>& out) const {
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
    if (m_brush.ditherDensity < 16) {
        const int d = std::max(1, m_brush.ditherDensity);
        std::erase_if(pts, [d](const Point& p) { return bayer4(p.x, p.y) >= d; });
    }
    const std::size_t base = pts.size();
    for (std::size_t i = 0; i < base; ++i) {
        const Point p = pts[i];
        if (m_brush.mirror == MirrorMode::Horizontal || m_brush.mirror == MirrorMode::Both) pts.push_back({W - 1 - p.x, p.y});
        if (m_brush.mirror == MirrorMode::Vertical || m_brush.mirror == MirrorMode::Both) pts.push_back({p.x, H - 1 - p.y});
        if (m_brush.mirror == MirrorMode::Both) pts.push_back({W - 1 - p.x, H - 1 - p.y});
    }
    for (const Point& p : pts) writePixel(p.x, p.y, color);
}

void PaintSession::eraseStamp(int x, int y) { stamp(x, y, Rgba8::transparent()); }

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
    const Image& cur = d.cel(m_layer, m_frame)->image;

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
            if (m_originalCel) (void)d.setCel(m_layer, m_frame, *m_originalCel);
            else d.clearCel(m_layer, m_frame);
        }
        m_editor.notifyChanged();
        return false;
    }
    const IntRect rect{minX, minY, maxX - minX + 1, maxY - minY + 1};
    m_editor.recordApplied(std::make_unique<PaintCommand>(m_label, m_layer, m_frame, normalized, m_originalCel, rect,
                                                          m_original.copy(rect), cur.copy(rect), d.width(), d.height()));
    return true;
}

void PaintSession::cancel() {
    if (m_finished) return;
    m_finished = true;
    Document& d = m_editor.mutableDocument();
    if (m_originalCel) (void)d.setCel(m_layer, m_frame, *m_originalCel);
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

Rgba8 sampleColor(const Document& doc, int frame, Point p, LayerId layerOrNone) {
    if (layerOrNone == kNoLayer) return doc.composite(frame).pixel(p.x, p.y);
    const Cel* c = doc.cel(layerOrNone, frame);
    return c ? c->image.pixel(p.x - c->x, p.y - c->y) : Rgba8::transparent();
}

} // namespace acestudio
