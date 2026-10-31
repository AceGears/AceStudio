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

Image Tileset::tile(int index) const {
    if (index == kBaseTile) return base;
    const auto [cols, rows] = tilesetGrid(scheme);
    (void)rows;
    return image.copy({(index % cols) * tileSize, (index / cols) * tileSize, tileSize, tileSize});
}

Status validateTileset(const Tileset& t) {
    if (t.tileSize < 2 || t.tileSize > 512) return Error{ErrorCode::InvalidArgument, "Tile size must be between 2 and 512 pixels"};
    const auto [cols, rows] = tilesetGrid(t.scheme);
    if (t.image.width() != cols * t.tileSize || t.image.height() != rows * t.tileSize)
        return Error{ErrorCode::InvalidArgument, "A " + std::string(toString(t.scheme)) + " tileset of " + std::to_string(t.tileSize) +
                                                   " px tiles must be " + std::to_string(cols * t.tileSize) + "x" +
                                                   std::to_string(rows * t.tileSize) + " px (" + std::to_string(cols) + "x" +
                                                   std::to_string(rows) + " tiles)"};
    if (!t.base.isNull() && (t.base.width() != t.tileSize || t.base.height() != t.tileSize))
        return Error{ErrorCode::InvalidArgument, "The base tile must be one tile in size"};
    return {};
}

Result<Image> renderMap(const TerrainMap& map, const Tileset& t) {
    if (auto st = validateTileset(t); !st) return st.error();
    const int T = t.tileSize;
    auto out = Image::create(std::max(1, map.width() * T), std::max(1, map.height() * T));
    if (!out) return out.error();
    Image img = std::move(out).value();
    const TilePlacement p = autotile(map, t.scheme);
    std::vector<Image> tiles;
    for (int i = 0; i < tileCount(t.scheme); ++i) tiles.push_back(t.tile(i));
    // The dual grid (Wang16) is offset by half a tile so tiles straddle cells.
    const int off = t.scheme == AutotileScheme::Wang16 ? -T / 2 : 0;
    for (int y = 0; y < p.rows; ++y)
        for (int x = 0; x < p.columns; ++x) {
            const int idx = p.at(x, y);
            if (idx < 0) {
                if (!t.base.isNull()) img.blit(t.base, x * T, y * T);
                continue;
            }
            const Image& tile = tiles[static_cast<std::size_t>(idx)];
            for (int ty = 0; ty < T; ++ty)
                for (int tx = 0; tx < T; ++tx) {
                    const int px = x * T + off + tx, py = y * T + off + ty;
                    if (img.contains(px, py)) img.setPixel(px, py, tile.pixel(tx, ty));
                }
        }
    return img;
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
bool sideMatches(const TileSamples& a, const TileSamples& b, bool horizontal) {
    // a left of b: a's right column (TR, R, BR) vs b's left (TL, L, BL);
    // a above b: a's bottom row (BL, B, BR) vs b's top (TL, T, TR).
    if (horizontal) return a[2] == b[0] && a[5] == b[3] && a[8] == b[6];
    return a[6] == b[0] && a[7] == b[1] && a[8] == b[2];
}
} // namespace

Result<SeamReport> checkSeams(const Tileset& t, const SeamOptions& o) {
    if (auto st = validateTileset(t); !st) return st.error();
    const int n = tileCount(t.scheme), T = t.tileSize;
    std::vector<int> ids;
    for (int i = 0; i < n; ++i) ids.push_back(i);
    if (t.scheme != AutotileScheme::Wang16 && !t.base.isNull()) ids.push_back(kBaseTile);
    std::vector<Image> img(ids.size());
    std::vector<TileSamples> smp(ids.size());
    std::vector<double> typX(ids.size()), typY(ids.size());
    for (std::size_t k = 0; k < ids.size(); ++k) {
        img[k] = t.tile(ids[k]);
        smp[k] = ids[k] == kBaseTile ? TileSamples{} : tileSamples(t.scheme, ids[k]);
        typX[k] = interiorDiff(img[k], true);
        typY[k] = interiorDiff(img[k], false);
    }
    SeamReport r;
    for (std::size_t a = 0; a < ids.size(); ++a)
        for (std::size_t b = 0; b < ids.size(); ++b)
            for (bool horizontal : {true, false}) {
                if (!sideMatches(smp[a], smp[b], horizontal)) continue;
                ++r.pairsChecked;
                const double jump = edgeDiff(img[a], T - 1, img[b], 0, horizontal);
                const double typical = horizontal ? (typX[a] + typX[b]) / 2 : (typY[a] + typY[b]) / 2;
                if (jump > o.factor * typical && jump > o.floor) r.issues.push_back({ids[a], ids[b], horizontal, jump, typical});
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

Result<Tileset> generateTileset(AutotileScheme s, const Image& baseTex, const Image& terrainTex, const TilesetStyle& st) {
    const int T = terrainTex.width();
    if (T < 4 || T > 256 || terrainTex.height() != T) return Error{ErrorCode::InvalidArgument, "The terrain texture must be square, 4 to 256 px (one tile)"};
    if (baseTex.width() != T || baseTex.height() != T) return Error{ErrorCode::InvalidArgument, "The base texture must be the same size as the terrain texture"};
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
    Tileset t;
    t.scheme = s;
    t.tileSize = T;
    const auto [cols, rows] = tilesetGrid(s);
    t.image = Image(cols * T, rows * T);
    if (s != AutotileScheme::Wang16) t.base = baseTex;
    for (int i = 0; i < tileCount(s); ++i) {
        const TileSamples smp = tileSamples(s, i);
        auto covered = [&](int x, int y) {
            const double u = (x + 0.5) / T, v = (y + 0.5) / T;
            const double f = tileCoverage(smp, u, v, st.roundness);
            return f + st.edgeNoise * (periodicNoise(u, v, L, st.seed) - 0.5) >= 0.5;
        };
        const int ox = (i % cols) * T, oy = (i / cols) * T;
        for (int y = 0; y < T; ++y)
            for (int x = 0; x < T; ++x) {
                if (!covered(x, y)) { t.image.setPixel(ox + x, oy + y, baseTex.pixel(x, y)); continue; }
                bool edge = false;
                if (st.outline) {
                    // In-tile neighbours only: what lies across the tile edge
                    // is the same for every tile that may touch it.
                    for (auto [dx, dy] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
                        const int nx = x + dx, ny = y + dy;
                        if (nx >= 0 && ny >= 0 && nx < T && ny < T && !covered(nx, ny)) edge = true;
                    }
                }
                t.image.setPixel(ox + x, oy + y, edge ? outline : terrainTex.pixel(x, y));
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
