#include "acestudio/core/AnimationQA.h"

#include "acestudio/core/Transform.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <sstream>

namespace acestudio {

namespace {
int maxChannelDiff(Rgba8 a, Rgba8 b) {
    return std::max({std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b)});
}

// Nearest palette colour by squared RGB distance (alpha ignored: palettes
// describe colours, the frame keeps its own alpha).
std::size_t nearestRgb(const std::vector<Rgba8>& pal, Rgba8 c) {
    std::size_t best = 0;
    long bestD = -1;
    for (std::size_t i = 0; i < pal.size(); ++i) {
        const long dr = long{pal[i].r} - c.r, dg = long{pal[i].g} - c.g, db = long{pal[i].b} - c.b;
        const long d = dr * dr + dg * dg + db * db;
        if (bestD < 0 || d < bestD) { bestD = d; best = i; }
    }
    return best;
}

std::vector<double> histogram(const Image& img, const std::vector<Rgba8>& pal) {
    std::vector<double> h(pal.size(), 0.0);
    double n = 0;
    for (auto p : img.pixels())
        if (p.a) { h[nearestRgb(pal, p)] += 1; n += 1; }
    if (n > 0) for (double& v : h) v /= n;
    return h;
}

// Bottom-centre of a mask's bounds (x centre, last row); {0,0} when empty.
std::pair<int, int> anchor(const Mask& m) {
    const IntRect b = m.bounds();
    if (b.empty()) return {0, 0};
    return {b.x + b.w / 2, b.y + b.h - 1};
}
} // namespace

Image removeBorderBackground(const Image& src, int tolerance) {
    Image out = src;
    const int w = src.width(), h = src.height();
    if (w == 0 || h == 0) return out;
    // Median border colour (per channel).
    std::vector<int> rs, gs, bs;
    for (int x = 0; x < w; ++x)
        for (int y : {0, h - 1}) { const Rgba8 c = src.pixel(x, y); rs.push_back(c.r); gs.push_back(c.g); bs.push_back(c.b); }
    for (int y = 1; y < h - 1; ++y)
        for (int x : {0, w - 1}) { const Rgba8 c = src.pixel(x, y); rs.push_back(c.r); gs.push_back(c.g); bs.push_back(c.b); }
    auto median = [](std::vector<int>& v) {
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
        return v[v.size() / 2];
    };
    const Rgba8 bg{static_cast<std::uint8_t>(median(rs)), static_cast<std::uint8_t>(median(gs)),
                   static_cast<std::uint8_t>(median(bs)), 255};
    // Flood fill from every border pixel that matches the background.
    std::vector<std::uint8_t> seen(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0);
    std::deque<std::pair<int, int>> q;
    auto push = [&](int x, int y) {
        const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
        if (seen[i]) return;
        seen[i] = 1;
        const Rgba8 c = src.pixel(x, y);
        if (c.a == 0 || maxChannelDiff(c, bg) <= tolerance) q.emplace_back(x, y);
    };
    for (int x = 0; x < w; ++x) { push(x, 0); push(x, h - 1); }
    for (int y = 0; y < h; ++y) { push(0, y); push(w - 1, y); }
    while (!q.empty()) {
        const auto [x, y] = q.front();
        q.pop_front();
        out.setPixel(x, y, Rgba8::transparent());
        if (x > 0) push(x - 1, y);
        if (x + 1 < w) push(x + 1, y);
        if (y > 0) push(x, y - 1);
        if (y + 1 < h) push(x, y + 1);
    }
    return out;
}

Mask silhouette(const Image& img) {
    Mask m(img.width(), img.height());
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x)
            if (img.pixel(x, y).a) m.set(x, y, true);
    return m;
}

double silhouetteIoU(const Mask& a, const Mask& b) {
    const IntRect ba = a.bounds(), bb = b.bounds();
    if (ba.empty() && bb.empty()) return 1.0;
    if (ba.empty() || bb.empty()) return 0.0;
    const auto [ax, ay] = anchor(a);
    const auto [bx, by] = anchor(b);
    const int dx = ax - bx, dy = ay - by;   // shift b onto a
    long inter = 0, uni = 0;
    const int x0 = std::min(ba.x, bb.x + dx), x1 = std::max(ba.x + ba.w, bb.x + bb.w + dx);
    const int y0 = std::min(ba.y, bb.y + dy), y1 = std::max(ba.y + ba.h, bb.y + bb.h + dy);
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const bool pa = a.get(x, y), pb = b.get(x - dx, y - dy);
            inter += (pa && pb) ? 1 : 0;
            uni += (pa || pb) ? 1 : 0;
        }
    return uni ? static_cast<double>(inter) / static_cast<double>(uni) : 1.0;
}

double paletteDistance(const Image& img, const Palette& palette) {
    const auto& pal = palette.colors();
    if (pal.empty()) return 0.0;
    double sum = 0;
    long n = 0;
    for (auto p : img.pixels()) {
        if (!p.a) continue;
        sum += maxChannelDiff(p, pal[nearestRgb(pal, p)]) / 255.0;
        ++n;
    }
    return n ? sum / static_cast<double>(n) : 0.0;
}

double colorMixDistance(const Image& a, const Image& b, const Palette& palette) {
    const auto& pal = palette.colors();
    if (pal.empty()) return 0.0;
    const auto ha = histogram(a, pal), hb = histogram(b, pal);
    double d = 0;
    for (std::size_t i = 0; i < pal.size(); ++i) d += std::abs(ha[i] - hb[i]);
    return d / 2.0;
}

double QAReport::meanScore() const {
    if (frames.empty()) return 1.0;
    double s = 0;
    for (const auto& f : frames) s += f.score;
    return s / static_cast<double>(frames.size());
}

std::vector<int> QAReport::failing() const {
    std::vector<int> v;
    for (std::size_t i = 0; i < frames.size(); ++i) if (!frames[i].ok()) v.push_back(static_cast<int>(i));
    return v;
}

std::vector<int> QAReport::toRegenerate() const {
    std::vector<int> v;
    for (std::size_t i = 0; i < frames.size(); ++i) if (frames[i].needsRegeneration()) v.push_back(static_cast<int>(i));
    return v;
}

QAReport evaluateFrames(const std::vector<Image>& frames, const Image& reference, const Palette& palette,
                        const QAOptions& o) {
    QAReport r;
    const Mask refMask = silhouette(reference);
    const IntRect rb = refMask.bounds();
    r.referenceGround = rb.empty() ? reference.height() - 1 : rb.y + rb.h - 1;
    r.referenceHeight = rb.empty() ? 0 : rb.h;
    r.referenceCentreX = rb.empty() ? reference.width() / 2 : rb.x + rb.w / 2;
    Mask previous;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const Image& f = frames[i];
        FrameQA q;
        const Mask m = silhouette(f);
        q.bounds = m.bounds();
        if (q.bounds.empty()) {
            q.flags = QAEmpty;
            q.score = 0;
            q.iouPrevious = 0;
            r.frames.push_back(q);
            previous = m;
            continue;
        }
        q.groundOffset = (q.bounds.y + q.bounds.h - 1) - r.referenceGround;
        q.heightRatio = r.referenceHeight ? static_cast<double>(q.bounds.h) / r.referenceHeight : 1.0;
        q.paletteDistance = paletteDistance(f, palette);
        q.colorMix = colorMixDistance(f, reference, palette);
        q.iouPrevious = i == 0 ? silhouetteIoU(m, refMask) : silhouetteIoU(m, previous);
        if (std::abs(q.groundOffset) > o.groundTolerance) q.flags |= QAGround;
        if (std::abs(q.heightRatio - 1.0) > o.heightTolerance) q.flags |= QAHeight;
        if (q.paletteDistance > o.paletteTolerance) q.flags |= QAPalette;
        if (q.colorMix > o.colorMixTolerance) q.flags |= QAColorMix;
        if (q.iouPrevious < o.minIoU) q.flags |= QASilhouette;
        // Score: each check contributes; silhouette and colour mix weigh most.
        auto clamp01 = [](double v) { return std::clamp(v, 0.0, 1.0); };
        const double sGround = clamp01(1.0 - std::abs(q.groundOffset) / 8.0);
        const double sHeight = clamp01(1.0 - std::abs(q.heightRatio - 1.0) / 0.5);
        const double sPal = clamp01(1.0 - q.paletteDistance / 0.25);
        const double sMix = clamp01(1.0 - q.colorMix);
        const double sIoU = clamp01(q.iouPrevious);
        q.score = 0.15 * sGround + 0.15 * sHeight + 0.1 * sPal + 0.25 * sMix + 0.35 * sIoU;
        r.frames.push_back(q);
        previous = m;
    }
    return r;
}

std::string describe(const FrameQA& q) {
    std::ostringstream s;
    if (q.flags & QAEmpty) return "empty frame";
    bool first = true;
    auto add = [&](const std::string& t) { s << (first ? "" : "; ") << t; first = false; };
    if (q.flags & QAGround) add("ground off by " + std::to_string(q.groundOffset) + " px");
    if (q.flags & QAHeight) add("height " + std::to_string(static_cast<int>(std::lround(q.heightRatio * 100))) + "% of reference");
    if (q.flags & QAPalette) add("off-palette colours");
    if (q.flags & QAColorMix) add("colours differ from the reference");
    if (q.flags & QASilhouette) add("shape changed (" + std::to_string(static_cast<int>(std::lround(q.iouPrevious * 100))) + "% overlap)");
    if (first) return "ok";
    return s.str();
}

Image mapToPalette(const Image& img, const Palette& palette) {
    const auto& pal = palette.colors();
    if (pal.empty()) return img;
    Image out = img;
    for (auto& p : out.pixels()) {
        if (!p.a) continue;
        const Rgba8 c = pal[nearestRgb(pal, p)];
        p = {c.r, c.g, c.b, p.a};
    }
    return out;
}

std::vector<Image> correctFrames(const std::vector<Image>& frames, const QAReport& report, const Palette& palette,
                                 const QAOptions& o) {
    std::vector<Image> out;
    out.reserve(frames.size());
    for (std::size_t i = 0; i < frames.size(); ++i) {
        Image f = palette.size() ? mapToPalette(frames[i], palette) : frames[i];
        if (i >= report.frames.size()) { out.push_back(std::move(f)); continue; }
        const FrameQA& q = report.frames[i];
        if (q.needsRegeneration() || q.bounds.empty()) { out.push_back(std::move(f)); continue; }
        // Height: rescale the figure (nearest) about its bottom-centre.
        Image figure = f.copy(q.bounds);
        if ((q.flags & QAHeight) && report.referenceHeight > 0) {
            const double s = static_cast<double>(report.referenceHeight) / q.bounds.h;
            const int nw = std::max(1, static_cast<int>(std::lround(q.bounds.w * s)));
            const int nh = report.referenceHeight;
            if (auto scaled = scaleNearest(figure, nw, nh)) figure = std::move(scaled).value();
        }
        (void)o;
        // Place: feet on the reference ground line. The horizontal centre is the
        // frame's own (a stride's bounds move with the legs; re-centring would jitter).
        Image placed(f.width(), f.height());
        const int x = q.bounds.x + q.bounds.w / 2 - figure.width() / 2;
        const int y = report.referenceGround - (figure.height() - 1);
        placed.blit(figure, x, y);
        out.push_back(std::move(placed));
    }
    return out;
}

} // namespace acestudio
