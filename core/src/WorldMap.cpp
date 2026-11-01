#include "acestudio/core/WorldMap.h"

#include "acestudio/core/AssetFiles.h"
#include "acestudio/core/FileUtil.h"
#include "acestudio/core/PngCodec.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

namespace acestudio::world {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {
constexpr int kMaxCells = 4096;
constexpr std::size_t kMaxArea = 4u << 20;

std::size_t idx(int w, int x, int y) { return static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x); }

Rgba8 over(Rgba8 dst, Rgba8 src, int opacity = 255) {
    const int a = src.a * opacity / 255;
    if (a == 0) return dst;
    if (a == 255 || dst.a == 0) return {src.r, src.g, src.b, static_cast<std::uint8_t>(std::max<int>(a, dst.a))};
    const int outA = a + dst.a * (255 - a) / 255;
    auto ch = [&](int s, int d) { return static_cast<std::uint8_t>((s * a + d * dst.a * (255 - a) / 255) / std::max(1, outA)); };
    return {ch(src.r, dst.r), ch(src.g, dst.g), ch(src.b, dst.b), static_cast<std::uint8_t>(outA)};
}

void draw(Image& dst, const Image& src, const IntRect& from, int dx, int dy, int opacity = 255) {
    for (int y = 0; y < from.h; ++y) {
        const int ty = dy + y;
        if (ty < 0 || ty >= dst.height()) continue;
        for (int x = 0; x < from.w; ++x) {
            const int tx = dx + x;
            if (tx < 0 || tx >= dst.width()) continue;
            const Rgba8 s = src.pixel(from.x + x, from.y + y);
            if (s.a) dst.setPixel(tx, ty, over(dst.pixel(tx, ty), s, opacity));
        }
    }
}

std::string xmlEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
        case '&': o += "&amp;"; break;
        case '<': o += "&lt;"; break;
        case '>': o += "&gt;"; break;
        case '"': o += "&quot;"; break;
        default: o += c;
        }
    }
    return o;
}

// A file name from a user-given name: letters, digits, '-' and '_' only.
std::string safeName(const std::string& s, const std::string& fallback) {
    std::string o;
    for (char c : s)
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') o += c;
        else if (c == ' ' && !o.empty() && o.back() != '_') o += '_';
    if (o.empty()) o = fallback;
    return o.substr(0, 40);
}

fs::path fromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string toUtf8(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

// Only plain relative paths inside the map's asset folder are accepted.
bool safeRelative(const std::string& p) {
    if (p.empty() || p.find("..") != std::string::npos || p.find(':') != std::string::npos) return false;
    return p[0] != '/' && p[0] != '\\';
}
} // namespace

IntRect TileSheet::tileRect(int index) const {
    const int cols = columns();
    if (cols <= 0 || index < 0 || index >= count()) return {};
    return {(index % cols) * tileW, (index / cols) * tileH, tileW, tileH};
}

Result<WorldMap> WorldMap::create(int width, int height, int tileW, int tileH) {
    if (width < 1 || height < 1 || width > kMaxCells || height > kMaxCells ||
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) > kMaxArea)
        return Error(ErrorCode::InvalidArgument, "Map size must be 1 to 4096 cells per side (4M cells in all).");
    if (tileW < 1 || tileH < 1 || tileW > 512 || tileH > 512)
        return Error(ErrorCode::InvalidArgument, "Tile size must be 1 to 512 pixels.");
    WorldMap m;
    m.m_width = width;
    m.m_height = height;
    m.m_tileW = tileW;
    m.m_tileH = tileH;
    m.m_solid.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0);
    return m;
}

TileLayer& WorldMap::addLayer(const std::string& name) {
    TileLayer l;
    l.name = name;
    l.cells.assign(static_cast<std::size_t>(m_width) * static_cast<std::size_t>(m_height), kEmpty);
    layers.push_back(std::move(l));
    return layers.back();
}

Status WorldMap::addSheet(TileSheet sheet) {
    if (sheet.image.isNull()) return Error(ErrorCode::InvalidArgument, "The tile sheet has no image.");
    if (sheet.tileW < 1 || sheet.tileH < 1 || sheet.count() < 1)
        return Error(ErrorCode::InvalidArgument, "The tile size does not fit the sheet image.");
    if (sheet.count() > 0xFFFF) return Error(ErrorCode::InvalidArgument, "A tile sheet can hold at most 65535 tiles.");
    if (sheets.size() >= 0x7FFF) return Error(ErrorCode::InvalidArgument, "Too many tile sheets.");
    sheets.push_back(std::move(sheet));
    return {};
}

std::int32_t WorldMap::cell(int layer, int x, int y) const {
    if (layer < 0 || layer >= static_cast<int>(layers.size()) || x < 0 || y < 0 || x >= m_width || y >= m_height) return kEmpty;
    return layers[static_cast<std::size_t>(layer)].cells[idx(m_width, x, y)];
}

void WorldMap::setCell(int layer, int x, int y, std::int32_t value) {
    if (layer < 0 || layer >= static_cast<int>(layers.size()) || x < 0 || y < 0 || x >= m_width || y >= m_height) return;
    layers[static_cast<std::size_t>(layer)].cells[idx(m_width, x, y)] = value;
}

void WorldMap::fillRect(int layer, const IntRect& r, std::int32_t value) {
    for (int y = r.y; y < r.y + r.h; ++y)
        for (int x = r.x; x < r.x + r.w; ++x) setCell(layer, x, y, value);
}

void WorldMap::floodFill(int layer, int x, int y, std::int32_t value) {
    const std::int32_t target = cell(layer, x, y);
    if (layer < 0 || layer >= static_cast<int>(layers.size()) || x < 0 || y < 0 || x >= m_width || y >= m_height || target == value) return;
    std::vector<std::pair<int, int>> stack{{x, y}};
    auto& cells = layers[static_cast<std::size_t>(layer)].cells;
    while (!stack.empty()) {
        auto [cx, cy] = stack.back();
        stack.pop_back();
        if (cx < 0 || cy < 0 || cx >= m_width || cy >= m_height) continue;
        auto& c = cells[idx(m_width, cx, cy)];
        if (c != target) continue;
        c = value;
        stack.push_back({cx + 1, cy});
        stack.push_back({cx - 1, cy});
        stack.push_back({cx, cy + 1});
        stack.push_back({cx, cy - 1});
    }
}

bool WorldMap::solid(int x, int y) const {
    return x >= 0 && y >= 0 && x < m_width && y < m_height && m_solid[idx(m_width, x, y)] != 0;
}

void WorldMap::setSolid(int x, int y, bool on) {
    if (x >= 0 && y >= 0 && x < m_width && y < m_height) m_solid[idx(m_width, x, y)] = on ? 1 : 0;
}

void WorldMap::resize(int width, int height) {
    width = std::clamp(width, 1, kMaxCells);
    height = std::clamp(height, 1, kMaxCells);
    if (static_cast<std::size_t>(width) * static_cast<std::size_t>(height) > kMaxArea) return;
    auto remap = [&](auto& v, auto fillValue) {
        std::remove_reference_t<decltype(v)> out(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), fillValue);
        for (int y = 0; y < std::min(height, m_height); ++y)
            for (int x = 0; x < std::min(width, m_width); ++x) out[idx(width, x, y)] = v[idx(m_width, x, y)];
        v = std::move(out);
    };
    for (TileLayer& l : layers) remap(l.cells, kEmpty);
    remap(m_solid, std::uint8_t{0});
    m_width = width;
    m_height = height;
}

void WorldMap::solidFromLayer(int layer) {
    for (int y = 0; y < m_height; ++y)
        for (int x = 0; x < m_width; ++x)
            if (cell(layer, x, y) != kEmpty) setSolid(x, y, true);
}

Image render(const WorldMap& map, const RenderOptions& opt) {
    const bool camera = opt.viewWidth > 0;
    const int W = camera ? opt.viewWidth : map.pixelWidth();
    const int H = map.pixelHeight();
    const int cam = camera ? opt.cameraX : 0;
    Image out(W, H);
    // Parallax backgrounds repeat horizontally.
    for (const Parallax& p : map.backgrounds) {
        if (p.image.isNull()) continue;
        const int iw = p.image.width();
        const int shift = static_cast<int>(std::floor(cam * p.factor));
        int start = -(((shift % iw) + iw) % iw);
        for (int x = start; x < W; x += iw) draw(out, p.image, p.image.bounds(), x, p.y);
    }
    for (const TileLayer& l : map.layers) {
        if (!l.visible) continue;
        const int x0 = std::max(0, cam / map.tileW()), x1 = std::min(map.width(), (cam + W) / map.tileW() + 1);
        for (int y = 0; y < map.height(); ++y)
            for (int x = x0; x < x1; ++x) {
                const std::int32_t c = l.cells[static_cast<std::size_t>(y) * static_cast<std::size_t>(map.width()) + static_cast<std::size_t>(x)];
                if (c == kEmpty) continue;
                const int s = cellSheet(c);
                if (s < 0 || s >= static_cast<int>(map.sheets.size())) continue;
                const TileSheet& sh = map.sheets[static_cast<std::size_t>(s)];
                const IntRect r = sh.tileRect(cellTile(c));
                if (r.empty()) continue;
                // Tiles taller than the cell grow upwards, as in Tiled.
                draw(out, sh.image, r, x * map.tileW() - cam, (y + 1) * map.tileH() - r.h, l.opacity);
            }
    }
    if (opt.props) {
        // Props sorted by their bottom edge so lower ones stand in front.
        std::vector<const Prop*> order;
        for (const Prop& p : map.props) order.push_back(&p);
        std::stable_sort(order.begin(), order.end(), [&](const Prop* a, const Prop* b) {
            auto bottom = [&](const Prop* p) {
                return p->def >= 0 && p->def < static_cast<int>(map.propDefs.size()) ? p->y + map.propDefs[static_cast<std::size_t>(p->def)].image.height() : p->y;
            };
            return bottom(a) < bottom(b);
        });
        for (const Prop* p : order)
            if (p->def >= 0 && p->def < static_cast<int>(map.propDefs.size())) {
                const Image& img = map.propDefs[static_cast<std::size_t>(p->def)].image;
                draw(out, img, img.bounds(), p->x - cam, p->y);
            }
    }
    if (opt.lighting) {
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                Rgba8 c = out.pixel(x, y);
                if (!c.a) continue;
                double lr = map.ambient.r / 255.0, lg = map.ambient.g / 255.0, lb = map.ambient.b / 255.0;
                for (const Light& L : map.lights) {
                    const double dx = (x + cam) - L.x, dy = y - L.y;
                    const double d2 = dx * dx + dy * dy, r2 = double(L.radius) * L.radius;
                    if (d2 >= r2) continue;
                    const double f = 1.0 - std::sqrt(d2 / r2);
                    // Quantised falloff keeps the pixel-art look (4 bands).
                    const double band = std::ceil(f * 4.0) / 4.0;
                    lr += band * L.color.r / 255.0;
                    lg += band * L.color.g / 255.0;
                    lb += band * L.color.b / 255.0;
                }
                auto ch = [](std::uint8_t v, double k) { return static_cast<std::uint8_t>(std::clamp(v * std::min(k, 1.6), 0.0, 255.0)); };
                out.setPixel(x, y, {ch(c.r, lr), ch(c.g, lg), ch(c.b, lb), c.a});
            }
    }
    if (opt.collision) {
        const Rgba8 red{255, 40, 40, 110};
        for (int y = 0; y < map.height(); ++y)
            for (int x = 0; x < map.width(); ++x)
                if (map.solid(x, y))
                    for (int py = 0; py < map.tileH(); ++py)
                        for (int px = 0; px < map.tileW(); ++px) {
                            const int ox = x * map.tileW() + px - cam, oy = y * map.tileH() + py;
                            if (out.contains(ox, oy)) out.setPixel(ox, oy, over(out.pixel(ox, oy), red));
                        }
    }
    if (opt.grid) {
        const Rgba8 g{255, 255, 255, 40};
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                if ((x + cam) % map.tileW() == 0 || y % map.tileH() == 0) out.setPixel(x, y, over(out.pixel(x, y), g));
    }
    return out;
}

std::vector<IntRect> collisionRects(const WorldMap& map) {
    std::vector<std::uint8_t> used(static_cast<std::size_t>(map.width()) * static_cast<std::size_t>(map.height()), 0);
    std::vector<IntRect> out;
    for (int y = 0; y < map.height(); ++y)
        for (int x = 0; x < map.width(); ++x) {
            if (!map.solid(x, y) || used[idx(map.width(), x, y)]) continue;
            int w = 1;
            while (x + w < map.width() && map.solid(x + w, y) && !used[idx(map.width(), x + w, y)]) ++w;
            int h = 1;
            for (;;) {
                if (y + h >= map.height()) break;
                bool row = true;
                for (int i = 0; i < w && row; ++i) row = map.solid(x + i, y + h) && !used[idx(map.width(), x + i, y + h)];
                if (!row) break;
                ++h;
            }
            for (int yy = y; yy < y + h; ++yy)
                for (int xx = x; xx < x + w; ++xx) used[idx(map.width(), xx, yy)] = 1;
            out.push_back({x, y, w, h});
        }
    return out;
}

Status save(const WorldMap& map, const fs::path& file) {
    const fs::path assetsDir = fs::path(file).replace_extension().string() + ".assets";
    const std::string assetsName = toUtf8(assetsDir.filename());
    std::error_code ec;
    fs::create_directories(assetsDir, ec);
    if (ec) return Error(ErrorCode::IoError, "Could not create " + assetsDir.string() + ": " + ec.message());
    json j{{"format", "acestudio.map"}, {"version", 1}, {"width", map.width()}, {"height", map.height()},
           {"tileWidth", map.tileW()}, {"tileHeight", map.tileH()}, {"ambient", map.ambient.toHex(false)}};
    auto img = [&](const Image& image, const std::string& stem) -> Result<std::string> {
        const std::string rel = assetsName + "/" + stem + ".png";
        if (auto st = writePng(image, file.parent_path() / fromUtf8(rel)); !st) return st.error();
        return rel;
    };
    j["sheets"] = json::array();
    for (std::size_t i = 0; i < map.sheets.size(); ++i) {
        const TileSheet& s = map.sheets[i];
        auto rel = img(s.image, "sheet" + std::to_string(i) + "_" + safeName(s.name, "tiles"));
        if (!rel) return rel.error();
        j["sheets"].push_back({{"name", s.name}, {"image", rel.value()}, {"tileWidth", s.tileW}, {"tileHeight", s.tileH}});
    }
    j["layers"] = json::array();
    for (const TileLayer& l : map.layers) j["layers"].push_back({{"name", l.name}, {"visible", l.visible}, {"opacity", l.opacity}, {"cells", l.cells}});
    j["propDefs"] = json::array();
    for (std::size_t i = 0; i < map.propDefs.size(); ++i) {
        auto rel = img(map.propDefs[i].image, "prop" + std::to_string(i) + "_" + safeName(map.propDefs[i].name, "prop"));
        if (!rel) return rel.error();
        j["propDefs"].push_back({{"name", map.propDefs[i].name}, {"image", rel.value()}});
    }
    j["props"] = json::array();
    for (const Prop& p : map.props) j["props"].push_back({{"def", p.def}, {"x", p.x}, {"y", p.y}, {"solid", p.solid}});
    j["backgrounds"] = json::array();
    for (std::size_t i = 0; i < map.backgrounds.size(); ++i) {
        const Parallax& b = map.backgrounds[i];
        auto rel = img(b.image, "background" + std::to_string(i) + "_" + safeName(b.name, "bg"));
        if (!rel) return rel.error();
        j["backgrounds"].push_back({{"name", b.name}, {"image", rel.value()}, {"factor", b.factor}, {"y", b.y}});
    }
    j["lights"] = json::array();
    for (const Light& l : map.lights) j["lights"].push_back({{"x", l.x}, {"y", l.y}, {"radius", l.radius}, {"color", l.color.toHex(false)}});
    std::string solid;
    solid.reserve(static_cast<std::size_t>(map.width()) * static_cast<std::size_t>(map.height()));
    for (int y = 0; y < map.height(); ++y)
        for (int x = 0; x < map.width(); ++x) solid += map.solid(x, y) ? '1' : '0';
    j["collision"] = solid;
    const std::string text = j.dump(1);
    return writeFileAtomic(file, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

Result<WorldMap> load(const fs::path& file) {
    auto bytes = readFileBytes(file, 256u << 20);
    if (!bytes) return bytes.error();
    json j = json::parse(bytes.value().begin(), bytes.value().end(), nullptr, false);
    if (j.is_discarded() || !j.is_object() || j.value("format", "") != "acestudio.map")
        return Error(ErrorCode::CorruptData, file.string() + " is not an AceStudio map.");
    if (j.value("version", 0) > 1) return Error(ErrorCode::UnsupportedVersion, "This map was saved by a newer AceStudio.");
    try {
        auto m = WorldMap::create(j.at("width").get<int>(), j.at("height").get<int>(), j.at("tileWidth").get<int>(), j.at("tileHeight").get<int>());
        if (!m) return m.error();
        WorldMap& map = m.value();
        if (auto a = Rgba8::fromHex(j.value("ambient", "#7878a0"))) map.ambient = *a;
        auto image = [&](const json& e) -> Result<Image> {
            const std::string rel = e.at("image").get<std::string>();
            if (!safeRelative(rel)) return Error(ErrorCode::CorruptData, "The map refers to an image outside its folder: " + rel);
            return readPng(file.parent_path() / fromUtf8(rel));
        };
        for (const json& s : j.value("sheets", json::array())) {
            auto im = image(s);
            if (!im) return im.error();
            TileSheet sh{s.value("name", "tiles"), std::move(im).value(), s.value("tileWidth", map.tileW()), s.value("tileHeight", map.tileH())};
            if (auto st = map.addSheet(std::move(sh)); !st) return st.error();
        }
        const std::size_t area = static_cast<std::size_t>(map.width()) * static_cast<std::size_t>(map.height());
        for (const json& l : j.value("layers", json::array())) {
            TileLayer& layer = map.addLayer(l.value("name", "Layer"));
            layer.visible = l.value("visible", true);
            layer.opacity = std::clamp(l.value("opacity", 255), 0, 255);
            const json& cells = l.at("cells");
            if (!cells.is_array() || cells.size() != area) return Error(ErrorCode::CorruptData, "A map layer has the wrong number of cells.");
            for (std::size_t i = 0; i < area; ++i) {
                const std::int32_t c = cells[i].get<std::int32_t>();
                const int s = cellSheet(c);
                const bool ok = c == kEmpty || (s >= 0 && s < static_cast<int>(map.sheets.size()) &&
                                                cellTile(c) < map.sheets[static_cast<std::size_t>(s)].count());
                layer.cells[i] = ok ? c : kEmpty;
            }
        }
        for (const json& d : j.value("propDefs", json::array())) {
            auto im = image(d);
            if (!im) return im.error();
            map.propDefs.push_back({d.value("name", "prop"), std::move(im).value()});
        }
        for (const json& p : j.value("props", json::array())) {
            Prop pr{p.value("def", 0), p.value("x", 0), p.value("y", 0), p.value("solid", false)};
            if (pr.def >= 0 && pr.def < static_cast<int>(map.propDefs.size())) map.props.push_back(pr);
        }
        for (const json& b : j.value("backgrounds", json::array())) {
            auto im = image(b);
            if (!im) return im.error();
            map.backgrounds.push_back({b.value("name", "background"), std::move(im).value(), std::clamp(b.value("factor", 0.5), 0.0, 2.0), b.value("y", 0)});
        }
        for (const json& l : j.value("lights", json::array())) {
            Light L{l.value("x", 0), l.value("y", 0), std::clamp(l.value("radius", 64), 1, 4096)};
            if (auto c = Rgba8::fromHex(l.value("color", "#ffdca0"))) L.color = *c;
            map.lights.push_back(L);
        }
        const std::string solid = j.value("collision", std::string());
        if (solid.size() == area)
            for (std::size_t i = 0; i < area; ++i) map.m_solid[i] = solid[i] == '1' ? 1 : 0;
        return std::move(m).value();
    } catch (const json::exception& e) {
        return Error(ErrorCode::CorruptData, std::string("The map file is damaged: ") + e.what());
    }
}

Result<std::vector<fs::path>> exportTiled(const WorldMap& map, const fs::path& tmxFile) {
    const fs::path dir = tmxFile.parent_path();
    const std::string stem = toUtf8(tmxFile.stem());
    std::vector<fs::path> written;
    auto png = [&](const Image& img, const std::string& name) -> Result<std::string> {
        const std::string file = stem + "_" + name + ".png";
        if (auto st = writePng(img, dir / fromUtf8(file)); !st) return st.error();
        written.push_back(dir / fromUtf8(file));
        return file;
    };
    std::ostringstream x;
    int layerId = 1, objectId = 1;
    x << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      << "<map version=\"1.10\" tiledversion=\"1.10.2\" orientation=\"orthogonal\" renderorder=\"right-down\" width=\"" << map.width()
      << "\" height=\"" << map.height() << "\" tilewidth=\"" << map.tileW() << "\" tileheight=\"" << map.tileH() << "\" infinite=\"0\">\n";
    std::vector<int> firstGid;
    int gid = 1;
    for (std::size_t i = 0; i < map.sheets.size(); ++i) {
        const TileSheet& s = map.sheets[i];
        auto f = png(s.image, "tiles" + std::to_string(i));
        if (!f) return f.error();
        firstGid.push_back(gid);
        x << " <tileset firstgid=\"" << gid << "\" name=\"" << xmlEscape(s.name) << "\" tilewidth=\"" << s.tileW << "\" tileheight=\"" << s.tileH
          << "\" tilecount=\"" << s.count() << "\" columns=\"" << s.columns() << "\">\n"
          << "  <image source=\"" << xmlEscape(f.value()) << "\" width=\"" << s.image.width() << "\" height=\"" << s.image.height() << "\"/>\n </tileset>\n";
        gid += s.count();
    }
    const int propGid = gid;
    if (!map.propDefs.empty()) {
        int mw = 1, mh = 1;
        for (const PropDef& d : map.propDefs) { mw = std::max(mw, d.image.width()); mh = std::max(mh, d.image.height()); }
        x << " <tileset firstgid=\"" << propGid << "\" name=\"props\" tilewidth=\"" << mw << "\" tileheight=\"" << mh << "\" tilecount=\""
          << map.propDefs.size() << "\" columns=\"0\">\n  <grid orientation=\"orthogonal\" width=\"1\" height=\"1\"/>\n";
        for (std::size_t i = 0; i < map.propDefs.size(); ++i) {
            auto f = png(map.propDefs[i].image, "prop" + std::to_string(i) + "_" + safeName(map.propDefs[i].name, "prop"));
            if (!f) return f.error();
            x << "  <tile id=\"" << i << "\">\n   <image source=\"" << xmlEscape(f.value()) << "\" width=\"" << map.propDefs[i].image.width()
              << "\" height=\"" << map.propDefs[i].image.height() << "\"/>\n  </tile>\n";
        }
        x << " </tileset>\n";
    }
    for (std::size_t i = 0; i < map.backgrounds.size(); ++i) {
        const Parallax& b = map.backgrounds[i];
        auto f = png(b.image, "background" + std::to_string(i));
        if (!f) return f.error();
        x << " <imagelayer id=\"" << layerId++ << "\" name=\"" << xmlEscape(b.name) << "\" offsety=\"" << b.y << "\" parallaxx=\"" << b.factor
          << "\" repeatx=\"1\">\n  <image source=\"" << xmlEscape(f.value()) << "\" width=\"" << b.image.width() << "\" height=\"" << b.image.height()
          << "\"/>\n </imagelayer>\n";
    }
    for (const TileLayer& l : map.layers) {
        x << " <layer id=\"" << layerId++ << "\" name=\"" << xmlEscape(l.name) << "\" width=\"" << map.width() << "\" height=\"" << map.height() << "\"";
        if (!l.visible) x << " visible=\"0\"";
        if (l.opacity < 255) x << " opacity=\"" << l.opacity / 255.0 << "\"";
        x << ">\n  <data encoding=\"csv\">\n";
        for (int y = 0; y < map.height(); ++y) {
            for (int c = 0; c < map.width(); ++c) {
                const std::int32_t v = l.cells[static_cast<std::size_t>(y) * static_cast<std::size_t>(map.width()) + static_cast<std::size_t>(c)];
                const int g = v == kEmpty ? 0 : firstGid[static_cast<std::size_t>(cellSheet(v))] + cellTile(v);
                x << g << ((y == map.height() - 1 && c == map.width() - 1) ? "" : ",");
            }
            x << "\n";
        }
        x << "  </data>\n </layer>\n";
    }
    if (!map.props.empty()) {
        x << " <objectgroup id=\"" << layerId++ << "\" name=\"props\">\n";
        for (const Prop& p : map.props) {
            const Image& img = map.propDefs[static_cast<std::size_t>(p.def)].image;
            // Tile objects are anchored at their bottom-left corner.
            x << "  <object id=\"" << objectId++ << "\" name=\"" << xmlEscape(map.propDefs[static_cast<std::size_t>(p.def)].name) << "\" gid=\""
              << propGid + p.def << "\" x=\"" << p.x << "\" y=\"" << p.y + img.height() << "\" width=\"" << img.width() << "\" height=\""
              << img.height() << "\"";
            if (p.solid) x << ">\n   <properties>\n    <property name=\"solid\" type=\"bool\" value=\"true\"/>\n   </properties>\n  </object>\n";
            else x << "/>\n";
        }
        x << " </objectgroup>\n";
    }
    const auto rects = collisionRects(map);
    x << " <objectgroup id=\"" << layerId++ << "\" name=\"collision\" visible=\"0\">\n";
    for (const IntRect& r : rects)
        x << "  <object id=\"" << objectId++ << "\" type=\"solid\" x=\"" << r.x * map.tileW() << "\" y=\"" << r.y * map.tileH() << "\" width=\""
          << r.w * map.tileW() << "\" height=\"" << r.h * map.tileH() << "\"/>\n";
    for (const Prop& p : map.props)
        if (p.solid) {
            const Image& img = map.propDefs[static_cast<std::size_t>(p.def)].image;
            x << "  <object id=\"" << objectId++ << "\" type=\"solid\" x=\"" << p.x << "\" y=\"" << p.y << "\" width=\"" << img.width()
              << "\" height=\"" << img.height() << "\"/>\n";
        }
    x << " </objectgroup>\n</map>\n";
    std::string text = x.str();
    // nextlayerid / nextobjectid go on the map element.
    const std::string attrs = " nextlayerid=\"" + std::to_string(layerId) + "\" nextobjectid=\"" + std::to_string(objectId) + "\"";
    text.insert(text.find(" infinite=\"0\"") + 13, attrs);
    if (auto st = writeTextFile(tmxFile, text); !st) return st.error();
    written.push_back(tmxFile);
    auto rendered = png(render(map), "preview");
    if (!rendered) return rendered.error();
    return written;
}

} // namespace acestudio::world
