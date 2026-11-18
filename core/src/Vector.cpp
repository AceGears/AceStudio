#include "acestudio/core/Vector.h"

#include <algorithm>
#include <cmath>

namespace acestudio::vectors {

namespace {

using nlohmann::json;

Error bad(std::string m) { return Error{ErrorCode::InvalidArgument, std::move(m)}; }
Error corrupt(std::string m) { return Error{ErrorCode::CorruptData, "Vector shapes: " + std::move(m)}; }

bool finite(double v) { return std::isfinite(v) && std::abs(v) <= kMaxCoord; }

Point px(double x, double y) { return {static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y))}; }

void append(std::vector<Point>& path, const std::vector<Point>& seg) {
    for (const Point& p : seg)
        if (path.empty() || !(path.back() == p)) path.push_back(p);
}

IntRect boxOf(const VectorShape& s) {
    return rectFromCorners(px(s.points[0].x, s.points[0].y), px(s.points[1].x, s.points[1].y));
}

// The pixels a shape fills (without its stroke).
Mask fillMask(const VectorShape& s, const std::vector<Point>& line, int w, int h) {
    Mask m(w, h);
    switch (s.kind) {
    case ShapeKind::Rectangle: {
        const IntRect r = boxOf(s);
        for (int y = std::max(0, r.y); y < std::min(h, r.y + r.h); ++y)
            for (int x = std::max(0, r.x); x < std::min(w, r.x + r.w); ++x) m.set(x, y, true);
        break;
    }
    case ShapeKind::Ellipse:
        for (const Point& p : ellipsePoints(boxOf(s), true))
            if (p.x >= 0 && p.y >= 0 && p.x < w && p.y < h) m.set(p.x, p.y, true);
        break;
    case ShapeKind::Path:
        if (line.size() >= 3) m = polygonMask(w, h, line);
        break;
    }
    return m;
}

json xy(double x, double y) { return json::array({x, y}); }

bool readXY(const json& j, const char* key, double& x, double& y) {
    if (!j.contains(key)) return true;
    const json& v = j[key];
    if (!v.is_array() || v.size() != 2 || !v[0].is_number() || !v[1].is_number()) return false;
    x = v[0].get<double>();
    y = v[1].get<double>();
    return true;
}

bool readColor(const json& j, const char* key, Rgba8& c) {
    if (!j.contains(key)) return true;
    if (!j[key].is_string()) return false;
    const auto v = Rgba8::fromHex(j[key].get<std::string>());
    if (!v) return false;
    c = *v;
    return true;
}

bool readInt(const json& j, const char* key, int& v) {
    if (!j.contains(key)) return true;
    if (!j[key].is_number_integer()) return false;
    const auto n = j[key].get<long long>();
    if (n < -1'000'000 || n > 1'000'000) return false;
    v = static_cast<int>(n);
    return true;
}

} // namespace

const char* toString(ShapeKind k) {
    switch (k) {
    case ShapeKind::Path: return "path";
    case ShapeKind::Rectangle: return "rectangle";
    case ShapeKind::Ellipse: return "ellipse";
    }
    return "path";
}
const char* toString(StrokeStyle s) {
    switch (s) {
    case StrokeStyle::None: return "none";
    case StrokeStyle::Solid: return "solid";
    case StrokeStyle::Dashed: return "dashed";
    case StrokeStyle::Dotted: return "dotted";
    }
    return "solid";
}
const char* toString(FillStyle f) {
    switch (f) {
    case FillStyle::None: return "none";
    case FillStyle::Solid: return "solid";
    case FillStyle::Linear: return "linear";
    case FillStyle::Radial: return "radial";
    }
    return "none";
}
std::optional<ShapeKind> shapeKindFromString(const std::string& s) {
    for (ShapeKind k : {ShapeKind::Path, ShapeKind::Rectangle, ShapeKind::Ellipse})
        if (s == toString(k)) return k;
    return std::nullopt;
}
std::optional<StrokeStyle> strokeStyleFromString(const std::string& s) {
    for (StrokeStyle k : {StrokeStyle::None, StrokeStyle::Solid, StrokeStyle::Dashed, StrokeStyle::Dotted})
        if (s == toString(k)) return k;
    return std::nullopt;
}
std::optional<FillStyle> fillStyleFromString(const std::string& s) {
    for (FillStyle k : {FillStyle::None, FillStyle::Solid, FillStyle::Linear, FillStyle::Radial})
        if (s == toString(k)) return k;
    return std::nullopt;
}

Status validate(const std::vector<VectorShape>& shapes) {
    if (shapes.size() > static_cast<std::size_t>(kMaxShapes)) return bad("A cel holds at most " + std::to_string(kMaxShapes) + " shapes");
    for (const VectorShape& s : shapes) {
        if (s.points.size() > static_cast<std::size_t>(kMaxPoints)) return bad("A shape has at most " + std::to_string(kMaxPoints) + " points");
        if (s.kind == ShapeKind::Path ? s.points.size() < 2 : s.points.size() != 2)
            return bad(s.kind == ShapeKind::Path ? "A path needs at least two anchors" : "A rectangle or ellipse needs two corners");
        for (const VectorPoint& p : s.points)
            for (double v : {p.x, p.y, p.inX, p.inY, p.outX, p.outY})
                if (!finite(v)) return bad("A shape's point is out of range");
        for (double v : {s.fromX, s.fromY, s.toX, s.toY})
            if (!finite(v)) return bad("A gradient's end is out of range");
        if (s.width < 1 || s.width > kMaxWidth) return bad("Stroke width must be 1 to " + std::to_string(kMaxWidth));
        if (s.dash < 1 || s.dash > 256 || s.gap < 1 || s.gap > 256) return bad("Dashes and gaps must be 1 to 256 pixels");
    }
    return {};
}

std::vector<Point> outline(const VectorShape& s) {
    std::vector<Point> path;
    if (s.kind == ShapeKind::Path ? s.points.size() < 2 : s.points.size() != 2) return path;
    switch (s.kind) {
    case ShapeKind::Rectangle: {
        // Clockwise from the top-left corner.
        const IntRect r = boxOf(s);
        const int x1 = r.x + r.w - 1, y1 = r.y + r.h - 1;
        for (int x = r.x; x <= x1; ++x) path.push_back({x, r.y});
        for (int y = r.y + 1; y <= y1; ++y) path.push_back({x1, y});
        if (y1 > r.y)
            for (int x = x1 - 1; x >= r.x; --x) path.push_back({x, y1});
        if (x1 > r.x)
            for (int y = y1 - 1; y > r.y; --y) path.push_back({r.x, y});
        break;
    }
    case ShapeKind::Ellipse: {
        // The outline pixels in order of their angle about the centre.
        const IntRect r = boxOf(s);
        path = ellipsePoints(r, false);
        const double cx = r.x + (r.w - 1) / 2.0, cy = r.y + (r.h - 1) / 2.0;
        std::stable_sort(path.begin(), path.end(),
                         [&](Point a, Point b) { return std::atan2(a.y - cy, a.x - cx) < std::atan2(b.y - cy, b.x - cx); });
        break;
    }
    case ShapeKind::Path: {
        const std::size_t n = s.points.size();
        const std::size_t segs = s.closed ? n : n - 1;
        for (std::size_t i = 0; i < segs; ++i) {
            const VectorPoint& a = s.points[i];
            const VectorPoint& b = s.points[(i + 1) % n];
            const Point pa = px(a.x, a.y), pb = px(b.x, b.y);
            const bool straight = a.outX == 0 && a.outY == 0 && b.inX == 0 && b.inY == 0;
            append(path, straight ? linePoints(pa, pb) : curvePoints(pa, pb, px(a.x + a.outX, a.y + a.outY), px(b.x + b.inX, b.y + b.inY)));
        }
        if (s.closed && path.size() > 1 && path.back() == path.front()) path.pop_back();
        break;
    }
    }
    return path;
}

Image rasterize(const std::vector<VectorShape>& shapes, int w, int h) {
    Image out(w, h);
    if (out.isNull()) return out;
    for (const VectorShape& s : shapes) {
        const std::vector<Point> line = outline(s);
        if (line.empty()) continue;
        if (s.fill != FillStyle::None) {
            const Mask m = fillMask(s, line, w, h);
            const bool radial = s.fill == FillStyle::Radial;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    if (!m.get(x, y)) continue;
                    const Rgba8 c = s.fill == FillStyle::Solid
                                        ? s.fillColor
                                        : gradientColor(s.fillColor, s.fillColor2, gradientT(s.fromX, s.fromY, s.toX, s.toY, radial, x, y), s.dither, x, y);
                    out.setPixel(x, y, c);
                }
        }
        if (s.stroke == StrokeStyle::None) continue;
        const int lo = -(s.width - 1) / 2, hi = s.width / 2;
        const int period = s.stroke == StrokeStyle::Dashed ? s.dash + s.gap : 2 * s.width;
        for (std::size_t k = 0; k < line.size(); ++k) {
            const int at = static_cast<int>(k % static_cast<std::size_t>(period));
            if (s.stroke == StrokeStyle::Dashed && at >= s.dash) continue;
            if (s.stroke == StrokeStyle::Dotted && at != 0) continue;
            for (int dy = lo; dy <= hi; ++dy)
                for (int dx = lo; dx <= hi; ++dx) {
                    const int x = line[k].x + dx, y = line[k].y + dy;
                    if (x >= 0 && y >= 0 && x < w && y < h) out.setPixel(x, y, s.strokeColor);
                }
        }
    }
    return out;
}

json toJson(const std::vector<VectorShape>& shapes) {
    json arr = json::array();
    for (const VectorShape& s : shapes) {
        json pts = json::array();
        for (const VectorPoint& p : s.points) {
            json jp = {{"x", p.x}, {"y", p.y}};
            if (p.inX != 0 || p.inY != 0) jp["in"] = xy(p.inX, p.inY);
            if (p.outX != 0 || p.outY != 0) jp["out"] = xy(p.outX, p.outY);
            pts.push_back(jp);
        }
        json js = {{"kind", toString(s.kind)}, {"points", pts}};
        if (s.kind == ShapeKind::Path) js["closed"] = s.closed;
        js["stroke"] = {{"style", toString(s.stroke)}, {"color", s.strokeColor.toHex()}, {"width", s.width}, {"dash", s.dash}, {"gap", s.gap}};
        json jf = {{"style", toString(s.fill)}};
        if (s.fill != FillStyle::None) jf["color"] = s.fillColor.toHex();
        if (s.fill == FillStyle::Linear || s.fill == FillStyle::Radial) {
            jf["color2"] = s.fillColor2.toHex();
            jf["dither"] = s.dither;
            jf["from"] = xy(s.fromX, s.fromY);
            jf["to"] = xy(s.toX, s.toY);
        }
        js["fill"] = jf;
        arr.push_back(js);
    }
    return arr;
}

Result<std::vector<VectorShape>> fromJson(const json& j) {
    if (!j.is_array() || j.size() > static_cast<std::size_t>(kMaxShapes)) return corrupt("not a list of shapes");
    std::vector<VectorShape> out;
    for (const json& js : j) {
        if (!js.is_object() || !js.contains("kind") || !js["kind"].is_string()) return corrupt("a shape has no kind");
        VectorShape s;
        const auto kind = shapeKindFromString(js["kind"].get<std::string>());
        if (!kind) return corrupt("unknown shape '" + js["kind"].get<std::string>() + "'");
        s.kind = *kind;
        if (!js.contains("points") || !js["points"].is_array() || js["points"].size() > static_cast<std::size_t>(kMaxPoints))
            return corrupt("a shape has no point list");
        for (const json& jp : js["points"]) {
            VectorPoint p;
            if (!jp.is_object() || !jp.contains("x") || !jp.contains("y") || !jp["x"].is_number() || !jp["y"].is_number())
                return corrupt("a point has no x and y");
            p.x = jp["x"].get<double>();
            p.y = jp["y"].get<double>();
            if (!readXY(jp, "in", p.inX, p.inY) || !readXY(jp, "out", p.outX, p.outY)) return corrupt("a handle is not two numbers");
            s.points.push_back(p);
        }
        if (js.contains("closed")) {
            if (!js["closed"].is_boolean()) return corrupt("'closed' is not true or false");
            s.closed = js["closed"].get<bool>();
        }
        if (js.contains("stroke")) {
            const json& st = js["stroke"];
            if (!st.is_object()) return corrupt("a stroke is not an object");
            if (st.contains("style")) {
                const auto v = st["style"].is_string() ? strokeStyleFromString(st["style"].get<std::string>()) : std::nullopt;
                if (!v) return corrupt("unknown stroke style");
                s.stroke = *v;
            }
            if (!readColor(st, "color", s.strokeColor) || !readInt(st, "width", s.width) || !readInt(st, "dash", s.dash) || !readInt(st, "gap", s.gap))
                return corrupt("a stroke has a bad colour or size");
        }
        if (js.contains("fill")) {
            const json& f = js["fill"];
            if (!f.is_object()) return corrupt("a fill is not an object");
            if (f.contains("style")) {
                const auto v = f["style"].is_string() ? fillStyleFromString(f["style"].get<std::string>()) : std::nullopt;
                if (!v) return corrupt("unknown fill style");
                s.fill = *v;
            }
            if (!readColor(f, "color", s.fillColor) || !readColor(f, "color2", s.fillColor2) || !readXY(f, "from", s.fromX, s.fromY) ||
                !readXY(f, "to", s.toX, s.toY))
                return corrupt("a fill has a bad colour or axis");
            if (f.contains("dither")) {
                if (!f["dither"].is_boolean()) return corrupt("'dither' is not true or false");
                s.dither = f["dither"].get<bool>();
            }
        }
        out.push_back(std::move(s));
    }
    if (auto st = validate(out); !st) return corrupt(st.error().message);
    return out;
}

} // namespace acestudio::vectors
