#include "acestudio/core/Autotile.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <set>

namespace acestudio {

namespace {
constexpr std::uint8_t kN = 1, kNE = 2, kE = 4, kSE = 8, kS = 16, kSW = 32, kW = 64, kNW = 128;

std::uint8_t reduceBlob(std::uint8_t m) {
    auto keep = [&](std::uint8_t corner, std::uint8_t a, std::uint8_t b) {
        if ((m & corner) && !((m & a) && (m & b))) m = static_cast<std::uint8_t>(m & ~corner);
    };
    keep(kNE, kN, kE);
    keep(kSE, kS, kE);
    keep(kSW, kS, kW);
    keep(kNW, kN, kW);
    return m;
}

int channelDiff(Rgba8 a, Rgba8 b) {
    return std::max({std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b), std::abs(a.a - b.a)});
}
} // namespace

std::string_view toString(AutotileScheme s) noexcept {
    switch (s) {
    case AutotileScheme::Cardinal16: return "cardinal16";
    case AutotileScheme::Blob47: return "blob47";
    case AutotileScheme::Wang16: return "wang16";
    }
    return "blob47";
}

std::optional<AutotileScheme> autotileSchemeFromString(std::string_view s) noexcept {
    for (auto v : {AutotileScheme::Cardinal16, AutotileScheme::Blob47, AutotileScheme::Wang16})
        if (toString(v) == s) return v;
    return std::nullopt;
}

int tileCount(AutotileScheme s) noexcept { return s == AutotileScheme::Blob47 ? 47 : 16; }

std::pair<int, int> tilesetGrid(AutotileScheme s) noexcept {
    return s == AutotileScheme::Blob47 ? std::pair<int, int>{8, 6} : std::pair<int, int>{4, 4};
}

const std::vector<std::uint8_t>& blobMasks() {
    static const std::vector<std::uint8_t> masks = [] {
        std::set<std::uint8_t> u;
        for (int m = 0; m < 256; ++m) u.insert(reduceBlob(static_cast<std::uint8_t>(m)));
        return std::vector<std::uint8_t>(u.begin(), u.end());
    }();
    return masks;
}

TileSamples tileSamples(AutotileScheme s, int i) {
    TileSamples t{};
    auto on = [](bool b) { return static_cast<std::uint8_t>(b ? 255 : 0); };
    switch (s) {
    case AutotileScheme::Cardinal16: {
        const bool n = i & 1, e = i & 2, so = i & 4, w = i & 8;
        t = {on(n && w), on(n), on(n && e), on(w), 255, on(e), on(so && w), on(so), on(so && e)};
        break;
    }
    case AutotileScheme::Blob47: {
        const std::uint8_t m = blobMasks().at(static_cast<std::size_t>(i));
        t = {on(m & kNW), on(m & kN), on(m & kNE), on(m & kW), 255, on(m & kE), on(m & kSW), on(m & kS), on(m & kSE)};
        break;
    }
    case AutotileScheme::Wang16: {
        const int tl = (i & 1) ? 255 : 0, tr = (i & 2) ? 255 : 0, br = (i & 4) ? 255 : 0, bl = (i & 8) ? 255 : 0;
        auto avg = [](int a, int b) { return static_cast<std::uint8_t>((a + b + 1) / 2); };
        t = {static_cast<std::uint8_t>(tl), avg(tl, tr), static_cast<std::uint8_t>(tr),
             avg(tl, bl), static_cast<std::uint8_t>((tl + tr + br + bl + 2) / 4), avg(tr, br),
             static_cast<std::uint8_t>(bl), avg(bl, br), static_cast<std::uint8_t>(br)};
        break;
    }
    }
    return t;
}

TerrainMap::TerrainMap(int w, int h) : m_w(std::max(0, w)), m_h(std::max(0, h)), m_cells(static_cast<std::size_t>(m_w) * static_cast<std::size_t>(m_h), 0) {}

bool TerrainMap::at(int x, int y) const noexcept {
    if (x < 0 || y < 0 || x >= m_w || y >= m_h) return false;
    return m_cells[static_cast<std::size_t>(y) * static_cast<std::size_t>(m_w) + static_cast<std::size_t>(x)] != 0;
}

void TerrainMap::set(int x, int y, bool v) noexcept {
    if (x < 0 || y < 0 || x >= m_w || y >= m_h) return;
    m_cells[static_cast<std::size_t>(y) * static_cast<std::size_t>(m_w) + static_cast<std::size_t>(x)] = v ? 1 : 0;
}

void TerrainMap::fill(bool v) noexcept { std::fill(m_cells.begin(), m_cells.end(), static_cast<std::uint8_t>(v ? 1 : 0)); }

TilePlacement autotile(const TerrainMap& map, AutotileScheme s) {
    TilePlacement p;
    if (s == AutotileScheme::Wang16) {
        p.columns = map.width() + 1;
        p.rows = map.height() + 1;
        p.tiles.resize(static_cast<std::size_t>(p.columns) * static_cast<std::size_t>(p.rows));
        for (int y = 0; y < p.rows; ++y)
            for (int x = 0; x < p.columns; ++x)
                p.tiles[static_cast<std::size_t>(y) * static_cast<std::size_t>(p.columns) + static_cast<std::size_t>(x)] =
                    (map.at(x - 1, y - 1) ? 1 : 0) | (map.at(x, y - 1) ? 2 : 0) | (map.at(x, y) ? 4 : 0) | (map.at(x - 1, y) ? 8 : 0);
        return p;
    }
    p.columns = map.width();
    p.rows = map.height();
    p.tiles.assign(static_cast<std::size_t>(p.columns) * static_cast<std::size_t>(p.rows), -1);
    const auto& masks = blobMasks();
    for (int y = 0; y < p.rows; ++y)
        for (int x = 0; x < p.columns; ++x) {
            if (!map.at(x, y)) continue;
            const bool n = map.at(x, y - 1), e = map.at(x + 1, y), so = map.at(x, y + 1), w = map.at(x - 1, y);
            int idx;
            if (s == AutotileScheme::Cardinal16) {
                idx = (n ? 1 : 0) | (e ? 2 : 0) | (so ? 4 : 0) | (w ? 8 : 0);
            } else {
                std::uint8_t m = static_cast<std::uint8_t>((n ? kN : 0) | (e ? kE : 0) | (so ? kS : 0) | (w ? kW : 0) |
                                                           (map.at(x + 1, y - 1) ? kNE : 0) | (map.at(x + 1, y + 1) ? kSE : 0) |
                                                           (map.at(x - 1, y + 1) ? kSW : 0) | (map.at(x - 1, y - 1) ? kNW : 0));
                m = reduceBlob(m);
                idx = static_cast<int>(std::lower_bound(masks.begin(), masks.end(), m) - masks.begin());
            }
            p.tiles[static_cast<std::size_t>(y) * static_cast<std::size_t>(p.columns) + static_cast<std::size_t>(x)] = idx;
        }
    return p;
}

Image Tileset::tile(int index, int px, int py) const {
    const int P = std::max(1, phases);
    px = ((px % P) + P) % P;
    py = ((py % P) + P) % P;
    if (index == kBaseTile) return base.isNull() ? Image() : base.copy({px * tileSize, py * tileSize, tileSize, tileSize});
    const auto [cols, rows] = tilesetGrid(scheme);
    return image.copy({(px * cols + index % cols) * tileSize, (py * rows + index / cols) * tileSize, tileSize, tileSize});
}

Status validateTileset(const Tileset& t) {
    if (t.tileSize < 2 || t.tileSize > 512) return Error{ErrorCode::InvalidArgument, "Tile size must be between 2 and 512 pixels"};
    if (t.phases < 1 || t.phases > 4) return Error{ErrorCode::InvalidArgument, "A tileset has 1 to 4 phases"};
    const auto [cols, rows] = tilesetGrid(t.scheme);
    const int W = t.phases * cols * t.tileSize, H = t.phases * rows * t.tileSize;
    if (t.image.width() != W || t.image.height() != H)
        return Error{ErrorCode::InvalidArgument, "A " + std::string(toString(t.scheme)) + " tileset of " + std::to_string(t.tileSize) +
                                                   " px tiles" + (t.phases > 1 ? " with " + std::to_string(t.phases) + "x" + std::to_string(t.phases) + " phases" : std::string()) +
                                                   " must be " + std::to_string(W) + "x" + std::to_string(H) + " px (" +
                                                   std::to_string(cols * t.phases) + "x" + std::to_string(rows * t.phases) + " tiles)"};
    if (!t.base.isNull() && (t.base.width() != t.phases * t.tileSize || t.base.height() != t.phases * t.tileSize))
        return Error{ErrorCode::InvalidArgument, t.phases > 1 ? "The base must be " + std::to_string(t.phases) + "x" + std::to_string(t.phases) + " tiles in size"
                                                              : std::string("The base tile must be one tile in size")};
    return {};
}

Result<Image> renderMap(const TerrainMap& map, const Tileset& t) {
    if (auto st = validateTileset(t); !st) return st.error();
    const int T = t.tileSize, P = t.phases;
    auto out = Image::create(std::max(1, map.width() * T), std::max(1, map.height() * T));
    if (!out) return out.error();
    Image img = std::move(out).value();
    const TilePlacement p = autotile(map, t.scheme);
    // The dual grid (Wang16) is offset by half a tile so tiles straddle cells.
    const int off = t.scheme == AutotileScheme::Wang16 ? -T / 2 : 0;
    const auto [cols, rows] = tilesetGrid(t.scheme);
    for (int y = 0; y < p.rows; ++y)
        for (int x = 0; x < p.columns; ++x) {
            const int idx = p.at(x, y);
            const int px = x % P, py = y % P;
            const Image* src = &t.image;
            int sx = 0, sy = 0;
            if (idx < 0) {
                if (t.base.isNull()) continue;
                src = &t.base;
                sx = px * T;
                sy = py * T;
            } else {
                sx = (px * cols + idx % cols) * T;
                sy = (py * rows + idx / cols) * T;
            }
            for (int ty = 0; ty < T; ++ty)
                for (int tx = 0; tx < T; ++tx) {
                    const int dx = x * T + off + tx, dy = y * T + off + ty;
                    if (img.contains(dx, dy)) img.setPixel(dx, dy, src->pixel(sx + tx, sy + ty));
                }
        }
    return img;
}

LevelMap::LevelMap(int w, int h) : m_w(std::max(0, w)), m_h(std::max(0, h)), m_cells(static_cast<std::size_t>(m_w) * static_cast<std::size_t>(m_h), 0) {}

int LevelMap::at(int x, int y) const noexcept {
    if (x < 0 || y < 0 || x >= m_w || y >= m_h) return 0;
    return m_cells[static_cast<std::size_t>(y) * static_cast<std::size_t>(m_w) + static_cast<std::size_t>(x)];
}

void LevelMap::set(int x, int y, int level) noexcept {
    if (x < 0 || y < 0 || x >= m_w || y >= m_h) return;
    m_cells[static_cast<std::size_t>(y) * static_cast<std::size_t>(m_w) + static_cast<std::size_t>(x)] = static_cast<std::uint8_t>(std::clamp(level, 0, 255));
}

void LevelMap::fill(int level) noexcept { std::fill(m_cells.begin(), m_cells.end(), static_cast<std::uint8_t>(std::clamp(level, 0, 255))); }

void LevelMap::paint(int cx, int cy, int level, int radius, bool margins) noexcept {
    level = std::clamp(level, 0, 255);
    radius = std::max(0, radius);
    for (int y = 0; y < m_h; ++y)
        for (int x = 0; x < m_w; ++x) {
            // Chebyshev distance to the brush square.
            const int d = std::max({0, std::abs(x - cx) - radius, std::abs(y - cy) - radius});
            if (d == 0) { set(x, y, level); continue; }
            if (!margins) continue;
            set(x, y, std::clamp(at(x, y), level - d, level + d));
        }
}

void LevelMap::limitSteps() noexcept {
    for (bool changed = true; changed;) {
        changed = false;
        for (int y = 0; y < m_h; ++y)
            for (int x = 0; x < m_w; ++x) {
                int low = at(x, y);
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = x + dx, ny = y + dy;
                        if ((dx || dy) && nx >= 0 && ny >= 0 && nx < m_w && ny < m_h) low = std::min(low, at(nx, ny));
                    }
                if (at(x, y) > low + 1) { set(x, y, low + 1); changed = true; }
            }
    }
}

TerrainMap LevelMap::atLeast(int level) const {
    TerrainMap m(m_w, m_h);
    for (int y = 0; y < m_h; ++y)
        for (int x = 0; x < m_w; ++x) m.set(x, y, at(x, y) >= level);
    return m;
}

int LevelMap::maxLevel() const noexcept {
    int m = 0;
    for (std::uint8_t v : m_cells) m = std::max(m, static_cast<int>(v));
    return m;
}

namespace {
Rgba8 over(Rgba8 dst, Rgba8 src) {
    if (src.a == 255 || dst.a == 0) return src;
    if (src.a == 0) return dst;
    const int a = src.a, b = dst.a * (255 - a) / 255, oa = a + b;
    auto ch = [&](int s, int d) { return static_cast<std::uint8_t>((s * a + d * b + oa / 2) / oa); };
    return {ch(src.r, dst.r), ch(src.g, dst.g), ch(src.b, dst.b), static_cast<std::uint8_t>(oa)};
}
} // namespace

Result<Image> renderLevels(const LevelMap& map, const std::vector<Tileset>& levels) {
    if (levels.empty()) return Error{ErrorCode::InvalidArgument, "At least one terrain level is needed"};
    for (const Tileset& t : levels) {
        if (auto st = validateTileset(t); !st) return st.error();
        if (t.scheme != levels[0].scheme || t.tileSize != levels[0].tileSize || t.phases != levels[0].phases)
            return Error{ErrorCode::InvalidArgument, "All terrain levels must use the same layout, tile size and phases"};
    }
    auto first = renderMap(map.atLeast(1), levels[0]);
    if (!first) return first.error();
    Image img = std::move(first).value();
    for (std::size_t k = 1; k < levels.size(); ++k) {
        auto layer = renderMap(map.atLeast(static_cast<int>(k) + 1), levels[k]);
        if (!layer) return layer.error();
        auto dst = img.pixels();
        const auto src = layer.value().pixels();
        for (std::size_t i = 0; i < dst.size(); ++i) dst[i] = over(dst[i], src[i]);
    }
    return img;
}

std::string terrainSetJson(const std::vector<Tileset>& levels, const std::vector<std::string>& imageFiles,
                           const std::vector<std::string>& jsonFiles, const std::string& baseFile) {
    using nlohmann::ordered_json;
    ordered_json root = ordered_json::object();
    if (!levels.empty()) {
        root["scheme"] = std::string(toString(levels[0].scheme));
        root["tileSize"] = levels[0].tileSize;
        root["phases"] = levels[0].phases;
    }
    if (!baseFile.empty()) root["base"] = baseFile;
    ordered_json ls = ordered_json::array();
    for (std::size_t i = 0; i < levels.size(); ++i) {
        ordered_json o = ordered_json::object();
        o["level"] = static_cast<int>(i) + 1;
        o["image"] = i < imageFiles.size() ? imageFiles[i] : std::string();
        o["json"] = i < jsonFiles.size() ? jsonFiles[i] : std::string();
        ls.push_back(std::move(o));
    }
    root["levels"] = std::move(ls);
    root["drawOrder"] = "level 1 over the base, then each higher level over the result; a cell of level k is terrain for levels 1..k";
    return root.dump(2) + "\n";
}

namespace {
// Mean difference between column a of `ia` and column b of `ib` (or rows).
double edgeDiff(const Image& ia, int a, const Image& ib, int b, bool columns) {
    const int n = columns ? ia.height() : ia.width();
    long sum = 0;
    for (int k = 0; k < n; ++k)
        sum += columns ? channelDiff(ia.pixel(a, k), ib.pixel(b, k)) : channelDiff(ia.pixel(k, a), ib.pixel(k, b));
    return n ? static_cast<double>(sum) / (255.0 * n) : 0.0;
}
// Mean difference between neighbouring columns (or rows) inside an image.
double interiorDiff(const Image& img, bool columns) {
    const int n = columns ? img.width() : img.height();
    if (n < 2) return 0;
    double s = 0;
    for (int k = 0; k + 1 < n; ++k) s += edgeDiff(img, k, img, k + 1, columns);
    return s / (n - 1);
}
// A layer with a transparent base is judged as it will look, drawn over
// something: here a neutral grey, so a pixel's coverage counts as much as a
// colour change of the same size would, not as a full 0-to-255 alpha jump.
Image flattenOverGrey(Image img) {
    for (auto& p : img.pixels()) {
        if (p.a == 255) continue;
        auto ch = [&](int c) { return static_cast<std::uint8_t>((c * p.a + 128 * (255 - p.a) + 127) / 255); };
        p = {ch(p.r), ch(p.g), ch(p.b), 255};
    }
    return img;
}
bool sideMatches(const TileSamples& a, const TileSamples& b, bool horizontal) {
    // a left of b: a's right column (TR, R, BR) vs b's left (TL, L, BL);
    // a above b: a's bottom row (BL, B, BR) vs b's top (TL, T, TR).
    if (horizontal) return a[2] == b[0] && a[5] == b[3] && a[8] == b[6];
    return a[6] == b[0] && a[7] == b[1] && a[8] == b[2];
}
} // namespace

namespace {
// Compares the edge between a (left / above) and b (right / below) pixel by
// pixel. With a1, a0 the last two columns of a and b0, b1 the first two of b:
//   step  the jump a0 -> b0 is much bigger than a1 -> a0 and b0 -> b1, i.e.
//         bigger than the image changes on either side (a texture that does
//         not continue, a tile drawn offset)
//   line  a0 (or b0) differs from both its neighbours while those agree with
//         each other (a grid line or border drawn along the tile edge)
// The edge is a seam when such pixels make up most of it. Outline corners,
// ragged edges and noisy textures change only scattered pixels.
struct EdgeVerdict {
    bool seam = false;
    double jump = 0, local = 0;
};
EdgeVerdict judgeEdge(const Image& A, const Image& B, bool horizontal, const SeamOptions& o) {
    const int T = A.width();
    const int n = horizontal ? A.height() : A.width();
    auto px = [&](const Image& img, int line, int k) { return horizontal ? img.pixel(line, k) : img.pixel(k, line); };
    const double floor = o.floor * 255.0;
    int bad = 0;
    double jumpSum = 0, localSum = 0;
    for (int k = 0; k < n; ++k) {
        const Rgba8 a2 = px(A, T - 3, k), a1 = px(A, T - 2, k), a0 = px(A, T - 1, k), b0 = px(B, 0, k), b1 = px(B, 1, k), b2 = px(B, 2, k);
        const double d = channelDiff(a0, b0), la = channelDiff(a1, a0), lb = channelDiff(b0, b1);
        // How much the image varies a little away from the edge, on both sides.
        const double ref = std::max({static_cast<double>(channelDiff(a2, a1)), static_cast<double>(channelDiff(b1, b2)), floor / o.factor});
        jumpSum += d;
        localSum += std::max(la, lb);
        const bool step = d > floor && d > o.factor * std::max({la, lb, ref});
        auto line = [&](Rgba8 mid, Rgba8 left, Rgba8 right) {
            const double x = channelDiff(mid, left), y = channelDiff(mid, right), z = channelDiff(left, right);
            return x > floor && y > floor && std::min(x, y) > o.factor * ref && z * 2 < std::min(x, y);
        };
        if (step || line(a0, a1, b0) || line(b0, a0, b1)) ++bad;
    }
    EdgeVerdict v;
    v.jump = jumpSum / (255.0 * n);
    v.local = localSum / (255.0 * n);
    v.seam = bad >= 3 && bad * 2 >= n;
    return v;
}
} // namespace

Result<SeamReport> checkSeams(const Tileset& t, const SeamOptions& o) {
    if (auto st = validateTileset(t); !st) return st.error();
    if (t.tileSize < 4) return Error{ErrorCode::InvalidArgument, "Tiles must be at least 4 px for the seam check"};
    const int n = tileCount(t.scheme), P = t.phases;
    std::vector<int> ids;
    for (int i = 0; i < n; ++i) ids.push_back(i);
    if (t.scheme != AutotileScheme::Wang16 && !t.base.isNull()) ids.push_back(kBaseTile);
    const std::size_t N = ids.size();
    // Images per (phase, tile): index (py * P + px) * N + k.
    std::vector<Image> img(N * static_cast<std::size_t>(P * P));
    std::vector<TileSamples> smp(N);
    for (std::size_t k = 0; k < N; ++k) smp[k] = ids[k] == kBaseTile ? TileSamples{} : tileSamples(t.scheme, ids[k]);
    auto slot = [&](int px, int py, std::size_t k) { return (static_cast<std::size_t>(py * P + px)) * N + k; };
    for (int py = 0; py < P; ++py)
        for (int px = 0; px < P; ++px)
            for (std::size_t k = 0; k < N; ++k) img[slot(px, py, k)] = flattenOverGrey(t.tile(ids[k], px, py));
    SeamReport r;
    for (int py = 0; py < P; ++py)
        for (int px = 0; px < P; ++px)
            for (std::size_t a = 0; a < N; ++a)
                for (std::size_t b = 0; b < N; ++b)
                    for (bool horizontal : {true, false}) {
                        if (!sideMatches(smp[a], smp[b], horizontal)) continue;
                        // The neighbour to the right / below is in the next phase.
                        const std::size_t sa = slot(px, py, a);
                        const std::size_t sb = horizontal ? slot((px + 1) % P, py, b) : slot(px, (py + 1) % P, b);
                        ++r.pairsChecked;
                        const EdgeVerdict v = judgeEdge(img[sa], img[sb], horizontal, o);
                        if (v.seam) r.issues.push_back({ids[a], ids[b], horizontal, v.jump, v.local, px, py});
                    }
    std::stable_sort(r.issues.begin(), r.issues.end(), [](const SeamIssue& x, const SeamIssue& y) { return x.jump > y.jump; });
    return r;
}

WrapCheck checkWrap(const Image& img, const SeamOptions& o) {
    WrapCheck w;
    if (img.width() < 2 || img.height() < 2) return w;
    w.jumpX = edgeDiff(img, img.width() - 1, img, 0, true);
    w.typicalX = interiorDiff(img, true);
    w.jumpY = edgeDiff(img, img.height() - 1, img, 0, false);
    w.typicalY = interiorDiff(img, false);
    w.horizontal = !(w.jumpX > o.factor * w.typicalX && w.jumpX > o.floor);
    w.vertical = !(w.jumpY > o.factor * w.typicalY && w.jumpY > o.floor);
    return w;
}

double tileCoverage(const TileSamples& smp, double u, double v, double roundness) {
    u = std::clamp(u, 0.0, 1.0);
    v = std::clamp(v, 0.0, 1.0);
    const int qi = u < 0.5 ? 0 : 1, qj = v < 0.5 ? 0 : 1;
    double tx = (u - 0.5 * qi) * 2.0, ty = (v - 0.5 * qj) * 2.0;
    auto shape = [&](double t) { return (1.0 - roundness) * t + roundness * t * t * (3.0 - 2.0 * t); };
    tx = shape(tx);
    ty = shape(ty);
    auto at = [&](int i, int j) { return smp[static_cast<std::size_t>(j * 3 + i)] / 255.0; };
    const double a = at(qi, qj), b = at(qi + 1, qj), c = at(qi, qj + 1), d = at(qi + 1, qj + 1);
    return (a * (1 - tx) + b * tx) * (1 - ty) + (c * (1 - tx) + d * tx) * ty;
}

namespace {
std::uint32_t hash32(std::uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}
// Smooth value noise, periodic with period 1 in u and v (L lattice cells).
double periodicNoise(double u, double v, int L, std::uint32_t seed) {
    const double x = u * L, y = v * L;
    const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
    const double fx = x - x0, fy = y - y0;
    auto val = [&](int i, int j) {
        const std::uint32_t ii = static_cast<std::uint32_t>(((i % L) + L) % L), jj = static_cast<std::uint32_t>(((j % L) + L) % L);
        return (hash32(ii * 73856093U ^ jj * 19349663U ^ seed * 83492791U) & 0xffffU) / 65535.0;
    };
    auto s = [](double t) { return t * t * (3 - 2 * t); };
    const double sx = s(fx), sy = s(fy);
    const double a = val(x0, y0), b = val(x0 + 1, y0), c = val(x0, y0 + 1), d = val(x0 + 1, y0 + 1);
    return (a * (1 - sx) + b * sx) * (1 - sy) + (c * (1 - sx) + d * sx) * sy;
}
} // namespace

Result<Tileset> generateTileset(AutotileScheme s, const Image& baseTex, const Image& terrainTex, const TilesetStyle& st, int tileSize) {
    const int W = terrainTex.width();
    const int T = tileSize > 0 ? tileSize : W;
    if (W < 4 || terrainTex.height() != W) return Error{ErrorCode::InvalidArgument, "The terrain texture must be square"};
    if (T < 4 || T > 256) return Error{ErrorCode::InvalidArgument, "Tiles must be 4 to 256 px"};
    if (W % T != 0 || W / T > 4)
        return Error{ErrorCode::InvalidArgument, "The terrain texture must be 1 to 4 tiles a side (" + std::to_string(T) + ", " +
                                                     std::to_string(2 * T) + ", " + std::to_string(3 * T) + " or " + std::to_string(4 * T) + " px)"};
    const int P = W / T;
    const bool transparentBase = baseTex.isNull();
    if (!transparentBase && (baseTex.width() != W || baseTex.height() != W))
        return Error{ErrorCode::InvalidArgument, "The base texture must be the same size as the terrain texture"};
    if (st.edgeNoise < 0 || st.edgeNoise > 0.9) return Error{ErrorCode::InvalidArgument, "Edge noise must be between 0 and 0.9"};
    const int L = std::clamp(st.noiseScale, 1, T);
    Rgba8 outline{};
    if (st.outline) {
        if (st.outlineColor) {
            outline = *st.outlineColor;
        } else {
            long r = 0, g = 0, b = 0, n = 0;
            for (const Rgba8 p : terrainTex.pixels()) if (p.a) { r += p.r; g += p.g; b += p.b; ++n; }
            n = std::max(1L, n);
            outline = {static_cast<std::uint8_t>(r / n * 55 / 100), static_cast<std::uint8_t>(g / n * 55 / 100), static_cast<std::uint8_t>(b / n * 55 / 100), 255};
        }
    }
    const Image base = transparentBase ? Image(W, W) : baseTex;
    Tileset t;
    t.scheme = s;
    t.tileSize = T;
    t.phases = P;
    const auto [cols, rows] = tilesetGrid(s);
    t.image = Image(P * cols * T, P * rows * T);
    if (s != AutotileScheme::Wang16) t.base = base;
    for (int i = 0; i < tileCount(s); ++i) {
        const TileSamples smp = tileSamples(s, i);
        // Coverage depends only on the tile, not the phase: every phase has
        // the same shape and edges, only the texture underneath moves.
        std::vector<std::uint8_t> cov(static_cast<std::size_t>(T) * static_cast<std::size_t>(T));
        for (int y = 0; y < T; ++y)
            for (int x = 0; x < T; ++x) {
                const double u = (x + 0.5) / T, v = (y + 0.5) / T;
                const double f = tileCoverage(smp, u, v, st.roundness);
                cov[static_cast<std::size_t>(y * T + x)] = f + st.edgeNoise * (periodicNoise(u, v, L, st.seed) - 0.5) >= 0.5;
            }
        auto covered = [&](int x, int y) { return cov[static_cast<std::size_t>(y * T + x)] != 0; };
        for (int py = 0; py < P; ++py)
            for (int px = 0; px < P; ++px) {
                const int ox = (px * cols + i % cols) * T, oy = (py * rows + i / cols) * T;
                const int tx0 = px * T, ty0 = py * T;
                for (int y = 0; y < T; ++y)
                    for (int x = 0; x < T; ++x) {
                        if (!covered(x, y)) { t.image.setPixel(ox + x, oy + y, base.pixel(tx0 + x, ty0 + y)); continue; }
                        bool edge = false;
                        if (st.outline) {
                            // In-tile neighbours only: what lies across the tile edge
                            // is the same for every tile that may touch it.
                            for (auto [dx, dy] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
                                const int nx = x + dx, ny = y + dy;
                                if (nx >= 0 && ny >= 0 && nx < T && ny < T && !covered(nx, ny)) edge = true;
                            }
                        }
                        t.image.setPixel(ox + x, oy + y, edge ? outline : terrainTex.pixel(tx0 + x, ty0 + y));
                    }
            }
    }
    return t;
}

std::string tilesetJson(const Tileset& t, const std::string& imageFile, const std::string& baseFile) {
    using nlohmann::ordered_json;
    const auto [cols, rows] = tilesetGrid(t.scheme);
    ordered_json tiles = ordered_json::array();
    for (int i = 0; i < tileCount(t.scheme); ++i) {
        ordered_json o = ordered_json::object();
        o["index"] = i;
        o["x"] = (i % cols) * t.tileSize;
        o["y"] = (i / cols) * t.tileSize;
        if (t.scheme == AutotileScheme::Wang16) o["corners"] = i;
        else o["mask"] = t.scheme == AutotileScheme::Blob47 ? static_cast<int>(blobMasks()[static_cast<std::size_t>(i)]) : i;
        const TileSamples smp = tileSamples(t.scheme, i);
        ordered_json sa = ordered_json::array();
        for (std::uint8_t v : smp) sa.push_back(v);
        o["samples"] = std::move(sa);
        tiles.push_back(std::move(o));
    }
    ordered_json root = ordered_json::object();
    root["scheme"] = std::string(toString(t.scheme));
    root["tileSize"] = t.tileSize;
    root["columns"] = cols;
    root["rows"] = rows;
    root["phases"] = t.phases;
    if (t.phases > 1) {
        ordered_json po = ordered_json::object();
        po["x"] = cols * t.tileSize;
        po["y"] = rows * t.tileSize;
        root["phaseOffset"] = std::move(po);
        root["phaseRule"] = "cell (x, y) uses the copy at (x mod phases, y mod phases) * phaseOffset";
    }
    root["image"] = imageFile;
    if (!baseFile.empty()) root["base"] = baseFile;
    root["tiles"] = std::move(tiles);
    return root.dump(2) + "\n";
}

Image tilesetTemplate(AutotileScheme s, int T, Rgba8 terrain, Rgba8 base) {
    const auto [cols, rows] = tilesetGrid(s);
    Image out(cols * T, rows * T);
    auto lerp = [&](std::uint8_t v) {
        auto mix = [&](std::uint8_t a, std::uint8_t b) { return static_cast<std::uint8_t>((a * (255 - v) + b * v + 127) / 255); };
        return Rgba8{mix(base.r, terrain.r), mix(base.g, terrain.g), mix(base.b, terrain.b), 255};
    };
    for (int i = 0; i < tileCount(s); ++i) {
        const TileSamples smp = tileSamples(s, i);
        const int ox = (i % cols) * T, oy = (i / cols) * T;
        for (int y = 0; y < T; ++y)
            for (int x = 0; x < T; ++x) {
                const int cx = std::min(2, x * 3 / T), cy = std::min(2, y * 3 / T);
                Rgba8 c = lerp(smp[static_cast<std::size_t>(cy * 3 + cx)]);
                if (x == 0 || y == 0) c = {static_cast<std::uint8_t>(c.r / 2), static_cast<std::uint8_t>(c.g / 2), static_cast<std::uint8_t>(c.b / 2), 255};
                out.setPixel(ox + x, oy + y, c);
            }
    }
    return out;
}

} // namespace acestudio
