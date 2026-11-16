#include "acestudio/core/Inbetween.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <random>

namespace acestudio::inbetween {

namespace {

std::size_t at(int w, int x, int y) { return static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x); }

bool opaque(Rgba8 c) { return c.a >= 128; }

std::uint32_t key(Rgba8 c) {
    return (std::uint32_t(c.r) << 24) | (std::uint32_t(c.g) << 16) | (std::uint32_t(c.b) << 8) | 255u;
}

Rgba8 fromKey(std::uint32_t k) {
    return Rgba8{static_cast<std::uint8_t>(k >> 24), static_cast<std::uint8_t>((k >> 16) & 255u), static_cast<std::uint8_t>((k >> 8) & 255u), 255};
}

bool stop(const Options& opt) { return opt.cancelled && opt.cancelled(); }

Error cancelledError() { return Error(ErrorCode::Cancelled, "In-betweening was cancelled."); }

// Difference of two pixels: opaque against clear costs most; two opaque
// pixels cost their colour distance (0..1).
float pixelCost(Rgba8 p, Rgba8 q) {
    const bool po = opaque(p), qo = opaque(q);
    if (po != qo) return 1.0f;
    if (!po) return 0.0f;
    const int d = std::abs(int(p.r) - int(q.r)) + std::abs(int(p.g) - int(q.g)) + std::abs(int(p.b) - int(q.b));
    return static_cast<float>(d) / (3.0f * 255.0f);
}

class Matcher {
public:
    Matcher(const Image& a, const Image& b, const Options& opt) : m_a(a), m_b(b), m_opt(opt) {}

    // Mean patch difference of a's pixel (x, y) and b's pixel (x+dx, y+dy),
    // plus a tiny pull towards the figure's overall move (it only breaks
    // ties, as in flat colour, where many moves match equally well).
    float cost(int x, int y, int dx, int dy) const {
        const int r = m_opt.patchRadius;
        float s = 0;
        int n = 0;
        for (int oy = -r; oy <= r; ++oy)
            for (int ox = -r; ox <= r; ++ox) {
                const Rgba8 p = m_a.pixel(x + ox, y + oy);
                const Rgba8 q = m_b.pixel(x + dx + ox, y + dy + oy);
                // The centre counts most: the pixel itself must match.
                const float w = (ox == 0 && oy == 0) ? 4.0f : 1.0f;
                s += w * pixelCost(p, q);
                n += (ox == 0 && oy == 0) ? 4 : 1;
            }
        return s / static_cast<float>(n) + 1e-4f * static_cast<float>(std::abs(dx - m_gx) + std::abs(dy - m_gy));
    }

    Result<Flow> run() {
        const int w = m_a.width(), h = m_a.height();
        Flow f;
        f.width = w;
        f.height = h;
        const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
        f.dx.assign(n, 0);
        f.dy.assign(n, 0);
        f.cost.assign(n, 0.0f);
        const int radius = m_opt.searchRadius > 0 ? m_opt.searchRadius : std::max(4, std::max(w, h) / 3);

        // Start every pixel at the shift between the two figures' boxes (or
        // nowhere, whichever matches better).
        const IntRect ba = m_a.opaqueBounds(), bb = m_b.opaqueBounds();
        if (!ba.empty() && !bb.empty()) {
            m_gx = (bb.x + bb.w / 2) - (ba.x + ba.w / 2);
            m_gy = (bb.y + bb.h) - (ba.y + ba.h);   // the feet, rather than the middle
        }
        const int gx = m_gx, gy = m_gy;
        std::vector<std::size_t> active;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                if (opaque(m_a.pixel(x, y))) active.push_back(at(w, x, y));
        for (std::size_t i : active) {
            const int x = static_cast<int>(i % static_cast<std::size_t>(w)), y = static_cast<int>(i / static_cast<std::size_t>(w));
            const float c0 = cost(x, y, 0, 0), c1 = cost(x, y, gx, gy);
            if (c1 < c0) {
                f.dx[i] = gx;
                f.dy[i] = gy;
                f.cost[i] = c1;
            } else {
                f.cost[i] = c0;
            }
        }

        // Uniqueness: how many pixels of a land on each pixel of b. In flat
        // colour many moves match equally well, and without this a turning
        // limb collapses onto the nearest pixels of its colour. Each other
        // pixel already landing on a target adds a little to the cost of
        // landing there too (f.cost stays the plain patch difference).
        std::vector<int> occupied(n, 0);
        auto target = [&](int x, int y, int dx, int dy) -> std::ptrdiff_t {
            const int tx = x + dx, ty = y + dy;
            if (tx < 0 || ty < 0 || tx >= w || ty >= h) return -1;
            return static_cast<std::ptrdiff_t>(at(w, tx, ty));
        };
        auto crowd = [&](std::ptrdiff_t t) { return t < 0 ? 0 : occupied[static_cast<std::size_t>(t)]; };
        for (std::size_t i : active) {
            const int x = static_cast<int>(i % static_cast<std::size_t>(w)), y = static_cast<int>(i / static_cast<std::size_t>(w));
            if (const auto t = target(x, y, f.dx[i], f.dy[i]); t >= 0) ++occupied[static_cast<std::size_t>(t)];
        }
        constexpr float kCrowdCost = 0.1f;
        std::mt19937 rng(m_opt.seed);
        auto tryMove = [&](std::size_t i, int x, int y, int dx, int dy) {
            if (dx == f.dx[i] && dy == f.dy[i]) return;
            const std::ptrdiff_t now = target(x, y, f.dx[i], f.dy[i]), next = target(x, y, dx, dy);
            const float c = cost(x, y, dx, dy);
            const float current = f.cost[i] + kCrowdCost * static_cast<float>(std::max(0, crowd(now) - 1));
            if (c + kCrowdCost * static_cast<float>(crowd(next)) < current) {
                if (now >= 0) --occupied[static_cast<std::size_t>(now)];
                if (next >= 0) ++occupied[static_cast<std::size_t>(next)];
                f.dx[i] = dx;
                f.dy[i] = dy;
                f.cost[i] = c;
            }
        };
        const int iters = std::clamp(m_opt.iterations, 1, 16);
        for (int it = 0; it < iters; ++it) {
            if (stop(m_opt)) return cancelledError();
            const bool forward = it % 2 == 0;
            const int step = forward ? 1 : -1;
            for (std::size_t k = 0; k < active.size(); ++k) {
                const std::size_t i = active[forward ? k : active.size() - 1 - k];
                const int x = static_cast<int>(i % static_cast<std::size_t>(w)), y = static_cast<int>(i / static_cast<std::size_t>(w));
                // Propagation: a neighbour's move, already improved this pass.
                for (const auto& [nx, ny] : std::array<std::pair<int, int>, 2>{{{x - step, y}, {x, y - step}}}) {
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                    const std::size_t j = at(w, nx, ny);
                    if (opaque(m_a.pixel(nx, ny))) tryMove(i, x, y, f.dx[j], f.dy[j]);
                }
                // Random search round the current move, ever closer. The
                // offsets are drawn one after the other and mapped by hand:
                // std::uniform_int_distribution and the order of function
                // arguments both differ between standard libraries and
                // compilers, and results must be the same on every build.
                for (int r = radius; r >= 1; r /= 2) {
                    const auto span = static_cast<std::uint32_t>(2 * r + 1);
                    const int ox = static_cast<int>(static_cast<std::uint32_t>(rng()) % span) - r;
                    const int oy = static_cast<int>(static_cast<std::uint32_t>(rng()) % span) - r;
                    tryMove(i, x, y, f.dx[i] + ox, f.dy[i] + oy);
                }
            }
        }

        // Clean-up: a pixel takes its neighbours' median move when that
        // matches nearly as well (flat colour matches anywhere; its
        // neighbours know better).
        std::vector<int> mx(f.dx), my(f.dy);
        for (std::size_t i : active) {
            const int x = static_cast<int>(i % static_cast<std::size_t>(w)), y = static_cast<int>(i / static_cast<std::size_t>(w));
            std::vector<int> xs, ys;
            for (int oy = -2; oy <= 2; ++oy)
                for (int ox = -2; ox <= 2; ++ox) {
                    const int nx = x + ox, ny = y + oy;
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h || !opaque(m_a.pixel(nx, ny))) continue;
                    xs.push_back(f.dx[at(w, nx, ny)]);
                    ys.push_back(f.dy[at(w, nx, ny)]);
                }
            const auto mid = static_cast<std::ptrdiff_t>(xs.size() / 2);
            std::nth_element(xs.begin(), xs.begin() + mid, xs.end());
            std::nth_element(ys.begin(), ys.begin() + mid, ys.end());
            const int ex = xs[static_cast<std::size_t>(mid)], ey = ys[static_cast<std::size_t>(mid)];
            if (ex == f.dx[i] && ey == f.dy[i]) continue;
            const float c = cost(x, y, ex, ey);
            if (c <= f.cost[i] + 0.02f) {
                mx[i] = ex;
                my[i] = ey;
            }
        }
        for (std::size_t i : active) {
            if (mx[i] == f.dx[i] && my[i] == f.dy[i]) continue;
            const int x = static_cast<int>(i % static_cast<std::size_t>(w)), y = static_cast<int>(i / static_cast<std::size_t>(w));
            f.dx[i] = mx[i];
            f.dy[i] = my[i];
            f.cost[i] = cost(x, y, mx[i], my[i]);
        }
        return f;
    }

private:
    const Image& m_a;
    const Image& m_b;
    const Options& m_opt;
    int m_gx = 0, m_gy = 0;
};

// Votes of one fine-scale canvas: per output pixel, colour -> weight.
struct Votes {
    int w = 0, h = 0, s = 1;
    std::vector<std::map<std::uint32_t, float>> cell;   // per output pixel
    std::vector<float> covered;                         // weighted samples per output pixel

    Votes(int width, int height, int scale)
        : w(width), h(height), s(scale), cell(static_cast<std::size_t>(width) * static_cast<std::size_t>(height)),
          covered(cell.size(), 0.0f) {}
};

// Draws `src`'s opaque pixels moved by t x flow onto the fine canvas.
// Pixels whose move the other direction does not confirm count less.
void splat(const Image& src, const Flow& f, const Flow& back, double t, float weight, Votes& v) {
    const int w = src.width(), h = src.height(), s = v.s;
    // Fine canvas: which colour (and weight) landed on each fine sample.
    std::vector<std::uint32_t> col(static_cast<std::size_t>(w * s) * static_cast<std::size_t>(h * s), 0u);
    std::vector<float> wt(col.size(), 0.0f);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const Rgba8 c = src.pixel(x, y);
            if (!opaque(c)) continue;
            const std::size_t i = at(w, x, y);
            const int dx = f.dx[i], dy = f.dy[i];
            // Forward-backward check: where it lands, does the way back lead home?
            float conf = 1.0f;
            const int tx = x + dx, ty = y + dy;
            if (tx >= 0 && ty >= 0 && tx < w && ty < h) {
                const std::size_t j = at(w, tx, ty);
                const int ex = dx + back.dx[j], ey = dy + back.dy[j];
                // Far moves (a limb's tip as it turns) may come back a few
                // pixels off in flat colour and still be right.
                const int tolerance = std::max(2, (std::abs(dx) + std::abs(dy)) / 3);
                if (std::abs(ex) + std::abs(ey) > tolerance) conf = 0.35f;
            } else {
                conf = 0.35f;
            }
            if (f.cost[i] > 0.25f) conf *= 0.5f;
            const double fx = (x + t * dx) * s, fy = (y + t * dy) * s;
            const int x0 = static_cast<int>(std::lround(fx)), y0 = static_cast<int>(std::lround(fy));
            for (int oy = 0; oy < s; ++oy)
                for (int ox = 0; ox < s; ++ox) {
                    const int px = x0 + ox, py = y0 + oy;
                    if (px < 0 || py < 0 || px >= w * s || py >= h * s) continue;
                    const std::size_t k = at(w * s, px, py);
                    // The surer pixel stays on top where two land on one sample.
                    if (conf * weight >= wt[k]) {
                        col[k] = key(c);
                        wt[k] = conf * weight;
                    }
                }
        }
    for (int py = 0; py < h * s; ++py)
        for (int px = 0; px < w * s; ++px) {
            const std::size_t k = at(w * s, px, py);
            if (wt[k] <= 0) continue;
            const std::size_t o = at(w, px / s, py / s);
            v.cell[o][col[k]] += wt[k];
            v.covered[o] += wt[k];
        }
}

int neighbours(const Image& img, int x, int y) {
    int n = 0;
    for (int oy = -1; oy <= 1; ++oy)
        for (int ox = -1; ox <= 1; ++ox)
            if ((ox || oy) && opaque(img.pixel(x + ox, y + oy))) ++n;
    return n;
}

// Pinholes inside a shape are filled with the colour round them; lone specks
// outside go.
void tidy(Image& img) {
    for (int pass = 0; pass < 2; ++pass) {
        Image next = img;
        for (int y = 0; y < img.height(); ++y)
            for (int x = 0; x < img.width(); ++x) {
                const int n = neighbours(img, x, y);
                const bool o = opaque(img.pixel(x, y));
                if (!o && n >= 6) {
                    std::map<std::uint32_t, int> c;
                    for (int oy = -1; oy <= 1; ++oy)
                        for (int ox = -1; ox <= 1; ++ox)
                            if (opaque(img.pixel(x + ox, y + oy))) ++c[key(img.pixel(x + ox, y + oy))];
                    const auto best = std::max_element(c.begin(), c.end(), [](const auto& l, const auto& r) { return l.second < r.second; });
                    next.setPixel(x, y, fromKey(best->first));
                } else if (o && n == 0) {
                    next.setPixel(x, y, Rgba8::transparent());
                }
            }
        img = std::move(next);
    }
}

int area(const Image& img) {
    int n = 0;
    for (const Rgba8& c : img.pixels()) n += opaque(c) ? 1 : 0;
    return n;
}

Result<void> checkOptions(const Options& opt) {
    if (opt.intermediates < 1 || opt.intermediates > 7) return Error(ErrorCode::InvalidArgument, "Choose 1 to 7 in-betweens.");
    if (opt.supersample < 1 || opt.supersample > 4) return Error(ErrorCode::InvalidArgument, "The fine scale must be 1 to 4.");
    if (opt.patchRadius < 0 || opt.patchRadius > 4) return Error(ErrorCode::InvalidArgument, "The patch radius must be 0 to 4.");
    if (opt.opaqueShare <= 0 || opt.opaqueShare >= 1) return Error(ErrorCode::InvalidArgument, "The opaque share must be between 0 and 1.");
    return {};
}

} // namespace

Result<Flow> estimateFlow(const Image& from, const Image& to, const Options& opt) {
    if (from.isNull() || to.isNull()) return Error(ErrorCode::InvalidArgument, "Both frames need pixels.");
    if (from.width() != to.width() || from.height() != to.height())
        return Error(ErrorCode::InvalidArgument, "Both frames must be the same size.");
    if (auto ok = checkOptions(opt); !ok) return ok.error();
    if (static_cast<long long>(from.width()) * from.height() > 1024LL * 1024LL)
        return Error(ErrorCode::InvalidArgument, "In-betweening is for sprites up to 1024 x 1024 pixels.");
    Matcher m(from, to, opt);
    return m.run();
}

Result<PairResult> inbetweenPair(const Image& a, const Image& b, const Options& opt) {
    auto fwd = estimateFlow(a, b, opt);
    if (!fwd) return fwd.error();
    if (stop(opt)) return cancelledError();
    auto bwd = estimateFlow(b, a, opt);
    if (!bwd) return bwd.error();
    const int w = a.width(), h = a.height(), s = opt.supersample;
    const double areaA = area(a), areaB = area(b);
    PairResult out;
    for (int k = 1; k <= opt.intermediates; ++k) {
        if (stop(opt)) return cancelledError();
        const double t = static_cast<double>(k) / (opt.intermediates + 1);
        Votes v(w, h, s);
        // Each side's pixels weigh by how near the in-between is to it.
        splat(a, fwd.value(), bwd.value(), t, static_cast<float>(1.0 - t), v);
        splat(b, bwd.value(), fwd.value(), 1.0 - t, static_cast<float>(t), v);
        Image img(w, h);
        // Full weight of a pixel: s*s samples, from the side it is nearer to.
        const float full = static_cast<float>(s * s) * static_cast<float>(std::max(t, 1.0 - t));
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const std::size_t i = at(w, x, y);
                if (v.covered[i] < static_cast<float>(opt.opaqueShare) * full) continue;
                const auto best = std::max_element(v.cell[i].begin(), v.cell[i].end(),
                                                   [](const auto& l, const auto& r) { return l.second < r.second; });
                img.setPixel(x, y, fromKey(best->first));
            }
        tidy(img);
        const double expected = areaA + (areaB - areaA) * t;
        out.areaDeviation.push_back(expected > 0 ? (area(img) - expected) / expected : 0.0);
        out.frames.push_back(std::move(img));
    }
    return out;
}

Result<SequenceResult> inbetweenSequence(const std::vector<Image>& keys, const std::vector<int>& durations, bool loop,
                                         Timing timing, int fixedMs, const Options& opt) {
    if (keys.size() < 2) return Error(ErrorCode::InvalidArgument, "In-betweening needs at least two key frames.");
    if (durations.size() != keys.size()) return Error(ErrorCode::InvalidArgument, "Give one duration per key frame.");
    if (timing == Timing::Fixed && fixedMs < 1) return Error(ErrorCode::InvalidArgument, "A fixed duration must be at least 1 ms.");
    for (const Image& k : keys)
        if (k.width() != keys[0].width() || k.height() != keys[0].height())
            return Error(ErrorCode::InvalidArgument, "Every key frame must be the same size.");
    SequenceResult res;
    const std::size_t pairs = loop ? keys.size() : keys.size() - 1;
    const int n = opt.intermediates;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const int dur = durations[i];
        const bool hasPair = i < pairs;
        // Split: the key and its in-betweens share the key's time (at least 1 ms each).
        const int parts = hasPair ? n + 1 : 1;
        const int share = timing == Timing::Fixed ? fixedMs : std::max(1, static_cast<int>(std::lround(static_cast<double>(dur) / parts)));
        res.frames.push_back(keys[i]);
        res.durations.push_back(share);
        res.key.push_back(static_cast<int>(i));
        if (!hasPair) continue;
        auto pr = inbetweenPair(keys[i], keys[(i + 1) % keys.size()], opt);
        if (!pr) return pr.error();
        for (std::size_t k = 0; k < pr.value().frames.size(); ++k) {
            const double dev = pr.value().areaDeviation[k];
            if (std::abs(dev) > opt.areaTolerance) {
                res.flagged.push_back(static_cast<int>(res.frames.size()));
                res.notes.push_back("Frame " + std::to_string(res.frames.size() + 1) + " (between keys " + std::to_string(i + 1) + " and " +
                                    std::to_string((i + 1) % keys.size() + 1) + ") is " + std::to_string(static_cast<int>(std::lround(dev * 100))) +
                                    "% off the expected size; the pose may change too much for an in-between. Check it, or draw it.");
            }
            res.frames.push_back(std::move(pr.value().frames[k]));
            res.durations.push_back(share);
            res.key.push_back(-1);
        }
    }
    return res;
}

Result<DocumentResult> inbetweenLayer(const Document& doc, LayerId layer, int first, int last, bool loop, Timing timing,
                                      int fixedMs, const Options& opt) {
    const Layer* l = doc.layer(layer);
    if (!l || l->kind != LayerKind::Raster) return Error(ErrorCode::InvalidArgument, "Choose a raster layer to in-between.");
    if (first < 0 || last >= doc.frameCount() || last - first < 1)
        return Error(ErrorCode::InvalidArgument, "Select two or more frames (the keys) to in-between.");
    const int w = doc.width(), h = doc.height();
    std::vector<Image> keys;
    std::vector<int> durations;
    for (int f = first; f <= last; ++f) {
        Image img(w, h);
        if (const Cel* c = doc.cel(layer, f); c && !c->image.isNull()) img.blit(c->image, c->x, c->y);
        keys.push_back(std::move(img));
        durations.push_back(doc.frames()[static_cast<std::size_t>(f)].durationMs);
    }
    auto seq = inbetweenSequence(keys, durations, loop, timing, fixedMs, opt);
    if (!seq) return seq.error();
    const SequenceResult& s = seq.value();
    if (doc.frameCount() + static_cast<int>(s.frames.size() - keys.size()) > Document::kMaxFrames)
        return Error(ErrorCode::InvalidArgument, "The in-betweens would pass the frame limit.");

    DocumentResult out{doc, {}, {}, s.notes};
    Document& d = out.document;
    std::vector<AnimationTag> tags = d.tags();
    // Work from the last key back, so earlier frame numbers stay valid.
    std::size_t idx = s.frames.size();
    for (int k = static_cast<int>(keys.size()) - 1; k >= 0; --k) {
        // The run of sequence frames belonging to key k: the key, then its in-betweens.
        std::size_t start = idx;
        while (start > 0 && s.key[start - 1] != k) --start;
        const std::size_t keyAt = start - 1;
        const int keyFrame = first + k;
        const int count = static_cast<int>(idx - start);
        for (int j = 0; j < count; ++j) {
            auto dup = d.duplicateFrame(keyFrame);   // holds every layer's cel and mask
            if (!dup) return dup.error();
        }
        for (int j = 0; j < count; ++j) {
            const int f = keyFrame + 1 + j;
            const Image& img = s.frames[start + static_cast<std::size_t>(j)];
            const IntRect b = img.opaqueBounds();
            d.unlinkCels(layer, {f});
            if (b.empty()) d.clearCel(layer, f);
            else if (auto st = d.setCel(layer, f, Cel{img.copy(b), b.x, b.y}); !st) return st.error();
            if (auto st = d.setFrameDuration(f, s.durations[start + static_cast<std::size_t>(j)]); !st) return st.error();
        }
        if (auto st = d.setFrameDuration(keyFrame, s.durations[keyAt]); !st) return st.error();
        // Tags: duplicateFrame shifted those after the key; one that ends on
        // the key keeps its in-betweens too.
        for (std::size_t t = 0; t < tags.size(); ++t) {
            AnimationTag& tag = tags[t];
            if (tag.fromFrame > keyFrame) tag.fromFrame += count;
            if (tag.toFrame >= keyFrame && tag.fromFrame <= keyFrame) tag.toFrame += count;
            else if (tag.toFrame > keyFrame) tag.toFrame += count;
        }
        idx = keyAt;
    }
    if (auto st = d.replaceTags(tags); !st) return st.error();
    // Where the in-betweens ended up.
    int f = first;
    for (std::size_t i = 0; i < s.frames.size(); ++i, ++f) {
        if (s.key[i] < 0) out.newFrames.push_back(f);
        if (std::find(s.flagged.begin(), s.flagged.end(), static_cast<int>(i)) != s.flagged.end()) out.flagged.push_back(f);
    }
    return out;
}

} // namespace acestudio::inbetween
