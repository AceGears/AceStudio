#include "acestudio/core/AssetTools.h"

#include "acestudio/core/Autotile.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <tuple>

namespace acestudio {

namespace {

int chanDiff(Rgba8 a, Rgba8 b) {
    return std::max({std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b), std::abs(a.a - b.a)});
}

// Foreground mask of a sheet given its background.
std::vector<std::uint8_t> foreground(const Image& img, const std::optional<Rgba8>& bg, int tol) {
    std::vector<std::uint8_t> fg(static_cast<std::size_t>(img.width()) * static_cast<std::size_t>(img.height()));
    std::size_t i = 0;
    for (const Rgba8 p : img.pixels()) fg[i++] = bg ? (chanDiff(p, *bg) > tol ? 1 : 0) : (p.a > 0 ? 1 : 0);
    return fg;
}

bool touches(const IntRect& a, const IntRect& b, int d) {
    return a.x - d < b.x + b.w && b.x < a.x + a.w + d && a.y - d < b.y + b.h && b.y < a.y + a.h + d;
}

IntRect unite(const IntRect& a, const IntRect& b) {
    const int x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
    return {x0, y0, std::max(a.x + a.w, b.x + b.w) - x0, std::max(a.y + a.h, b.y + b.h) - y0};
}

std::string spriteName(std::size_t i) {
    std::string n = std::to_string(i);
    while (n.size() < 3) n.insert(n.begin(), '0');
    return "sprite_" + n;
}

// Reading order: rows (rects overlapping the first rect's vertical band), then x.
void readingOrder(std::vector<IntRect>& rects) {
    std::stable_sort(rects.begin(), rects.end(), [](const IntRect& a, const IntRect& b) { return a.y != b.y ? a.y < b.y : a.x < b.x; });
    std::vector<IntRect> out;
    std::vector<bool> used(rects.size(), false);
    for (std::size_t i = 0; i < rects.size(); ++i) {
        if (used[i]) continue;
        const int bandTop = rects[i].y, bandBottom = rects[i].y + rects[i].h;
        std::vector<IntRect> row;
        for (std::size_t j = i; j < rects.size(); ++j) {
            if (used[j]) continue;
            const int mid = rects[j].y + rects[j].h / 2;
            if (mid >= bandTop && mid < bandBottom) { row.push_back(rects[j]); used[j] = true; }
        }
        std::stable_sort(row.begin(), row.end(), [](const IntRect& a, const IntRect& b) { return a.x < b.x; });
        out.insert(out.end(), row.begin(), row.end());
    }
    rects = std::move(out);
}

} // namespace

std::optional<Rgba8> sliceBackground(const Image& img, const SliceOptions& o) {
    if (o.background) return o.background;
    for (const Rgba8 p : img.pixels())
        if (p.a < 255) return std::nullopt;   // has transparency: that is the background
    std::map<std::uint32_t, int> counts;
    auto add = [&](Rgba8 p) { ++counts[(static_cast<std::uint32_t>(p.r) << 16) | (static_cast<std::uint32_t>(p.g) << 8) | p.b]; };
    for (int x = 0; x < img.width(); ++x) { add(img.pixel(x, 0)); add(img.pixel(x, img.height() - 1)); }
    for (int y = 0; y < img.height(); ++y) { add(img.pixel(0, y)); add(img.pixel(img.width() - 1, y)); }
    std::uint32_t best = 0;
    int bestN = -1;
    for (const auto& [c, n] : counts)
        if (n > bestN) { best = c; bestN = n; }
    return Rgba8{static_cast<std::uint8_t>(best >> 16), static_cast<std::uint8_t>(best >> 8), static_cast<std::uint8_t>(best), 255};
}

std::vector<SlicedSprite> detectSprites(const Image& img, const SliceOptions& o) {
    const int w = img.width(), h = img.height();
    if (w == 0 || h == 0) return {};
    const auto fg = foreground(img, sliceBackground(img, o), o.tolerance);
    std::vector<std::uint8_t> seen(fg.size(), 0);
    std::vector<IntRect> rects;
    std::vector<int> counts;
    std::deque<std::pair<int, int>> q;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const std::size_t i0 = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
            if (!fg[i0] || seen[i0]) continue;
            int x0 = x, y0 = y, x1 = x, y1 = y, n = 0;
            seen[i0] = 1;
            q.emplace_back(x, y);
            while (!q.empty()) {
                const auto [cx, cy] = q.front();
                q.pop_front();
                ++n;
                x0 = std::min(x0, cx); x1 = std::max(x1, cx); y0 = std::min(y0, cy); y1 = std::max(y1, cy);
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = cx + dx, ny = cy + dy;
                        if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                        const std::size_t ni = static_cast<std::size_t>(ny) * static_cast<std::size_t>(w) + static_cast<std::size_t>(nx);
                        if (!fg[ni] || seen[ni]) continue;
                        seen[ni] = 1;
                        q.emplace_back(nx, ny);
                    }
            }
            if (n >= o.minPixels) {
                rects.push_back({x0, y0, x1 - x0 + 1, y1 - y0 + 1});
                counts.push_back(n);
            }
        }
    // A frame or border line around the whole sheet is not a sprite: it would
    // swallow every sprite inside it. Drop thin parts spanning the sheet when
    // there is something else.
    if (rects.size() > 1)
        for (std::size_t i = rects.size(); i-- > 0;) {
            const IntRect& r = rects[i];
            const bool spans = r.w * 100 >= w * 95 && r.h * 100 >= h * 95;
            const bool thin = static_cast<long long>(counts[i]) * 4 < static_cast<long long>(r.w) * r.h;
            if (spans && thin) {
                rects.erase(rects.begin() + static_cast<std::ptrdiff_t>(i));
                counts.erase(counts.begin() + static_cast<std::ptrdiff_t>(i));
            }
        }
    // Merge parts that are close to each other until nothing changes.
    for (bool merged = true; merged;) {
        merged = false;
        for (std::size_t i = 0; i < rects.size() && !merged; ++i)
            for (std::size_t j = i + 1; j < rects.size(); ++j)
                if (touches(rects[i], rects[j], o.mergeDistance)) {
                    rects[i] = unite(rects[i], rects[j]);
                    rects.erase(rects.begin() + static_cast<std::ptrdiff_t>(j));
                    merged = true;
                    break;
                }
    }
    readingOrder(rects);
    std::vector<SlicedSprite> out;
    for (std::size_t i = 0; i < rects.size(); ++i) out.push_back({spriteName(i), rects[i]});
    return out;
}

GridGuess detectGrid(const Image& img, const SliceOptions& o) {
    GridGuess g;
    const auto sprites = detectSprites(img, o);
    if (sprites.size() < 2) return g;
    int maxW = 0, maxH = 0;
    for (const auto& s : sprites) { maxW = std::max(maxW, s.rect.w); maxH = std::max(maxH, s.rect.h); }
    // Smallest cell (with spacing 0..8) that tiles the axis and that no
    // sprite crosses.
    // Prefer the smallest spacing, then the smallest cell giving at least
    // two cells; a single cell spanning the axis is the fallback.
    auto axis = [&](int size, int maxDim, bool horizontal, int& cell, int& spacing, int& count) {
        for (int s = 0; s <= 8; ++s)
            for (int c = std::max(2, maxDim); c <= size; ++c) {
                if ((size + s) % (c + s) != 0) continue;
                const int n = (size + s) / (c + s);
                if (n < 2) continue;
                bool ok = true;
                for (const auto& sp : sprites) {
                    const int a = horizontal ? sp.rect.x : sp.rect.y, len = horizontal ? sp.rect.w : sp.rect.h;
                    const int k = a / (c + s), start = k * (c + s);
                    if (a + len > start + c) { ok = false; break; }    // crosses into the gap or the next cell
                }
                if (ok) { cell = c; spacing = s; count = n; return; }
            }
        cell = size; spacing = 0; count = 1;
    };
    axis(img.width(), maxW, true, g.cellWidth, g.spacingX, g.columns);
    axis(img.height(), maxH, false, g.cellHeight, g.spacingY, g.rows);
    g.found = g.columns * g.rows >= 2;
    return g;
}

std::vector<SlicedSprite> sliceGrid(const Image& img, const GridGuess& g, bool skipEmpty, const SliceOptions& o) {
    std::vector<SlicedSprite> out;
    if (g.cellWidth <= 0 || g.cellHeight <= 0) return out;
    const auto fg = skipEmpty ? foreground(img, sliceBackground(img, o), o.tolerance) : std::vector<std::uint8_t>{};
    for (int r = 0; r < g.rows; ++r)
        for (int c = 0; c < g.columns; ++c) {
            const IntRect cell{g.offsetX + c * (g.cellWidth + g.spacingX), g.offsetY + r * (g.cellHeight + g.spacingY), g.cellWidth, g.cellHeight};
            const IntRect clipped = cell.intersected(img.bounds());
            if (clipped.empty()) continue;
            if (skipEmpty) {
                bool any = false;
                for (int y = clipped.y; y < clipped.y + clipped.h && !any; ++y)
                    for (int x = clipped.x; x < clipped.x + clipped.w && !any; ++x)
                        any = fg[static_cast<std::size_t>(y) * static_cast<std::size_t>(img.width()) + static_cast<std::size_t>(x)] != 0;
                if (!any) continue;
            }
            out.push_back({spriteName(out.size()), clipped});
        }
    return out;
}

std::string slicesJson(const std::vector<SlicedSprite>& slices, const std::string& imageFile, int width, int height) {
    using nlohmann::ordered_json;
    ordered_json frames = ordered_json::object();
    for (const auto& s : slices) {
        ordered_json f = ordered_json::object();
        f["frame"] = {{"x", s.rect.x}, {"y", s.rect.y}, {"w", s.rect.w}, {"h", s.rect.h}};
        f["rotated"] = false;
        f["trimmed"] = false;
        f["spriteSourceSize"] = {{"x", 0}, {"y", 0}, {"w", s.rect.w}, {"h", s.rect.h}};
        f["sourceSize"] = {{"w", s.rect.w}, {"h", s.rect.h}};
        frames[s.name] = std::move(f);
    }
    ordered_json root = ordered_json::object();
    root["frames"] = std::move(frames);
    root["meta"] = {{"app", "AceStudio"}, {"version", "1.0"}, {"image", imageFile}, {"format", "RGBA8888"},
                    {"size", {{"w", width}, {"h", height}}}, {"scale", "1"}};
    return root.dump(2) + "\n";
}

// ---------------------------------------------------------------- nine-slice

NineSlice detectNineSlice(const Image& img) {
    NineSlice n;
    const int w = img.width(), h = img.height();
    if (w < 3 || h < 3) return n;
    auto colEq = [&](int c) { for (int y = 0; y < h; ++y) if (img.pixel(c, y) != img.pixel(c + 1, y)) return false; return true; };
    auto rowEq = [&](int r) { for (int x = 0; x < w; ++x) if (img.pixel(x, r) != img.pixel(x, r + 1)) return false; return true; };
    // Longest run of equal neighbours; ties go to the run nearer the middle.
    auto run = [](int count, auto eq, int size, int& a, int& b) {
        int bestLen = 0, bestA = -1;
        double bestDist = 1e9;
        for (int i = 0; i < count;) {
            if (!eq(i)) { ++i; continue; }
            int j = i;
            while (j + 1 < count && eq(j + 1)) ++j;
            const int len = j - i + 1;
            const double dist = std::abs((i + j + 2) / 2.0 - size / 2.0);
            if (len > bestLen || (len == bestLen && dist < bestDist)) { bestLen = len; bestA = i; bestDist = dist; }
            i = j + 1;
        }
        if (bestA < 0) return false;
        a = bestA;
        b = bestA + bestLen;   // last column of the uniform band
        return true;
    };
    int a, b;
    const bool cx = run(w - 1, colEq, w, a, b);
    if (cx) { n.left = a; n.right = w - 1 - b; } else { n.left = n.right = w / 3; }
    const bool cy = run(h - 1, rowEq, h, a, b);
    if (cy) { n.top = a; n.bottom = h - 1 - b; } else { n.top = n.bottom = h / 3; }
    n.exact = cx && cy;
    return n;
}

Result<Image> renderNineSlice(const Image& img, const NineSlice& n, int w, int h, NineSliceFill fill) {
    const int sw = img.width(), sh = img.height();
    const int midSW = sw - n.left - n.right, midSH = sh - n.top - n.bottom;
    if (midSW < 1 || midSH < 1) return Error{ErrorCode::InvalidArgument, "The nine-slice borders leave no middle to stretch"};
    if (w < n.left + n.right || h < n.top + n.bottom)
        return Error{ErrorCode::InvalidArgument, "Target size " + std::to_string(w) + "x" + std::to_string(h) + " is smaller than the borders"};
    auto out = Image::create(std::max(1, w), std::max(1, h));
    if (!out) return out.error();
    Image o = std::move(out).value();
    const int midDW = w - n.left - n.right, midDH = h - n.top - n.bottom;
    auto map = [&](int d, int lo, int hi, int midS, int midD, int srcSize, int dstSize) {
        if (d < lo) return d;
        if (d >= dstSize - hi) return d - (dstSize - srcSize);
        const int k = d - lo;
        return lo + (fill == NineSliceFill::Tile ? k % midS : (midD > 0 ? k * midS / midD : 0));
    };
    for (int y = 0; y < h; ++y) {
        const int syy = map(y, n.top, n.bottom, midSH, midDH, sh, h);
        for (int x = 0; x < w; ++x) o.setPixel(x, y, img.pixel(map(x, n.left, n.right, midSW, midDW, sw, w), syy));
    }
    return o;
}

// ---------------------------------------------------------------- UI kit

namespace {

Rgba8 mixc(Rgba8 a, Rgba8 b, double t) {
    auto m = [&](std::uint8_t x, std::uint8_t y) { return static_cast<std::uint8_t>(std::lround(x + (y - x) * t)); };
    return {m(a.r, b.r), m(a.g, b.g), m(a.b, b.b), m(a.a, b.a)};
}
Rgba8 grey(Rgba8 c) {
    const auto l = static_cast<std::uint8_t>((c.r * 30 + c.g * 59 + c.b * 11) / 100);
    return {l, l, l, c.a};
}

struct Box {
    Rgba8 fill, topLeft, bottomRight, outline;
};

// A bevelled box: outline, one bevel line, fill (or a tiled texture).
Image drawBox(int w, int h, int s, const Box& b, bool rounded, const Image* texture = nullptr) {
    Image img(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            Rgba8 c = texture && !texture->isNull() ? texture->pixel(x % texture->width(), y % texture->height()) : b.fill;
            const int dx = std::min(x, w - 1 - x), dy = std::min(y, h - 1 - y);
            if (dx < s || dy < s) c = b.outline;
            else if (dx < 2 * s || dy < 2 * s) c = (x < 2 * s || y < 2 * s) && !(x >= w - 2 * s || y >= h - 2 * s) ? b.topLeft : b.bottomRight;
            img.setPixel(x, y, c);
        }
    if (rounded)
        for (int y = 0; y < s; ++y)
            for (int x = 0; x < s; ++x) {
                img.setPixel(x, y, Rgba8::transparent());
                img.setPixel(w - 1 - x, y, Rgba8::transparent());
                img.setPixel(x, h - 1 - y, Rgba8::transparent());
                img.setPixel(w - 1 - x, h - 1 - y, Rgba8::transparent());
            }
    return img;
}

} // namespace

std::vector<UiElement> generateUiKit(const UiTheme& t) {
    const int s = std::clamp(t.scale, 1, 8);
    const int b = 2 * s;          // outline + bevel
    std::vector<UiElement> kit;
    auto add = [&](std::string name, Image img, NineSlice n, bool stretch = true) {
        n.exact = stretch;
        kit.push_back({std::move(name), std::move(img), stretch ? n : NineSlice{}, stretch});
    };
    const NineSlice box{b, b, b, b, true};
    const Box raised{t.base, t.light, t.dark, t.outline};
    const Box inset{t.dark, mixc(t.dark, t.outline, 0.5), t.base, t.outline};
    add("panel", drawBox(12 * s, 12 * s, s, raised, t.rounded, t.texture.isNull() ? nullptr : &t.texture), box, t.texture.isNull());
    if (!t.texture.isNull()) kit.back() = {"panel", kit.back().image, box, true};   // textured centre tiles; borders still exact
    add("panel_inset", drawBox(12 * s, 12 * s, s, inset, t.rounded), box);
    add("button_normal", drawBox(16 * s, 10 * s, s, raised, t.rounded), box);
    add("button_hover", drawBox(16 * s, 10 * s, s, {mixc(t.base, t.light, 0.35), mixc(t.light, t.text, 0.3), t.dark, t.outline}, t.rounded), box);
    add("button_pressed", drawBox(16 * s, 10 * s, s, {t.dark, t.outline, t.base, t.outline}, t.rounded), box);
    add("button_disabled", drawBox(16 * s, 10 * s, s, {grey(t.base), grey(t.light), grey(t.dark), grey(t.outline)}, t.rounded), box);
    // Checkbox and radio: fixed size.
    {
        Image off = drawBox(8 * s, 8 * s, s, inset, false);
        Image on = off;
        for (int i = 0; i < 4 * s; ++i)
            for (int k = 0; k < s; ++k) {
                on.setPixel(2 * s + i / 2, 4 * s + i / 2 - s + k, t.accent);              // short stroke
                on.setPixel(3 * s + i, 5 * s - i + k - s, t.accent);                       // long stroke
            }
        add("checkbox_off", off, {}, false);
        add("checkbox_on", on, {}, false);
        Image roff(8 * s, 8 * s), ron;
        const double r = 4.0 * s, cx = 4.0 * s - 0.5, cy = 4.0 * s - 0.5;
        for (int y = 0; y < 8 * s; ++y)
            for (int x = 0; x < 8 * s; ++x) {
                const double d = std::hypot(x - cx, y - cy);
                if (d <= r - 0.5) roff.setPixel(x, y, d > r - 1.5 * s ? t.outline : t.dark);
            }
        ron = roff;
        for (int y = 0; y < 8 * s; ++y)
            for (int x = 0; x < 8 * s; ++x)
                if (std::hypot(x - cx, y - cy) <= 1.6 * s) ron.setPixel(x, y, t.accent);
        add("radio_off", roff, {}, false);
        add("radio_on", ron, {}, false);
    }
    add("slider_track", drawBox(16 * s, 6 * s, s, inset, t.rounded), {b, b, b, b, true});
    add("slider_knob", drawBox(6 * s, 10 * s, s, raised, t.rounded), {}, false);
    add("progress_back", drawBox(16 * s, 8 * s, s, inset, t.rounded), box);
    add("progress_fill", drawBox(16 * s, 8 * s, s, {t.accent, mixc(t.accent, t.text, 0.4), mixc(t.accent, t.outline, 0.4), t.outline}, t.rounded), box);
    add("tooltip", drawBox(10 * s, 10 * s, s, {t.outline, mixc(t.outline, t.light, 0.25), mixc(t.outline, t.light, 0.25), t.light}, t.rounded), box);
    {
        // Window: a raised frame with a title bar of `title` px at the top.
        const int W = 24 * s, H = 24 * s, title = 7 * s;
        Image win = drawBox(W, H, s, raised, t.rounded);
        for (int y = b; y < title; ++y)
            for (int x = b; x < W - b; ++x) win.setPixel(x, y, y == title - 1 ? t.outline : t.accent);
        add("window", win, {b, title, b, b, true});
        // The rows under the title bar down to the bottom border must repeat.
        for (int y = title; y < title + s; ++y)
            for (int x = b; x < W - b; ++x) win.setPixel(x, y, t.base);
        kit.back().image = win;
    }
    {
        Image div(8 * s, 3 * s);
        for (int x = 0; x < 8 * s; ++x) {
            for (int k = 0; k < s; ++k) {
                div.setPixel(x, k, t.dark);
                div.setPixel(x, s + k, t.outline);
                div.setPixel(x, 2 * s + k, t.light);
            }
        }
        add("divider", div, {0, 0, 0, 0, true});   // stretches along its length; rows are the design
    }
    add("scroll_track", drawBox(6 * s, 16 * s, s, inset, t.rounded), box);
    add("scroll_thumb", drawBox(6 * s, 10 * s, s, raised, t.rounded), box);
    return kit;
}

Result<UiKitFiles> packUiKit(const std::vector<UiElement>& kit, const std::string& imageFile, int padding) {
    std::vector<AtlasItem> items;
    for (const auto& e : kit) items.push_back({e.name, e.image});
    AtlasOptions o;
    o.padding = padding;
    o.mergeDuplicates = false;
    o.multiplePages = false;
    auto at = packAtlas(items, o);
    if (!at) return at.error();
    UiKitFiles f;
    f.atlas = std::move(at).value();
    auto j = nlohmann::ordered_json::parse(atlasJson(f.atlas, {imageFile}));
    for (const auto& e : kit) {
        auto& frame = j["frames"][e.name];
        frame["nineSlice"] = {{"left", e.slice.left}, {"top", e.slice.top}, {"right", e.slice.right}, {"bottom", e.slice.bottom}};
        frame["stretchable"] = e.stretchable;
    }
    f.json = j.dump(2) + "\n";
    return f;
}

// ---------------------------------------------------------------- parallax

namespace {
// Longest run of rows where columns a and b clearly differ. A mean over the
// column hides a step in a silhouette (a 6 px cliff in an 80 px mountain
// layer), so the wrap is also judged by this run length.
int differingRun(const Image& img, int a, int b) {
    int best = 0, run = 0;
    for (int y = 0; y < img.height(); ++y) {
        const Rgba8 p = img.pixel(a, y), q = img.pixel(b, y);
        const int d = std::max({std::abs(p.r - q.r), std::abs(p.g - q.g), std::abs(p.b - q.b), std::abs(p.a - q.a)});
        run = d > 64 ? run + 1 : 0;
        best = std::max(best, run);
    }
    return best;
}
} // namespace

int wrapStepRows(const Image& img, int* typical) {
    if (img.width() < 3) { if (typical) *typical = 0; return 0; }
    std::vector<int> runs;
    for (int x = 0; x + 1 < img.width(); ++x) runs.push_back(differingRun(img, x, x + 1));
    std::sort(runs.begin(), runs.end());
    // The larger of 1.5 x the 95th percentile and the largest change: a
    // step bigger than anything inside the layer stands out.
    const int p95 = runs[std::min(runs.size() - 1, runs.size() * 95 / 100)];
    if (typical) *typical = std::max(3 * p95 / 2, runs.back());
    return differingRun(img, img.width() - 1, 0);
}

namespace {
int maxChannelDiff(Rgba8 a, Rgba8 b) {
    return std::max({std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b), std::abs(a.a - b.a)});
}
} // namespace

// Makes the sky of a layer transparent, starting from the top edge:
//   Flat     pixels connected to the top edge that match the top-left colour
//   Gradient pixels connected to the top edge, each close to the cleared
//            neighbour it was reached from (follows gradients and clouds)
//   Every    every pixel matching the top-left colour, also enclosed gaps
Image clearSky(const Image& src, int tolerance, SkyClear mode) {
    Image img = src;
    if (img.isNull()) return img;
    const Rgba8 key = img.pixel(0, 0);
    if (mode == SkyClear::Every) {
        for (auto& p : img.pixels())
            if (maxChannelDiff(p, key) <= tolerance) p = Rgba8::transparent();
        return img;
    }
    const auto W = static_cast<std::size_t>(img.width());
    std::vector<std::uint8_t> seen(W * static_cast<std::size_t>(img.height()), 0);
    // Each entry: a pixel and the (original) colour it is compared with.
    std::deque<std::tuple<int, int, Rgba8>> q;
    for (int x = 0; x < img.width(); ++x) q.emplace_back(x, 0, key);
    while (!q.empty()) {
        const auto [x, y, ref] = q.front();
        q.pop_front();
        if (!img.contains(x, y)) continue;
        auto& s = seen[static_cast<std::size_t>(y) * W + static_cast<std::size_t>(x)];
        if (s) continue;
        const Rgba8 here = src.pixel(x, y);
        if (maxChannelDiff(here, ref) > tolerance) continue;
        s = 1;
        img.setPixel(x, y, Rgba8::transparent());
        const Rgba8 next = mode == SkyClear::Flat ? key : here;
        q.emplace_back(x + 1, y, next);
        q.emplace_back(x - 1, y, next);
        q.emplace_back(x, y + 1, next);
        q.emplace_back(x, y - 1, next);
    }
    return img;
}

std::vector<std::string> checkParallax(const ParallaxSet& set) {
    std::vector<std::string> out;
    for (const auto& l : set.layers) {
        if (l.image.isNull()) { out.push_back("Layer \"" + l.name + "\" has no image"); continue; }
        if (l.repeatX) {
            const WrapCheck w = checkWrap(l.image);
            int typicalRun = 0;
            const int step = wrapStepRows(l.image, &typicalRun);
            if (w.horizontal && step >= 3 && step > typicalRun)
                out.push_back("Layer \"" + l.name + "\" repeats but its outline jumps " + std::to_string(step) +
                              " px where its left and right edges meet (it changes by up to " + std::to_string(typicalRun) +
                              " px elsewhere); use Make Seamless");
            else if (!w.horizontal)
                out.push_back("Layer \"" + l.name + "\" repeats but its left and right edges do not match (jump " +
                              std::to_string(static_cast<int>(std::lround(w.jumpX * 100))) + "% vs " +
                              std::to_string(static_cast<int>(std::lround(w.typicalX * 100))) + "% inside); use Make Seamless");
        } else if (l.image.width() < set.viewWidth && l.scroll != 0.0) {
            out.push_back("Layer \"" + l.name + "\" is narrower than the view and does not repeat: gaps will show");
        }
        if (l.y >= set.viewHeight || l.y + l.image.height() <= 0)
            out.push_back("Layer \"" + l.name + "\" lies outside the view");
        if (l.scroll < 0 || l.scroll > 4) out.push_back("Layer \"" + l.name + "\" has an unusual scroll factor");
    }
    return out;
}

Image renderParallax(const ParallaxSet& set, double cameraX) {
    Image out(std::max(1, set.viewWidth), std::max(1, set.viewHeight));
    auto over = [](Rgba8 dst, Rgba8 src) {
        if (src.a == 255 || dst.a == 0) return src;
        if (src.a == 0) return dst;
        const double a = src.a / 255.0, b = dst.a / 255.0 * (1 - a), oa = a + b;
        auto ch = [&](std::uint8_t s, std::uint8_t d) { return static_cast<std::uint8_t>(std::lround((s * a + d * b) / oa)); };
        return Rgba8{ch(src.r, dst.r), ch(src.g, dst.g), ch(src.b, dst.b), static_cast<std::uint8_t>(std::lround(oa * 255))};
    };
    for (const auto& l : set.layers) {
        if (l.image.isNull()) continue;
        const int lw = l.image.width();
        const int shift = static_cast<int>(std::floor(cameraX * l.scroll));
        for (int y = 0; y < out.height(); ++y) {
            const int sy = y - l.y;
            if (sy < 0 || sy >= l.image.height()) continue;
            for (int x = 0; x < out.width(); ++x) {
                int sx = x + shift;
                if (l.repeatX) sx = ((sx % lw) + lw) % lw;
                else if (sx < 0 || sx >= lw) continue;
                out.setPixel(x, y, over(out.pixel(x, y), l.image.pixel(sx, sy)));
            }
        }
    }
    return out;
}

Result<Image> makeSeamlessX(const Image& img, int overlap) {
    if (overlap < 1 || overlap * 2 > img.width()) return Error{ErrorCode::InvalidArgument, "The overlap must be between 1 px and half the image width"};
    static const int bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    const int W = img.width() - overlap, H = img.height();
    Image out(W, H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            if (x >= overlap) { out.setPixel(x, y, img.pixel(x, y)); continue; }
            // t = 0 at the left edge (take the image's end, which continues
            // from the right edge) .. 1 at the end of the overlap.
            const double t = (x + 0.5) / overlap;
            const bool fromStart = t * 16.0 > bayer[y & 3][x & 3] + 0.5;
            out.setPixel(x, y, fromStart ? img.pixel(x, y) : img.pixel(W + x, y));
        }
    return out;
}

std::string parallaxJson(const ParallaxSet& set, const std::vector<std::string>& files) {
    using nlohmann::ordered_json;
    ordered_json layers = ordered_json::array();
    for (std::size_t i = 0; i < set.layers.size(); ++i) {
        const auto& l = set.layers[i];
        ordered_json o = ordered_json::object();
        o["name"] = l.name;
        o["file"] = i < files.size() ? files[i] : l.name + ".png";
        o["width"] = l.image.width();
        o["height"] = l.image.height();
        o["scroll"] = l.scroll;
        o["y"] = l.y;
        o["repeat"] = l.repeatX;
        layers.push_back(std::move(o));
    }
    ordered_json root = ordered_json::object();
    root["view"] = {{"w", set.viewWidth}, {"h", set.viewHeight}};
    root["layers"] = std::move(layers);
    return root.dump(2) + "\n";
}

} // namespace acestudio
