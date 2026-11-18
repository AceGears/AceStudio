// acestudio-cli vector: show, add to, clear and rasterise a cel's vector
// shapes (Vector.h).

#include "CliCommon.h"
#include "ProjectArgs.h"

#include "acestudio/core/Editor.h"
#include "acestudio/core/Vector.h"

#include <algorithm>

namespace acestudio::cli {

namespace {

// "a,b,c,d" as exactly `n` numbers.
std::optional<std::vector<double>> numbers(const std::string& s, std::size_t n) {
    const auto parts = splitOn(s, ',');
    if (parts.size() != n) return std::nullopt;
    std::vector<double> v;
    for (const auto& p : parts) {
        const auto d = parseDouble(p);
        if (!d) return std::nullopt;
        v.push_back(*d);
    }
    return v;
}

struct CelJob {
    Document doc;
    LayerId layer = kNoLayer;
    int frame = 0;
};

// The project (positional[2]), --layer and --frame (default 0).
std::optional<CelJob> openCel(const Args& a, Output& out, const std::string& cmd, int& code) {
    if (a.positional.size() != 3) {
        code = out.fail(kUsage, cmd + ": expected one project file");
        return std::nullopt;
    }
    const auto key = a.opt("layer");
    if (!key) {
        code = out.fail(kUsage, cmd + ": --layer is required");
        return std::nullopt;
    }
    auto doc = loadProject(a.positional[2]);
    if (!doc) {
        code = out.fail(doc.error());
        return std::nullopt;
    }
    std::string err;
    const auto layer = findLayer(doc.value(), *key, err);
    if (!layer) {
        code = out.fail(kUsage, cmd + ": " + err);
        return std::nullopt;
    }
    return CelJob{std::move(doc).value(), *layer, optInt(a, "frame").value_or(0)};
}

int vectorShow(const Args& a, Output& out) {
    if (a.positional.size() != 3) return out.fail(kUsage, "vector show: expected one project file");
    auto doc = loadProject(a.positional[2]);
    if (!doc) return out.fail(doc.error());
    json cels = json::array();
    std::string human;
    for (const auto& [k, shapes] : doc.value().vectorCels()) {
        cels.push_back({{"layer", k.layer}, {"layerName", layerName(doc.value(), k.layer)}, {"frame", k.frame}, {"shapes", vectors::toJson(shapes)}});
        human += "  '" + layerName(doc.value(), k.layer) + "' frame " + std::to_string(k.frame) + ": " + std::to_string(shapes.size()) + " shapes\n";
    }
    return out.success({{"cels", cels}}, a.positional[2] + ": " + std::to_string(cels.size()) + " cels with vector shapes\n" + human);
}

// One shape from --rect/--ellipse x0,y0,x1,y1 or --path "x,y[,inx,iny,outx,outy];..."
// [--closed] and the style options.
int vectorAdd(const Args& a, Output& out) {
    int code = kOk;
    auto job = openCel(a, out, "vector add", code);
    if (!job) return code;
    const auto outPath = a.opt("out");
    if (!outPath) return out.fail(kUsage, "vector add: --out is required");
    VectorShape s;
    const auto rect = a.opt("rect"), ellipse = a.opt("ellipse"), path = a.opt("path");
    if ((rect ? 1 : 0) + (ellipse ? 1 : 0) + (path ? 1 : 0) != 1) return out.fail(kUsage, "vector add: give one of --rect, --ellipse or --path");
    if (rect || ellipse) {
        const auto v = numbers(rect ? *rect : *ellipse, 4);
        if (!v) return out.fail(kUsage, "vector add: --rect and --ellipse want x0,y0,x1,y1");
        s.kind = rect ? ShapeKind::Rectangle : ShapeKind::Ellipse;
        s.points = {VectorPoint{(*v)[0], (*v)[1]}, VectorPoint{(*v)[2], (*v)[3]}};
    } else {
        // Anchors are separated by ';' or spaces (CMake and some shells split on ';').
        std::string anchors = *path;
        std::replace(anchors.begin(), anchors.end(), ' ', ';');
        for (const std::string& item : splitOn(anchors, ';')) {
            if (item.empty()) continue;
            const auto two = numbers(item, 2);
            const auto six = two ? std::nullopt : numbers(item, 6);
            if (!two && !six) return out.fail(kUsage, "vector add: --path anchors are x,y or x,y,inX,inY,outX,outY; got '" + item + "'");
            const auto& v = two ? *two : *six;
            VectorPoint p{v[0], v[1]};
            if (six) {
                p.inX = v[2];
                p.inY = v[3];
                p.outX = v[4];
                p.outY = v[5];
            }
            s.points.push_back(p);
        }
        s.closed = a.flags.count("closed") > 0;
    }
    if (auto v = a.opt("stroke")) {
        const auto st = vectors::strokeStyleFromString(*v);
        if (!st) return out.fail(kUsage, "vector add: --stroke wants none, solid, dashed or dotted");
        s.stroke = *st;
    }
    auto color = [&](const char* key, Rgba8& c) -> bool {
        if (auto v = a.opt(key)) {
            const auto parsed = Rgba8::fromHex(*v);
            if (!parsed) return false;
            c = *parsed;
        }
        return true;
    };
    if (!color("stroke-color", s.strokeColor) || !color("fill-color", s.fillColor) || !color("fill-color2", s.fillColor2))
        return out.fail(kUsage, "vector add: colours are #rrggbb or #rrggbbaa");
    if (auto w = a.opt("width")) {
        const auto n = parseInt(*w);
        if (!n) return out.fail(kUsage, "vector add: --width wants a whole number");
        s.width = *n;
    }
    if (auto d = a.opt("dash")) {
        const auto v = numbers(*d, 2);
        if (!v || (*v)[0] != std::floor((*v)[0]) || (*v)[1] != std::floor((*v)[1])) return out.fail(kUsage, "vector add: --dash wants on,off pixels");
        s.dash = static_cast<int>((*v)[0]);
        s.gap = static_cast<int>((*v)[1]);
    }
    if (auto f = a.opt("fill")) {
        const auto st = vectors::fillStyleFromString(*f);
        if (!st) return out.fail(kUsage, "vector add: --fill wants none, solid, linear or radial");
        s.fill = *st;
    }
    if (auto ax = a.opt("axis")) {
        const auto v = numbers(*ax, 4);
        if (!v) return out.fail(kUsage, "vector add: --axis wants x0,y0,x1,y1");
        s.fromX = (*v)[0];
        s.fromY = (*v)[1];
        s.toX = (*v)[2];
        s.toY = (*v)[3];
    }
    s.dither = a.flags.count("smooth") == 0;

    std::vector<VectorShape> shapes;
    if (const auto* now = job->doc.vectorShapes(job->layer, job->frame)) shapes = *now;
    shapes.push_back(s);
    Editor ed(std::move(job->doc));
    if (auto st = ed.setVectorShapes(job->layer, job->frame, shapes, "Add Shape"); !st) return out.fail(st.error());
    if (int c = writeProject(a, out, ed.document(), *outPath); c != kOk) return c;
    return out.success({{"path", *outPath}, {"layer", job->layer}, {"frame", job->frame}, {"shapes", shapes.size()}, {"kind", vectors::toString(s.kind)}},
                       "Added a " + std::string(vectors::toString(s.kind)) + " (" + std::to_string(shapes.size()) + " shapes on the cel) into " +
                           *outPath + "\n");
}

// clear: remove the shapes and their pixels; rasterize: keep the pixels.
int vectorDrop(const Args& a, Output& out, bool keepPixels) {
    const std::string cmd = keepPixels ? "vector rasterize" : "vector clear";
    int code = kOk;
    auto job = openCel(a, out, cmd, code);
    if (!job) return code;
    const auto outPath = a.opt("out");
    if (!outPath) return out.fail(kUsage, cmd + ": --out is required");
    if (!job->doc.vectorShapes(job->layer, job->frame)) return out.fail(kUsage, cmd + ": the cel has no vector shapes");
    Editor ed(std::move(job->doc));
    const Status st = keepPixels ? ed.rasterizeVectorShapes(job->layer, job->frame) : ed.setVectorShapes(job->layer, job->frame, {}, "Clear Shapes");
    if (!st) return out.fail(st.error());
    if (int c = writeProject(a, out, ed.document(), *outPath); c != kOk) return c;
    return out.success({{"path", *outPath}}, (keepPixels ? "Rasterised the shapes into " : "Cleared the shapes in ") + *outPath + "\n");
}

} // namespace

int cmdVector(const Args& a, Output& out) {
    const std::string sub = a.positional.size() > 1 ? a.positional[1] : "";
    if (sub == "show") return vectorShow(a, out);
    if (sub == "add") return vectorAdd(a, out);
    if (sub == "clear") return vectorDrop(a, out, false);
    if (sub == "rasterize") return vectorDrop(a, out, true);
    return out.fail(kUsage, "vector: expected show, add, clear or rasterize");
}

} // namespace acestudio::cli
