// acestudio-cli asset commands (Phase 7): tilesets, seam checks, atlases,
// slicing, UI kits and engine export, over the same core code as the app.

#include "CliCommon.h"

#include "acestudio/core/AssetGen.h"
#include "acestudio/core/SpriteMotion.h"
#include "acestudio/core/WorldMap.h"
#include "acestudio/core/AssetFiles.h"
#include "acestudio/core/AssetTools.h"
#include "acestudio/core/Atlas.h"
#include "acestudio/core/Autotile.h"
#include "acestudio/core/PngCodec.h"
#include "acestudio/core/Transform.h"

#include <filesystem>
#include <sstream>

namespace acestudio::cli {

namespace fs = std::filesystem;

namespace {

std::vector<std::string> splitList(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        while (!item.empty() && item.front() == ' ') item.erase(item.begin());
        while (!item.empty() && item.back() == ' ') item.pop_back();
        if (!item.empty()) out.push_back(item);
    }
    return out;
}

std::optional<double> optDouble(const Args& a, const std::string& k) {
    auto v = a.opt(k);
    if (!v) return std::nullopt;
    try {
        std::size_t used = 0;
        const double d = std::stod(*v, &used);
        if (used != v->size()) return std::nullopt;
        return d;
    } catch (...) {
        return std::nullopt;
    }
}

// WxH, e.g. 16x16.
std::optional<std::pair<int, int>> parseSize(const std::string& s) {
    const std::size_t x = s.find('x');
    if (x == std::string::npos) return std::nullopt;
    auto w = parseInt(s.substr(0, x)), h = parseInt(s.substr(x + 1));
    if (!w || !h || *w < 1 || *h < 1) return std::nullopt;
    return std::pair{*w, *h};
}

// A,B, e.g. 2,0.
std::optional<std::pair<int, int>> parsePair(const std::string& s) {
    const auto p = splitList(s);
    if (p.size() != 2) return std::nullopt;
    auto a = parseInt(p[0]), b = parseInt(p[1]);
    if (!a || !b) return std::nullopt;
    return std::pair{*a, *b};
}

// Square crop from the centre, nearest-scaled to size x size.
Result<Image> fitSquare(const Image& img, int size) {
    const int side = std::min(img.width(), img.height());
    Image sq = img.copy({(img.width() - side) / 2, (img.height() - side) / 2, side, side});
    if (side == size) return sq;
    return scaleNearest(sq, size, size);
}

json filesJson(const WrittenFiles& w) {
    json files = json::array(), notes = json::array();
    for (const auto& f : w.files) files.push_back(f.string());
    for (const auto& n : w.notes) notes.push_back(n);
    return {{"files", files}, {"notes", notes}};
}

std::string filesHuman(const WrittenFiles& w) {
    std::string s = "Wrote " + std::to_string(w.files.size()) + " file(s):\n";
    for (const auto& f : w.files) s += "  " + f.string() + "\n";
    for (const auto& n : w.notes) s += "Note: " + n + "\n";
    return s;
}

std::optional<AutotileScheme> schemeArg(const Args& a) {
    return autotileSchemeFromString(a.opt("scheme").value_or("blob47"));
}

Result<unsigned> enginesArg(const Args& a) { return parseEngineTargets(a.opt("engines").value_or("none")); }

} // namespace

// tileset --base B.png --terrain T1.png[,T2.png,...] --out tiles.png
int cmdTileset(const Args& a, Output& out) {
    const auto scheme = schemeArg(a);
    if (!scheme) return out.fail(kUsage, "tileset: --scheme must be blob47, wang16 or cardinal16");
    const auto basePath = a.opt("base");
    const auto terrains = splitList(a.opt("terrain").value_or(""));
    const auto outPath = a.opt("out");
    if (!basePath || terrains.empty() || !outPath) return out.fail(kUsage, "tileset: --base, --terrain and --out are required");
    if (terrains.size() > 3) return out.fail(kUsage, "tileset: at most three terrain levels");
    const int tile = optInt(a, "tile").value_or(16);
    const int phases = optInt(a, "phases").value_or(1);
    if (tile < 4 || tile > 256) return out.fail(kUsage, "tileset: --tile must be 4..256");
    if (phases < 1 || phases > 4) return out.fail(kUsage, "tileset: --phases must be 1..4");
    TilesetStyle st;
    st.roundness = optDouble(a, "roundness").value_or(1.0);
    st.edgeNoise = optDouble(a, "noise").value_or(0.3);
    st.noiseScale = optInt(a, "noise-scale").value_or(4);
    st.outline = a.flags.count("no-outline") == 0;
    st.seed = static_cast<std::uint32_t>(optInt(a, "seed").value_or(1));
    auto engines = enginesArg(a);
    if (!engines) return out.fail(engines.error());

    auto load = [&](const std::string& p) -> Result<Image> {
        auto img = readPng(p);
        if (!img) return img.error();
        return fitSquare(img.value(), tile * phases);
    };
    auto base = load(*basePath);
    if (!base) return out.fail(base.error());
    std::vector<Tileset> levels;
    for (std::size_t i = 0; i < terrains.size(); ++i) {
        auto tex = load(terrains[i]);
        if (!tex) return out.fail(tex.error());
        auto t = generateTileset(*scheme, i == 0 ? base.value() : Image(), tex.value(), st, tile);
        if (!t) return out.fail(t.error());
        levels.push_back(std::move(t).value());
    }
    // Seam check of the result, and of the textures themselves.
    json seams = json::array();
    int issues = 0;
    for (std::size_t i = 0; i < levels.size(); ++i) {
        auto rep = checkSeams(levels[i]);
        if (!rep) return out.fail(rep.error());
        seams.push_back({{"level", i + 1}, {"pairs", rep.value().pairsChecked}, {"seams", rep.value().issues.size()}});
        issues += static_cast<int>(rep.value().issues.size());
    }
    GodotTerrainNames names;
    const auto nameList = splitList(a.opt("names").value_or(""));
    if (!nameList.empty()) names.base = nameList[0];
    for (std::size_t i = 1; i < nameList.size(); ++i) names.levels.push_back(nameList[i]);
    AssetWriteOptions wo;
    wo.engines = engines.value();
    wo.overwrite = a.force;
    wo.pixelsPerUnit = optInt(a, "ppu").value_or(0);
    auto w = writeTilesetFiles(levels, *outPath, wo, names);
    if (!w) return out.fail(w.error());
    json j = filesJson(w.value());
    j["seamCheck"] = seams;
    j["scheme"] = std::string(toString(*scheme));
    j["tileSize"] = tile;
    j["phases"] = phases;
    std::string human = filesHuman(w.value());
    human += issues == 0 ? "Seam check: no seams.\n" : "Seam check: " + std::to_string(issues) + " seam(s); the textures probably do not tile.\n";
    return out.success(std::move(j), human);
}

// seams tiles.png --scheme S [--phases N] [--base base.png]
int cmdSeams(const Args& a, Output& out) {
    if (a.positional.size() != 2) return out.fail(kUsage, "seams: expected one tileset PNG");
    const auto scheme = schemeArg(a);
    if (!scheme) return out.fail(kUsage, "seams: --scheme must be blob47, wang16 or cardinal16");
    auto img = readPng(a.positional[1]);
    if (!img) return out.fail(img.error());
    Tileset t;
    t.scheme = *scheme;
    t.phases = optInt(a, "phases").value_or(1);
    t.image = std::move(img).value();
    if (t.phases < 1 || t.phases > 4) return out.fail(kUsage, "seams: --phases must be 1..4");
    t.tileSize = t.image.width() / (tilesetGrid(*scheme).first * t.phases);
    if (auto b = a.opt("base")) {
        auto bi = readPng(*b);
        if (!bi) return out.fail(bi.error());
        t.base = std::move(bi).value();
    }
    auto rep = checkSeams(t);
    if (!rep) return out.fail(rep.error());
    json issues = json::array();
    std::string human;
    for (const auto& i : rep.value().issues) {
        issues.push_back({{"a", i.a}, {"b", i.b}, {"edge", i.horizontal ? "right" : "bottom"}, {"jump", i.jump}, {"phase", {i.phaseX, i.phaseY}}});
        human += "  tile " + std::to_string(i.a) + (i.horizontal ? " -> right -> " : " -> below -> ") + std::to_string(i.b) + "\n";
    }
    json j = {{"pairs", rep.value().pairsChecked}, {"seams", rep.value().issues.size()}, {"issues", issues}, {"tileSize", t.tileSize}};
    if (!rep.value().ok()) {
        // Seams are invalid data for a tileset: exit 3, with the details.
        if (out.isJson()) {
            j["ok"] = false;
            j["exitCode"] = kCorrupt;
            std::cout << j.dump() << "\n";
            return kCorrupt;
        }
        std::cerr << rep.value().issues.size() << " seam(s) in " << rep.value().pairsChecked << " pairs:\n" << human;
        return kCorrupt;
    }
    return out.success(std::move(j), "No seams in " + std::to_string(rep.value().pairsChecked) + " pairs of tiles that can touch.\n");
}

// atlas a.png b.png ... --out atlas.png
int cmdAtlas(const Args& a, Output& out) {
    if (a.positional.size() < 2) return out.fail(kUsage, "atlas: expected PNG files to pack");
    const auto outPath = a.opt("out");
    if (!outPath) return out.fail(kUsage, "atlas: --out is required");
    std::vector<AtlasItem> items;
    for (std::size_t i = 1; i < a.positional.size(); ++i) {
        auto img = readPng(a.positional[i]);
        if (!img) return out.fail(img.error());
        items.push_back({fs::path(a.positional[i]).stem().string(), std::move(img).value()});
    }
    AtlasOptions o;
    o.maxWidth = o.maxHeight = optInt(a, "max").value_or(4096);
    o.padding = optInt(a, "padding").value_or(1);
    o.border = optInt(a, "border").value_or(0);
    o.extrude = optInt(a, "extrude").value_or(0);
    o.trim = a.flags.count("trim") > 0;
    o.powerOfTwo = a.flags.count("pot") > 0;
    o.allowRotation = a.flags.count("rotate") > 0;
    o.mergeDuplicates = a.flags.count("no-merge") == 0;
    o.multiplePages = true;
    auto atlas = packAtlas(items, o);
    if (!atlas) return out.fail(atlas.error());
    auto engines = enginesArg(a);
    if (!engines) return out.fail(engines.error());
    fs::path png = *outPath;
    if (png.extension() != ".png") png += ".png";
    AssetWriteOptions wo;
    wo.engines = engines.value();
    wo.overwrite = a.force;
    wo.pixelsPerUnit = optInt(a, "ppu").value_or(0);
    auto w = writeAtlasFiles(atlas.value(), png, wo);
    if (!w) return out.fail(w.error());
    json j = filesJson(w.value());
    j["pages"] = atlas.value().pages.size();
    j["sprites"] = items.size();
    return out.success(std::move(j), filesHuman(w.value()));
}

// slice sheet.png --out folder [--grid WxH [--offset X,Y] [--spacing X,Y]]
int cmdSlice(const Args& a, Output& out) {
    if (a.positional.size() != 2) return out.fail(kUsage, "slice: expected one sprite sheet PNG");
    const auto outDir = a.opt("out");
    if (!outDir) return out.fail(kUsage, "slice: --out (a folder) is required");
    auto img = readPng(a.positional[1]);
    if (!img) return out.fail(img.error());
    SliceOptions so;
    so.tolerance = optInt(a, "tolerance").value_or(8);
    so.mergeDistance = optInt(a, "merge").value_or(2);
    so.minPixels = optInt(a, "min-pixels").value_or(4);
    if (auto bg = a.opt("background")) {
        auto c = Rgba8::fromHex(*bg);
        if (!c) return out.fail(kUsage, "slice: --background expects #rrggbb");
        so.background = *c;
    }
    std::vector<SlicedSprite> slices;
    if (auto cells = a.opt("cells")) {
        // A generated sheet: N x M subjects, spacing not exact; background removed per cell.
        auto cr = parseSize(*cells);
        if (!cr) return out.fail(kUsage, "slice: --cells expects COLSxROWS");
        const auto grid = gen::extractGrid(img.value(), cr->first, cr->second);
        if (grid.empty()) return out.fail(kCorrupt, "slice: no subjects found in the cells");
        const fs::path dir = *outDir;
        std::error_code ec;
        fs::create_directories(dir, ec);
        const std::string stem = fs::path(a.positional[1]).stem().string();
        json files = json::array(), rects = json::array();
        for (const auto& c : grid) {
            const std::string name = stem + "_r" + std::to_string(c.row + 1) + "c" + std::to_string(c.column + 1);
            const fs::path p = dir / (name + ".png");
            if (!a.force && fs::exists(p, ec)) return out.fail(kIo, p.string() + " already exists (use --force)");
            if (auto st = writePng(c.image, p); !st) return out.fail(st.error());
            files.push_back(p.string());
            rects.push_back({{"name", name}, {"x", c.rect.x}, {"y", c.rect.y}, {"w", c.rect.w}, {"h", c.rect.h}});
        }
        return out.success({{"files", files}, {"sprites", rects}}, "Cut " + std::to_string(grid.size()) + " subject(s) into " + dir.string() + "\n");
    }
    if (auto g = a.opt("grid")) {
        GridGuess grid;
        if (*g == "auto") {
            grid = detectGrid(img.value(), so);
            if (!grid.found) return out.fail(kCorrupt, "slice: no regular grid found; try without --grid");
        } else {
            auto sz = parseSize(*g);
            if (!sz) return out.fail(kUsage, "slice: --grid expects WxH or auto");
            grid.cellWidth = sz->first;
            grid.cellHeight = sz->second;
            if (auto off = a.opt("offset")) {
                auto p = parsePair(*off);
                if (!p) return out.fail(kUsage, "slice: --offset expects X,Y");
                grid.offsetX = p->first;
                grid.offsetY = p->second;
            }
            if (auto sp = a.opt("spacing")) {
                auto p = parsePair(*sp);
                if (!p) return out.fail(kUsage, "slice: --spacing expects X,Y");
                grid.spacingX = p->first;
                grid.spacingY = p->second;
            }
            grid.columns = (img.value().width() - grid.offsetX + grid.spacingX) / (grid.cellWidth + grid.spacingX);
            grid.rows = (img.value().height() - grid.offsetY + grid.spacingY) / (grid.cellHeight + grid.spacingY);
            grid.found = grid.columns > 0 && grid.rows > 0;
        }
        slices = sliceGrid(img.value(), grid, a.flags.count("keep-empty") == 0, so);
    } else {
        slices = detectSprites(img.value(), so);
    }
    if (slices.empty()) return out.fail(kCorrupt, "slice: no sprites found");
    const fs::path dir = *outDir;
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string stem = fs::path(a.positional[1]).stem().string();
    const fs::path js = dir / (stem + ".slices.json");
    if (!a.force) {
        for (const auto& s : slices)
            if (fs::exists(dir / (s.name + ".png"), ec)) return out.fail(kIo, (dir / (s.name + ".png")).string() + " already exists (use --force)");
        if (fs::exists(js, ec)) return out.fail(kIo, js.string() + " already exists (use --force)");
    }
    json files = json::array(), rects = json::array();
    for (const auto& s : slices) {
        const fs::path p = dir / (s.name + ".png");
        if (auto st = writePng(img.value().copy(s.rect), p); !st) return out.fail(st.error());
        files.push_back(p.string());
        rects.push_back({{"name", s.name}, {"x", s.rect.x}, {"y", s.rect.y}, {"w", s.rect.w}, {"h", s.rect.h}});
    }
    if (auto st = writeTextFile(js, slicesJson(slices, fs::path(a.positional[1]).filename().string(), img.value().width(), img.value().height())); !st)
        return out.fail(st.error());
    files.push_back(js.string());
    return out.success({{"files", files}, {"sprites", rects}},
                       "Found " + std::to_string(slices.size()) + " sprite(s); wrote them and " + js.string() + "\n");
}

// animate sprite.png --out dir [--action a,b|all] [--frames N] [--amount X] [--body B] [--face left]
int cmdAnimate(const Args& a, Output& out) {
    if (a.positional.size() != 2) return out.fail(kUsage, "animate: expected one sprite PNG");
    const auto outDir = a.opt("out");
    if (!outDir) return out.fail(kUsage, "animate: --out (a folder) is required");
    auto img = readPng(a.positional[1]);
    if (!img) return out.fail(img.error());
    std::vector<motion::Action> list;
    const std::string spec = a.opt("action").value_or("idle");
    if (spec == "all") {
        for (const auto& info : motion::actions())
            if (info.action != motion::Action::Custom) list.push_back(info.action);
    } else {
        std::size_t start = 0;
        while (start <= spec.size()) {
            const std::size_t comma = spec.find(',', start);
            const std::string key = spec.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            auto act = motion::actionFromKey(key);
            if (!act) return out.fail(kUsage, "animate: unknown action '" + key + "'");
            list.push_back(*act);
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }
    motion::Options o;
    o.frames = optInt(a, "frames").value_or(0);
    if (auto am = a.opt("amount")) {
        try { o.amount = std::stod(*am); } catch (...) { return out.fail(kUsage, "animate: --amount expects a number"); }
    }
    if (auto b = a.opt("body")) {
        o.body = motion::bodyFromString(*b);
        if (!o.body) return out.fail(kUsage, "animate: --body expects biped, quadruped, blob, flyer or object");
    }
    o.faceLeft = a.opt("face").value_or("right") == "left";
    auto pack = motion::animatePack(img.value(), list, o);
    if (!pack) return out.fail(pack.error());
    const fs::path dir = *outDir;
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string stem = fs::path(a.positional[1]).stem().string();
    json clips = json::array(), files = json::array();
    for (const auto& c : pack.value()) {
        const fs::path png = dir / (stem + "_" + c.name + ".png");
        const fs::path js = dir / (stem + "_" + c.name + ".json");
        if (!a.force && (fs::exists(png, ec) || fs::exists(js, ec))) return out.fail(kIo, png.string() + " already exists (use --force)");
        const int fw = c.frames[0].width(), fh = c.frames[0].height();
        Image strip(fw * static_cast<int>(c.frames.size()), fh);
        for (std::size_t i = 0; i < c.frames.size(); ++i) strip.blit(c.frames[i], static_cast<int>(i) * fw, 0);
        if (auto st = writePng(strip, png); !st) return out.fail(st.error());
        const motion::ClipCheck chk = motion::checkClip(c);
        json j{{"action", c.name}, {"body", motion::toString(c.body)}, {"frameWidth", fw}, {"frameHeight", fh},
               {"frames", c.frames.size()}, {"durations", c.durations}, {"loop", c.loops}, {"image", png.filename().string()},
               {"check", {{"maxAreaChange", chk.maxAreaChange}, {"minNeighbourIoU", chk.minNeighbourIoU}, {"loopIoU", chk.loopIoU},
                          {"issues", chk.issues}}}};
        if (auto st = writeTextFile(js, j.dump(2)); !st) return out.fail(st.error());
        files.push_back(png.string());
        files.push_back(js.string());
        clips.push_back(j);
    }
    return out.success({{"files", files}, {"clips", clips}},
                       "Wrote " + std::to_string(pack.value().size()) + " animation(s) to " + dir.string() + "\n");
}

// map info|render|export <file.acemap> [--out file] [--lighting] [--collision] [--show-grid]
int cmdMap(const Args& a, Output& out) {
    if (a.positional.size() != 3) return out.fail(kUsage, "map: expected info, render or export and an .acemap file");
    const std::string verb = a.positional[1];
    auto m = world::load(a.positional[2]);
    if (!m) return out.fail(m.error());
    const world::WorldMap& map = m.value();
    if (verb == "info") {
        json layers = json::array();
        for (const auto& l : map.layers) {
            int used = 0;
            for (auto c : l.cells) used += c != world::kEmpty;
            layers.push_back({{"name", l.name}, {"visible", l.visible}, {"tiles", used}});
        }
        json sheets = json::array();
        for (const auto& s : map.sheets) sheets.push_back({{"name", s.name}, {"tileWidth", s.tileW}, {"tileHeight", s.tileH}, {"tiles", s.count()}});
        int solid = 0;
        for (int y = 0; y < map.height(); ++y)
            for (int x = 0; x < map.width(); ++x) solid += map.solid(x, y);
        return out.success({{"width", map.width()}, {"height", map.height()}, {"tileWidth", map.tileW()}, {"tileHeight", map.tileH()},
                            {"layers", layers}, {"sheets", sheets}, {"props", map.props.size()}, {"propTypes", map.propDefs.size()},
                            {"backgrounds", map.backgrounds.size()}, {"lights", map.lights.size()}, {"solidCells", solid},
                            {"collisionRects", world::collisionRects(map).size()}},
                           std::to_string(map.width()) + " x " + std::to_string(map.height()) + " cells of " + std::to_string(map.tileW()) + " x " +
                               std::to_string(map.tileH()) + " px, " + std::to_string(map.layers.size()) + " layer(s), " +
                               std::to_string(map.props.size()) + " prop(s), " + std::to_string(solid) + " solid cell(s)\n");
    }
    const auto target = a.opt("out");
    if (!target) return out.fail(kUsage, "map: --out is required");
    std::error_code ec;
    if (!a.force && fs::exists(*target, ec)) return out.fail(kIo, *target + " already exists (use --force)");
    if (verb == "render") {
        world::RenderOptions o;
        o.lighting = a.flags.count("lighting") > 0;
        o.collision = a.flags.count("collision") > 0;
        o.grid = a.flags.count("show-grid") > 0;
        const Image img = world::render(map, o);
        if (auto st = writePng(img, *target); !st) return out.fail(st.error());
        return out.success({{"path", *target}, {"width", img.width()}, {"height", img.height()}}, "Wrote " + *target + "\n");
    }
    if (verb == "export") {
        auto files = world::exportTiled(map, *target);
        if (!files) return out.fail(files.error());
        json list = json::array();
        for (const auto& f : files.value()) list.push_back(f.string());
        return out.success({{"files", list}}, "Wrote " + std::to_string(files.value().size()) + " file(s) for " + *target + "\n");
    }
    return out.fail(kUsage, "map: use info, render or export");
}

// uikit --out ui.png [--scale N] [--colors base,light,dark,outline,accent,text] [--square]
int cmdUiKit(const Args& a, Output& out) {
    const auto outPath = a.opt("out");
    if (!outPath) return out.fail(kUsage, "uikit: --out is required");
    UiTheme th;
    th.scale = optInt(a, "scale").value_or(1);
    if (th.scale < 1 || th.scale > 8) return out.fail(kUsage, "uikit: --scale must be 1..8");
    th.rounded = a.flags.count("square") == 0;
    if (auto c = a.opt("colors")) {
        const auto list = splitList(*c);
        Rgba8* slots[] = {&th.base, &th.light, &th.dark, &th.outline, &th.accent, &th.text};
        if (list.size() != 6) return out.fail(kUsage, "uikit: --colors expects 6 colours: base,light,dark,outline,accent,text");
        for (std::size_t i = 0; i < 6; ++i) {
            auto col = Rgba8::fromHex(list[i]);
            if (!col) return out.fail(kUsage, "uikit: bad colour " + list[i]);
            *slots[i] = *col;
        }
    }
    if (auto tex = a.opt("texture")) {
        auto img = readPng(*tex);
        if (!img) return out.fail(img.error());
        th.texture = std::move(img).value();
    }
    auto engines = enginesArg(a);
    if (!engines) return out.fail(engines.error());
    AssetWriteOptions wo;
    wo.engines = engines.value();
    wo.overwrite = a.force;
    wo.pixelsPerUnit = optInt(a, "ppu").value_or(0);
    auto w = writeUiKitFiles(generateUiKit(th), *outPath, wo);
    if (!w) return out.fail(w.error());
    return out.success(filesJson(w.value()), filesHuman(w.value()));
}

// tileset-template --scheme S --tile N --out t.png
int cmdTemplate(const Args& a, Output& out) {
    const auto scheme = schemeArg(a);
    if (!scheme) return out.fail(kUsage, "tileset-template: --scheme must be blob47, wang16 or cardinal16");
    const auto outPath = a.opt("out");
    if (!outPath) return out.fail(kUsage, "tileset-template: --out is required");
    const int tile = optInt(a, "tile").value_or(16);
    if (tile < 4 || tile > 256) return out.fail(kUsage, "tileset-template: --tile must be 4..256");
    std::error_code ec;
    if (fs::exists(*outPath, ec) && !a.force) return out.fail(kIo, *outPath + " already exists (use --force)");
    const Image img = tilesetTemplate(*scheme, tile, {96, 168, 72, 255}, {150, 108, 70, 255});
    if (auto st = writePng(img, *outPath); !st) return out.fail(st.error());
    return out.success({{"path", *outPath}, {"width", img.width()}, {"height", img.height()}},
                       "Wrote " + *outPath + " (" + std::to_string(img.width()) + "x" + std::to_string(img.height()) + ")\n");
}

} // namespace acestudio::cli
