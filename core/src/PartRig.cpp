#include "PartRig.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <numeric>

namespace acestudio::motion::rig {

namespace {

constexpr int kUp = 8;   // the up-scaled copies are Scale2x three times

int iround(double v) { return static_cast<int>(std::lround(v)); }

int luma(Rgba8 c) { return (c.r * 299 + c.g * 587 + c.b * 114) / 1000; }

std::uint32_t key(Rgba8 c) {
    return (static_cast<std::uint32_t>(c.r) << 24) | (static_cast<std::uint32_t>(c.g) << 16) | (static_cast<std::uint32_t>(c.b) << 8) | c.a;
}

Rgba8 unkey(std::uint32_t k) {
    return Rgba8{static_cast<std::uint8_t>(k >> 24), static_cast<std::uint8_t>(k >> 16), static_cast<std::uint8_t>(k >> 8),
                 static_cast<std::uint8_t>(k)};
}

} // namespace

Image scale2x(const Image& in) {
    const int W = in.width(), H = in.height();
    Image out(W * 2, H * 2);
    auto px = [&](int x, int y) { return in.pixel(std::clamp(x, 0, W - 1), std::clamp(y, 0, H - 1)); };
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const Rgba8 B = px(x, y - 1), D = px(x - 1, y), E = px(x, y), F = px(x + 1, y), Hh = px(x, y + 1);
            Rgba8 e0 = E, e1 = E, e2 = E, e3 = E;
            if (!(B == Hh) && !(D == F)) {
                if (D == B) e0 = D;
                if (B == F) e1 = F;
                if (D == Hh) e2 = D;
                if (Hh == F) e3 = F;
            }
            out.setPixel(2 * x, 2 * y, e0);
            out.setPixel(2 * x + 1, 2 * y, e1);
            out.setPixel(2 * x, 2 * y + 1, e2);
            out.setPixel(2 * x + 1, 2 * y + 1, e3);
        }
    return out;
}

// ---- Masks ---------------------------------------------------------------------

Mask opaqueMask(const Image& sprite) {
    Mask m(sprite.width(), sprite.height());
    for (int y = 0; y < sprite.height(); ++y)
        for (int x = 0; x < sprite.width(); ++x)
            if (sprite.pixel(x, y).a > 0) m.set(x, y, true);
    return m;
}

int count(const Mask& m) {
    int n = 0;
    for (int y = 0; y < m.height(); ++y)
        for (int x = 0; x < m.width(); ++x) n += m.get(x, y) ? 1 : 0;
    return n;
}

IntRect bounds(const Mask& m) {
    int x0 = m.width(), y0 = m.height(), x1 = -1, y1 = -1;
    for (int y = 0; y < m.height(); ++y)
        for (int x = 0; x < m.width(); ++x)
            if (m.get(x, y)) {
                x0 = std::min(x0, x);
                y0 = std::min(y0, y);
                x1 = std::max(x1, x);
                y1 = std::max(y1, y);
            }
    if (x1 < 0) return {};
    return {x0, y0, x1 - x0 + 1, y1 - y0 + 1};
}

Mask dilate(const Mask& m, int r) {
    if (r <= 0) return m;
    Mask out(m.width(), m.height());
    for (int y = 0; y < m.height(); ++y)
        for (int x = 0; x < m.width(); ++x) {
            if (!m.get(x, y)) continue;
            for (int dy = -r; dy <= r; ++dy)
                for (int dx = -r; dx <= r; ++dx) out.set(x + dx, y + dy, true);
        }
    return out;
}

Mask erode(const Mask& m, int r) {
    if (r <= 0) return m;
    Mask out(m.width(), m.height());
    for (int y = 0; y < m.height(); ++y)
        for (int x = 0; x < m.width(); ++x) {
            bool all = m.get(x, y);
            for (int dy = -r; dy <= r && all; ++dy)
                for (int dx = -r; dx <= r && all; ++dx) all = m.get(x + dx, y + dy);
            out.set(x, y, all);
        }
    return out;
}

Mask openMask(const Mask& m, int r) { return andMask(dilate(erode(m, r), r), m); }

Mask andMask(const Mask& a, const Mask& b) {
    Mask out(a.width(), a.height());
    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width(); ++x) out.set(x, y, a.get(x, y) && b.get(x, y));
    return out;
}

Mask minusMask(const Mask& a, const Mask& b) {
    Mask out(a.width(), a.height());
    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width(); ++x) out.set(x, y, a.get(x, y) && !b.get(x, y));
    return out;
}

Mask orMask(const Mask& a, const Mask& b) {
    Mask out(a.width(), a.height());
    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width(); ++x) out.set(x, y, a.get(x, y) || b.get(x, y));
    return out;
}

std::vector<Mask> components(const Mask& m) {
    const int w = m.width(), h = m.height();
    std::vector<int> label(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), -1);
    std::vector<Mask> out;
    std::vector<int> sizes;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (!m.get(x, y) || label[static_cast<std::size_t>(y * w + x)] >= 0) continue;
            const int id = static_cast<int>(out.size());
            Mask piece(w, h);
            int n = 0;
            std::vector<std::pair<int, int>> stack{{x, y}};
            label[static_cast<std::size_t>(y * w + x)] = id;
            while (!stack.empty()) {
                const auto [px, py] = stack.back();
                stack.pop_back();
                piece.set(px, py, true);
                ++n;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = px + dx, ny = py + dy;
                        if ((dx || dy) && m.get(nx, ny) && label[static_cast<std::size_t>(ny * w + nx)] < 0) {
                            label[static_cast<std::size_t>(ny * w + nx)] = id;
                            stack.emplace_back(nx, ny);
                        }
                    }
            }
            out.push_back(std::move(piece));
            sizes.push_back(n);
        }
    std::vector<std::size_t> order(out.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return sizes[a] > sizes[b]; });
    std::vector<Mask> sorted;
    for (std::size_t i : order) sorted.push_back(std::move(out[i]));
    return sorted;
}

// ---- Rig -------------------------------------------------------------------------

std::vector<PartPose> Rig::rest() const {
    std::vector<PartPose> p(parts.size());
    for (std::size_t i = 0; i < parts.size(); ++i) p[i].z = parts[i].z;
    return p;
}

int Rig::find(const std::string& name) const {
    for (std::size_t i = 0; i < parts.size(); ++i)
        if (parts[i].name == name) return static_cast<int>(i);
    return -1;
}

int addPart(Rig& rig, const Image& sprite, const std::string& name, const Mask& own, Vec pivot, int z, Side side) {
    if (rig.width == 0) {
        rig.width = sprite.width();
        rig.height = sprite.height();
    }
    for (Part& p : rig.parts) {
        p.own = minusMask(p.own, own);
        for (int y = 0; y < rig.height; ++y)
            for (int x = 0; x < rig.width; ++x)
                if (own.get(x, y)) p.pixels.setPixel(x, y, Rgba8::transparent());
    }
    Part p;
    p.name = name;
    p.own = andMask(own, opaqueMask(sprite));
    p.pixels = Image(sprite.width(), sprite.height());
    for (int y = 0; y < sprite.height(); ++y)
        for (int x = 0; x < sprite.width(); ++x)
            if (p.own.get(x, y)) p.pixels.setPixel(x, y, sprite.pixel(x, y));
    p.pivot = pivot;
    p.z = z;
    p.side = side;
    rig.parts.push_back(std::move(p));
    return static_cast<int>(rig.parts.size()) - 1;
}

void inpaintHidden(Rig& rig, int maxGap) {
    const int w = rig.width, h = rig.height;
    for (std::size_t i = 0; i < rig.parts.size(); ++i) {
        Part& p = rig.parts[i];
        if (count(p.own) == 0) continue;
        // What covers this part: the pixels of parts drawn in front of it.
        Mask front(w, h);
        for (std::size_t j = 0; j < rig.parts.size(); ++j)
            if (j != i && rig.parts[j].z > p.z) front = orMask(front, rig.parts[j].own);
        // The part continues underneath where it lies on both sides of the
        // covered pixel, along the row or along the column.
        Mask hidden(w, h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                if (!front.get(x, y)) continue;
                auto reach = [&](int sx, int sy) {
                    for (int k = 1; k <= maxGap; ++k) {
                        const int nx = x + sx * k, ny = y + sy * k;
                        if (nx < 0 || ny < 0 || nx >= w || ny >= h) return false;
                        if (p.own.get(nx, ny)) return true;
                        if (!front.get(nx, ny)) return false;   // air or a part behind: the part ends
                    }
                    return false;
                };
                if ((reach(-1, 0) && reach(1, 0)) || (reach(0, -1) && reach(0, 1))) hidden.set(x, y, true);
            }
        if (count(hidden) == 0) continue;
        // The outline colour: the most common colour on the part's edge.
        std::map<std::uint32_t, int> edge;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                if (!p.own.get(x, y)) continue;
                const bool rim = !p.own.get(x - 1, y) || !p.own.get(x + 1, y) || !p.own.get(x, y - 1) || !p.own.get(x, y + 1);
                if (rim) ++edge[key(p.pixels.pixel(x, y))];
            }
        std::uint32_t outline = 0;
        int best = -1;
        for (const auto& [k, n] : edge)
            if (n > best && luma(unkey(k)) < 96) { best = n; outline = k; }
        // Fill from the part inwards, one ring at a time: each pixel takes
        // the most common inner (non-outline) colour around it.
        Mask done = p.own;
        Mask todo = hidden;
        for (int pass = 0; pass < maxGap * 2 && count(todo) > 0; ++pass) {
            std::vector<std::pair<std::pair<int, int>, Rgba8>> ring;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    if (!todo.get(x, y)) continue;
                    std::map<std::uint32_t, int> near;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx)
                            if ((dx || dy) && done.get(x + dx, y + dy)) ++near[key(p.pixels.pixel(x + dx, y + dy))];
                    if (near.empty()) continue;
                    std::uint32_t pick = 0;
                    int pn = -1;
                    for (const auto& [k, n] : near) {
                        const int score = n * 4 - (best >= 0 && k == outline ? 3 * n : 0);
                        if (score > pn) { pn = score; pick = k; }
                    }
                    ring.push_back({{x, y}, unkey(pick)});
                }
            if (ring.empty()) break;
            for (const auto& [xy, c] : ring) {
                p.pixels.setPixel(xy.first, xy.second, c);
                done.set(xy.first, xy.second, true);
                todo.set(xy.first, xy.second, false);
            }
        }
        // The edge of the filled area, where the part ends under the cover,
        // is drawn in the outline colour.
        if (best >= 0)
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    if (!hidden.get(x, y) || !done.get(x, y)) continue;
                    if (!done.get(x - 1, y) || !done.get(x + 1, y) || !done.get(x, y - 1) || !done.get(x, y + 1))
                        p.pixels.setPixel(x, y, unkey(outline));
                }
    }
}

void prepare(Rig& rig) {
    for (Part& p : rig.parts) {
        const IntRect b = p.pixels.opaqueBounds();
        if (b.empty()) {
            p.hiBox = {};
            p.hi = Image();
            continue;
        }
        p.hiBox = {b.x - 1, b.y - 1, b.w + 2, b.h + 2};
        Image crop(p.hiBox.w, p.hiBox.h);
        for (int y = 0; y < p.hiBox.h; ++y)
            for (int x = 0; x < p.hiBox.w; ++x) crop.setPixel(x, y, p.pixels.pixel(p.hiBox.x + x, p.hiBox.y + y));
        for (int k = 0; k < 3; ++k) crop = scale2x(crop);
        p.hi = std::move(crop);
    }
}

namespace {

bool rigid(const PartPose& q) { return q.angle == 0 && q.sx == 1 && q.sy == 1 && q.bend == 0; }

double smooth01(double k) {
    k = std::clamp(k, 0.0, 1.0);
    return k * k * (3 - 2 * k);
}

// Calls put(x, y, colour) for every pixel of part `p` under pose `q`
// (sprite coordinates).
template <class Put>
void drawPart(const Part& p, const PartPose& q, Put&& put) {
    if (!q.visible || p.hiBox.empty()) return;
    const int rdx = iround(q.dx), rdy = iround(q.dy);
    const IntRect b{p.hiBox.x + 1, p.hiBox.y + 1, p.hiBox.w - 2, p.hiBox.h - 2};
    auto colOff = [&](int sx) {
        if (q.colDy.empty()) return 0;
        const int k = std::clamp(sx - b.x, 0, static_cast<int>(q.colDy.size()) - 1);
        return q.colDy[static_cast<std::size_t>(k)];
    };
    if (rigid(q)) {
        for (int y = b.y; y < b.y + b.h; ++y)
            for (int x = b.x; x < b.x + b.w; ++x) {
                const Rgba8 c = p.pixels.pixel(x, y);
                if (c.a && y + rdy + colOff(x) <= q.maxY) put(x + rdx, y + rdy + colOff(x), c);
            }
        return;
    }
    const double c = std::cos(q.angle), s = std::sin(q.angle);
    const Vec piv{p.pivot.x + rdx, p.pivot.y + rdy};
    // Output area: the transformed corners of the part's box.
    int maxCol = 0, minCol = 0;
    for (int v : q.colDy) {
        maxCol = std::max(maxCol, v);
        minCol = std::min(minCol, v);
    }
    double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
    for (int k = 0; k < 4; ++k) {
        const double px = (k & 1 ? b.x + b.w : b.x) - p.pivot.x, py = (k & 2 ? b.y + b.h + maxCol : b.y + minCol) - p.pivot.y;
        const double ux = px * q.sx, uy = py * q.sy;
        const double ox = c * ux - s * uy + piv.x, oy = s * ux + c * uy + piv.y;
        x0 = std::min(x0, ox);
        y0 = std::min(y0, oy);
        x1 = std::max(x1, ox);
        y1 = std::max(y1, oy);
    }
    int ix0 = static_cast<int>(std::floor(x0)) - 1, iy0 = static_cast<int>(std::floor(y0)) - 1;
    int ix1 = static_cast<int>(std::ceil(x1)) + 1, iy1 = static_cast<int>(std::ceil(y1)) + 1;
    const double isx = 1.0 / std::max(1e-3, std::abs(q.sx)) * (q.sx < 0 ? -1 : 1);
    const double isy = 1.0 / std::max(1e-3, std::abs(q.sy)) * (q.sy < 0 ? -1 : 1);
    // A curled part reaches further round: widen the area to its whole circle.
    if (q.bend != 0 || q.skinDepth > 0) {
        double r = 0;
        for (int k = 0; k < 4; ++k)
            r = std::max(r, std::hypot((k & 1 ? b.x + b.w : b.x) - p.pivot.x, (k & 2 ? b.y + b.h : b.y) - p.pivot.y) *
                                std::max(std::abs(q.sx), std::abs(q.sy)));
        ix0 = std::min(ix0, static_cast<int>(std::floor(piv.x - r)) - 1);
        iy0 = std::min(iy0, static_cast<int>(std::floor(piv.y - r)) - 1);
        ix1 = std::max(ix1, static_cast<int>(std::ceil(piv.x + r)) + 1);
        iy1 = std::max(iy1, static_cast<int>(std::ceil(piv.y + r)) + 1);
    }
    for (int oy = iy0; oy <= iy1; ++oy)
        for (int ox = ix0; ox <= ix1; ++ox) {
            const double qx = ox + 0.5 - piv.x, qy = oy + 0.5 - piv.y;
            double cc = c, ss = s;
            double a = q.angle;
            if (q.bend != 0) a += q.bend * smooth01(std::hypot(qx, qy) / std::max(1.0, q.bendRadius));
            double u = 0, v = 0;
            auto sourceAt = [&](double m) {
                cc = std::cos(a * m);
                ss = std::sin(a * m);
                u = (cc * qx + ss * qy) * isx;
                v = (-ss * qx + cc * qy) * isy;
            };
            if (q.skinDepth > 0) {
                // A skinned root turns less the deeper its source point lies
                // below skinTop: m = weight(source(m)). Where the root is
                // stretched over itself several m solve it; the most turned
                // one that lands on the part's own pixels wins (it is on top).
                auto g = [&](double m) {
                    sourceAt(m);
                    const double sy = p.pivot.y + v, sx = p.pivot.x + u;
                    double wgt = 1.0 - smooth01((sy - q.skinTop) / q.skinDepth);
                    if (q.skinFade > 0 && sy > q.skinTop) {
                        const double out = std::max({0.0, q.skinX0 - sx, sx - q.skinX1});
                        wgt *= 1.0 - smooth01(out / q.skinFade);
                    }
                    return wgt - m;
                };
                auto opaqueAt = [&]() {
                    const int hx = static_cast<int>(std::floor((p.pivot.x + u - p.hiBox.x) * kUp));
                    const int hy = static_cast<int>(std::floor((p.pivot.y + v - p.hiBox.y) * kUp));
                    return p.hi.contains(hx, hy) && p.hi.pixel(hx, hy).a != 0;
                };
                constexpr int kSamples = 12;
                double found = -1;
                double prev = g(1.0);
                if (std::abs(prev) < 1e-9 && opaqueAt()) found = 1.0;
                for (int k = kSamples - 1; k >= 0 && found < 0; --k) {
                    const double m0 = static_cast<double>(k) / kSamples, m1 = static_cast<double>(k + 1) / kSamples;
                    const double g0 = g(m0);
                    if ((g0 > 0) != (prev > 0) || g0 == 0) {
                        double lo = m0, hi = m1;   // g(lo) and g(hi) differ in sign
                        const bool loPos = g0 > 0;
                        for (int it = 0; it < 12; ++it) {
                            const double mid = 0.5 * (lo + hi);
                            ((g(mid) > 0) == loPos ? lo : hi) = mid;
                        }
                        sourceAt(0.5 * (lo + hi));
                        if (opaqueAt()) found = 0.5 * (lo + hi);
                    }
                    prev = g0;
                }
                if (found < 0) continue;
                sourceAt(found);
            } else {
                sourceAt(1.0);
            }
            const double srcX = p.pivot.x + u;
            double srcY = p.pivot.y + v;
            srcY -= colOff(static_cast<int>(std::floor(srcX)));
            const int hx = static_cast<int>(std::floor((srcX - p.hiBox.x) * kUp));
            const int hy = static_cast<int>(std::floor((srcY - p.hiBox.y) * kUp));
            if (!p.hi.contains(hx, hy)) continue;
            const Rgba8 col = p.hi.pixel(hx, hy);
            if (col.a && oy <= q.maxY) put(ox, oy, col);
        }
}

} // namespace

Image renderPose(const Rig& rig, const std::vector<PartPose>& pose, IntRect box) {
    Image out(box.w, box.h);
    std::vector<std::size_t> order(rig.parts.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        const int za = a < pose.size() ? pose[a].z : rig.parts[a].z, zb = b < pose.size() ? pose[b].z : rig.parts[b].z;
        return za < zb;
    });
    const PartPose none;
    for (std::size_t i : order) {
        const PartPose& q = i < pose.size() ? pose[i] : none;
        if (rigid(q)) {
            drawPart(rig.parts[i], q, [&](int x, int y, Rgba8 c) {
                const int tx = x - box.x, ty = y - box.y;
                if (tx >= 0 && ty >= 0 && tx < box.w && ty < box.h) out.setPixel(tx, ty, Rgba8{c.r, c.g, c.b, 255});
            });
            continue;
        }
        // A turned or curled part is drawn on its own layer first: crumbs a
        // stretched area leaves behind (a few pixels touching nothing else of
        // the part) are dropped, so no specks float beside it.
        Image layer(box.w, box.h);
        Mask on(box.w, box.h);
        drawPart(rig.parts[i], q, [&](int x, int y, Rgba8 c) {
            const int tx = x - box.x, ty = y - box.y;
            if (tx >= 0 && ty >= 0 && tx < box.w && ty < box.h) {
                layer.setPixel(tx, ty, Rgba8{c.r, c.g, c.b, 255});
                on.set(tx, ty, true);
            }
        });
        const int minPiece = std::max(4, count(rig.parts[i].own) / 150);
        for (const Mask& piece : components(on)) {
            if (count(piece) < minPiece) continue;
            const IntRect r = bounds(piece);
            for (int y = r.y; y < r.y + r.h; ++y)
                for (int x = r.x; x < r.x + r.w; ++x)
                    if (piece.get(x, y)) out.setPixel(x, y, layer.pixel(x, y));
        }
    }
    return out;
}

Vec transformPoint(const Rig& rig, int i, const PartPose& q, Vec pt) {
    const Part& p = rig.parts[static_cast<std::size_t>(i)];
    const double c = std::cos(q.angle), s = std::sin(q.angle);
    const double ux = (pt.x - p.pivot.x) * q.sx, uy = (pt.y - p.pivot.y) * q.sy;
    return {c * ux - s * uy + p.pivot.x + iround(q.dx), s * ux + c * uy + p.pivot.y + iround(q.dy)};
}

Vec solveTwoBone(Vec root, Vec target, double l1, double l2, int bend) {
    double dx = target.x - root.x, dy = target.y - root.y;
    double d = std::hypot(dx, dy);
    const double maxD = l1 + l2 - 1e-6, minD = std::abs(l1 - l2) + 1e-6;
    if (d < 1e-9) {
        dx = 0;
        dy = 1;
        d = 1e-9;
    }
    const double dc = std::clamp(d, minD, maxD);
    // Distance from root to the foot of the joint's perpendicular.
    const double a = (l1 * l1 - l2 * l2 + dc * dc) / (2 * dc);
    const double hh = std::sqrt(std::max(0.0, l1 * l1 - a * a));
    const double ux = dx / d, uy = dy / d;
    // Right of the direction (screen, y down) is (-uy, ux).
    const double side = bend >= 0 ? 1.0 : -1.0;
    return {root.x + ux * a + side * -uy * hh, root.y + uy * a + side * ux * hh};
}

std::vector<ZSwap> zPops(const Rig& rig, const std::vector<std::vector<PartPose>>& poses, IntRect box) {
    std::vector<ZSwap> out;
    const std::size_t n = rig.parts.size();
    auto masks = [&](const std::vector<PartPose>& pose) {
        std::vector<Mask> m;
        for (std::size_t i = 0; i < n; ++i) {
            Mask mk(box.w, box.h);
            drawPart(rig.parts[i], i < pose.size() ? pose[i] : PartPose{}, [&](int x, int y, Rgba8) { mk.set(x - box.x, y - box.y, true); });
            m.push_back(std::move(mk));
        }
        return m;
    };
    auto overlaps = [](const Mask& a, const Mask& b) {
        for (int y = 0; y < a.height(); ++y)
            for (int x = 0; x < a.width(); ++x)
                if (a.get(x, y) && b.get(x, y)) return true;
        return false;
    };
    for (std::size_t f = 0; f < poses.size(); ++f) {
        const auto& A = poses[f];
        const auto& B = poses[(f + 1) % poses.size()];
        std::vector<Mask> ma, mb;
        for (std::size_t i = 0; i < n; ++i)
            for (std::size_t j = i + 1; j < n; ++j) {
                const int before = (A[i].z > A[j].z) - (A[i].z < A[j].z), after = (B[i].z > B[j].z) - (B[i].z < B[j].z);
                if (before == after || before == 0 || after == 0) continue;
                if (ma.empty()) ma = masks(A), mb = masks(B);
                if (overlaps(ma[i], ma[j]) && overlaps(mb[i], mb[j]))
                    out.push_back({static_cast<int>(f), static_cast<int>(i), static_cast<int>(j)});
            }
    }
    return out;
}

} // namespace acestudio::motion::rig
