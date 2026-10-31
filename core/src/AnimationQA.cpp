#include "acestudio/core/AnimationQA.h"

#include "acestudio/core/PaletteIO.h"
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

Image removeDetachedBelow(const Image& src) {
    const int w = src.width(), h = src.height();
    std::vector<int> label(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), -1);
    struct Comp { int top, bottom; long n; };
    std::vector<Comp> comps;
    std::deque<std::pair<int, int>> q;
    auto at = [&](int x, int y) -> int& { return label[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)]; };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (src.pixel(x, y).a == 0 || at(x, y) >= 0) continue;
            const int id = static_cast<int>(comps.size());
            comps.push_back({y, y, 0});
            at(x, y) = id;
            q.emplace_back(x, y);
            while (!q.empty()) {
                const auto [cx, cy] = q.front();
                q.pop_front();
                Comp& c = comps[static_cast<std::size_t>(id)];
                c.top = std::min(c.top, cy);
                c.bottom = std::max(c.bottom, cy);
                ++c.n;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = cx + dx, ny = cy + dy;
                        if (nx < 0 || ny < 0 || nx >= w || ny >= h || src.pixel(nx, ny).a == 0 || at(nx, ny) >= 0) continue;
                        at(nx, ny) = id;
                        q.emplace_back(nx, ny);
                    }
            }
        }
    if (comps.size() < 2) return src;
    std::size_t main = 0;
    for (std::size_t i = 1; i < comps.size(); ++i) if (comps[i].n > comps[main].n) main = i;
    // Parts entirely below the figure (a floor line or shadow under a jump)
    // are not part of the character; a detached sword beside it is kept.
    Image out = src;
    bool changed = false;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int id = at(x, y);
            if (id >= 0 && static_cast<std::size_t>(id) != main && comps[static_cast<std::size_t>(id)].top > comps[main].bottom) {
                out.setPixel(x, y, Rgba8::transparent());
                changed = true;
            }
        }
    return changed ? out : src;
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

namespace {
struct ColourSide {
    double count = 0, offset = 0;   // pixels; mean signed x offset from the figure centre (fraction of width)
};
// Coarse colour bins (3 bits per channel) with their mean horizontal offset.
std::vector<ColourSide> colourSides(const Image& img) {
    std::vector<ColourSide> bins(512);
    int x0 = img.width(), x1 = -1;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x)
            if (img.pixel(x, y).a >= 128) { x0 = std::min(x0, x); x1 = std::max(x1, x); }
    if (x1 < x0) return bins;
    const double cx = (x0 + x1) / 2.0, w = std::max(1, x1 - x0 + 1);
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) {
            const Rgba8 p = img.pixel(x, y);
            if (p.a < 128) continue;
            ColourSide& b = bins[static_cast<std::size_t>((p.r >> 5) << 6 | (p.g >> 5) << 3 | (p.b >> 5))];
            b.count += 1;
            b.offset += (x - cx) / w;
        }
    for (auto& b : bins) if (b.count > 0) b.offset /= b.count;
    return bins;
}
} // namespace

namespace {
std::array<double, 40> colourProfile(const Image& img) {
    std::array<double, 40> h{};
    double n = 0;
    for (const Rgba8 p : img.pixels()) {
        if (p.a < 128) continue;
        const int mx = std::max({p.r, p.g, p.b}), mn = std::min({p.r, p.g, p.b});
        const double l = (mx + mn) / 510.0, chroma = (mx - mn) / 255.0;
        std::size_t bin;
        if (chroma < 0.12) {
            bin = 36 + static_cast<std::size_t>(std::min(3, static_cast<int>(l * 4)));
        } else {
            double hue;   // 0..6
            const double r = p.r / 255.0, g = p.g / 255.0, b = p.b / 255.0, d = chroma;
            if (mx == p.r) hue = std::fmod((g - b) / d + 6.0, 6.0);
            else if (mx == p.g) hue = (b - r) / d + 2.0;
            else hue = (r - g) / d + 4.0;
            const int hb = std::min(11, static_cast<int>(hue * 2.0));
            bin = static_cast<std::size_t>(hb * 3 + std::min(2, static_cast<int>(l * 3)));
        }
        h[bin] += 1;
        n += 1;
    }
    if (n > 0) for (double& v : h) v /= n;
    return h;
}
} // namespace

double colourProfileDistance(const Image& a, const Image& b) {
    const auto ha = colourProfile(a), hb = colourProfile(b);
    double d = 0;
    for (std::size_t i = 0; i < ha.size(); ++i) d += std::abs(ha[i] - hb[i]);
    return d / 2.0;
}

double facingAgreement(const Image& img, const Image& reference) {
    const auto a = colourSides(img), b = colourSides(reference);
    double num = 0, den = 0, refAsym = 0, refTotal = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        refTotal += b[i].count;
        refAsym += b[i].count * std::abs(b[i].offset);
        const double w = std::min(a[i].count, b[i].count);
        if (w <= 0) continue;
        num += w * a[i].offset * b[i].offset;
        den += w * std::abs(a[i].offset) * std::abs(b[i].offset);
    }
    // A symmetric reference (mean colour offset under 3% of the width) or
    // no shared colours: facing cannot be told.
    if (refTotal <= 0 || refAsym / refTotal < 0.03 || den <= 1e-9) return 1.0;
    return num / den;
}

std::vector<FrameExpectation> expectationsFromPoses(const std::vector<Pose>& poses, const std::array<double, 4>& box,
                                                    int spriteHeight) {
    std::vector<FrameExpectation> out(poses.size());
    if (poses.empty() || spriteHeight <= 0 || box[3] <= box[1]) return out;
    auto extent = [](const Pose& p, double& top, double& bottom) {
        top = 1; bottom = 0;
        for (const Keypoint& k : p.points)
            if (k.visible) { top = std::min(top, k.y); bottom = std::max(bottom, k.y); }
    };
    double t0, b0;
    extent(poses.front(), t0, b0);
    const double h0 = b0 - t0;
    const double pxPerUnit = spriteHeight / (box[3] - box[1]);
    for (std::size_t i = 0; i < poses.size(); ++i) {
        double t, b;
        extent(poses[i], t, b);
        out[i].groundOffset = static_cast<int>(std::lround((b - b0) * pxPerUnit));
        out[i].heightRatio = h0 > 1e-6 ? (b - t) / h0 : 1.0;
    }
    // Thick skeletons in sprite space (limbs as capsules about a fifth of the
    // figure's width), to know how much consecutive poses overlap.
    const int side = std::max(16, spriteHeight);
    const double bw = box[2] - box[0], bh = box[3] - box[1];
    const double radius = std::max(1.5, 0.05 * h0 * side / std::max(1e-6, bh));
    auto body = [&](const Pose& p) {
        Mask m(side, side);
        auto map = [&](const Keypoint& k) { return std::pair<double, double>{(k.x - box[0]) / bw * side, (k.y - box[1]) / bh * side}; };
        for (const auto& [a, bj] : poseLimbs()) {
            const Keypoint &ka = p[a], &kb = p[bj];
            if (!ka.visible || !kb.visible) continue;
            const auto [x1, y1] = map(ka);
            const auto [x2, y2] = map(kb);
            const int minx = static_cast<int>(std::floor(std::min(x1, x2) - radius)), maxx = static_cast<int>(std::ceil(std::max(x1, x2) + radius));
            const int miny = static_cast<int>(std::floor(std::min(y1, y2) - radius)), maxy = static_cast<int>(std::ceil(std::max(y1, y2) + radius));
            const double dx = x2 - x1, dy = y2 - y1, len2 = dx * dx + dy * dy;
            for (int y = std::max(0, miny); y <= std::min(side - 1, maxy); ++y)
                for (int x = std::max(0, minx); x <= std::min(side - 1, maxx); ++x) {
                    const double t = len2 > 0 ? std::clamp(((x + 0.5 - x1) * dx + (y + 0.5 - y1) * dy) / len2, 0.0, 1.0) : 0.0;
                    const double ex = x1 + t * dx - (x + 0.5), ey = y1 + t * dy - (y + 0.5);
                    if (ex * ex + ey * ey <= radius * radius) m.set(x, y, true);
                }
        }
        return m;
    };
    std::vector<Mask> bodies;
    bodies.reserve(poses.size());
    for (const Pose& p : poses) bodies.push_back(body(p));
    for (std::size_t i = 0; i < poses.size(); ++i) {
        if (i > 0) out[i].poseOverlapPrev = silhouetteIoU(bodies[i], bodies[i - 1]);
        if (i + 1 < poses.size()) out[i].poseOverlapNext = silhouetteIoU(bodies[i], bodies[i + 1]);
        else if (poses.size() > 2) out[i].poseOverlapNext = silhouetteIoU(bodies[i], bodies[0]);   // looping cycles
    }
    return out;
}

QAReport evaluateFrames(const std::vector<Image>& frames, const Image& reference, const Palette& palette,
                        const QAOptions& o) {
    QAReport r;
    const Mask refMask = silhouette(reference);
    const IntRect rb = refMask.bounds();
    r.referenceGround = rb.empty() ? reference.height() - 1 : rb.y + rb.h - 1;
    r.referenceHeight = rb.empty() ? 0 : rb.h;
    r.referenceCentreX = rb.empty() ? reference.width() / 2 : rb.x + rb.w / 2;
    const std::size_t n = frames.size();
    r.frames.resize(n);
    // Pass 1: per-frame checks, and each frame's shape as it will be after
    // correction (mirrored back when drawn facing the wrong way).
    std::vector<Mask> shapes(n);
    for (std::size_t i = 0; i < n; ++i) {
        const Image& f = frames[i];
        FrameQA& q = r.frames[i];
        shapes[i] = silhouette(f);
        q.bounds = shapes[i].bounds();
        if (q.bounds.empty()) {
            q.flags = QAEmpty;
            continue;
        }
        if (i < o.expected.size()) q.expected = o.expected[i];
        q.groundOffset = (q.bounds.y + q.bounds.h - 1) - (r.referenceGround + q.expected.groundOffset);
        const double expectedH = r.referenceHeight * q.expected.heightRatio;
        q.heightRatio = expectedH > 0 ? q.bounds.h / expectedH : 1.0;
        q.facing = i == 0 ? 1.0 : facingAgreement(f, reference);
        q.paletteDistance = paletteDistance(f, palette);
        q.colorMix = colourProfileDistance(f, reference);
        if (!o.identity.empty()) {
            q.identity = 1.0;
            for (const Image& id : o.identity) q.identity = std::min(q.identity, colourProfileDistance(f, id));
        }
        if (std::abs(q.groundOffset) > o.groundTolerance) q.flags |= QAGround;
        if (std::abs(q.heightRatio - 1.0) > o.heightTolerance) q.flags |= QAHeight;
        if (q.paletteDistance > o.paletteTolerance) q.flags |= QAPalette;
        if (q.colorMix > o.colorMixTolerance) q.flags |= QAColorMix;
        if (!o.identity.empty() && q.identity > o.identityTolerance) q.flags |= QAOffModel;
        if (q.facing < o.flipThreshold) {
            q.flags |= QAFlipped;
            shapes[i] = silhouette(flipHorizontal(f));
        }
    }
    // Pass 2: shape continuity. A frame is fine if it matches either
    // neighbour (the last frame's next is frame 1 in a looping cycle), so one
    // badly drawn frame does not also fail the good frame after it.
    for (std::size_t i = 0; i < n; ++i) {
        FrameQA& q = r.frames[i];
        if (q.flags & QAEmpty) {
            q.score = 0;
            q.iouPrevious = 0;
            continue;
        }
        bool shapeOk = true;
        if (i == 0) {
            q.iouPrevious = 1.0;   // the reference itself
        } else {
            // Fine if the frame matches either neighbour as well as their
            // poses allow (a big motion overlaps less).
            double best = 0;
            shapeOk = false;
            auto consider = [&](std::size_t j, double poseOverlap) {
                if (j == i || r.frames[j].bounds.empty()) return;
                const double iou = silhouetteIoU(shapes[i], shapes[j]);
                best = std::max(best, iou);
                const double need = std::min(o.minIoU, o.poseOverlapFactor * poseOverlap);
                if (iou >= need) shapeOk = true;
            };
            consider(i - 1, q.expected.poseOverlapPrev);
            if (i + 1 < n) consider(i + 1, q.expected.poseOverlapNext);
            else if (o.cyclic && n > 2) consider(0, q.expected.poseOverlapNext);
            q.iouPrevious = best;
        }
        if (!shapeOk) q.flags |= QASilhouette;
        // Score: each check contributes; silhouette and colour mix weigh most.
        auto clamp01 = [](double v) { return std::clamp(v, 0.0, 1.0); };
        const double sGround = clamp01(1.0 - std::abs(q.groundOffset) / 8.0);
        const double sHeight = clamp01(1.0 - std::abs(q.heightRatio - 1.0) / 0.5);
        const double sPal = clamp01(1.0 - q.paletteDistance / 0.25);
        const double sMix = clamp01(1.0 - 1.5 * std::max(q.colorMix, q.identity));
        const double sIoU = clamp01(q.iouPrevious);
        q.score = 0.15 * sGround + 0.15 * sHeight + 0.1 * sPal + 0.25 * sMix + 0.35 * sIoU;
        if (q.flags & QAFlipped) q.score *= 0.7;
    }
    return r;
}

std::string describe(const FrameQA& q) {
    std::ostringstream s;
    if (q.flags & QAEmpty) return "empty frame";
    bool first = true;
    auto add = [&](const std::string& t) { s << (first ? "" : "; ") << t; first = false; };
    if (q.flags & QAFlipped) add("facing the wrong way");
    if (q.flags & QAGround) add("ground off by " + std::to_string(q.groundOffset) + " px");
    if (q.flags & QAHeight) add("height " + std::to_string(static_cast<int>(std::lround(q.heightRatio * 100))) + "% of expected");
    if (q.flags & QAPalette) add("off-palette colours");
    if (q.flags & QAColorMix) add("colours differ from frame 1");
    if (q.flags & QAOffModel) add("does not look like the character's reference images");
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
        Image figure = f.copy(q.bounds);
        if (q.flags & QAFlipped) figure = flipHorizontal(figure);
        // Height: rescale the figure (nearest) about its bottom-centre to the
        // height its pose expects.
        if ((q.flags & QAHeight) && report.referenceHeight > 0) {
            const int nh = std::max(1, static_cast<int>(std::lround(report.referenceHeight * q.expected.heightRatio)));
            const double s = static_cast<double>(nh) / q.bounds.h;
            const int nw = std::max(1, static_cast<int>(std::lround(q.bounds.w * s)));
            if (auto scaled = scaleNearest(figure, nw, nh)) figure = std::move(scaled).value();
        }
        (void)o;
        // Place: feet where the pose expects them (the reference ground, or
        // above it for a jump). The horizontal centre is the frame's own (a
        // stride's bounds move with the legs; re-centring would jitter).
        // Never push the figure off the canvas: a correction that would clip
        // it is limited (the frame then stays flagged) instead of cutting
        // off the head or feet.
        if (figure.height() > f.height() || figure.width() > f.width()) figure = f.copy(q.bounds);
        Image placed(f.width(), f.height());
        const int x = std::clamp(q.bounds.x + q.bounds.w / 2 - figure.width() / 2, 0, f.width() - figure.width());
        const int ground = (q.flags & QAGround) ? report.referenceGround + q.expected.groundOffset
                                                : q.bounds.y + q.bounds.h - 1;
        const int y = std::clamp(ground - (figure.height() - 1), 0, f.height() - figure.height());
        placed.blit(figure, x, y);
        out.push_back(std::move(placed));
    }
    return out;
}

} // namespace acestudio
