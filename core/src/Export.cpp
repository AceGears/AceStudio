#include "acestudio/core/Export.h"
#include "acestudio/core/Animation.h"
#include "acestudio/core/FileUtil.h"
#include "acestudio/core/PngCodec.h"
#include "acestudio/core/Transform.h"
#include "acestudio/core/Version.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace fs = std::filesystem;
using nlohmann::json;

namespace acestudio {

namespace {
const AnimationTag* findTag(const Document& doc, const std::string& name) {
    for (const auto& t : doc.tags())
        if (t.name == name) return &t;
    return nullptr;
}

int nextPow2(int v) {
    int p = 1;
    while (p < v) p <<= 1;
    return p;
}
} // namespace

Result<std::vector<int>> resolveFrames(const Document& doc, const FrameRange& range) {
    int from = range.from, to = range.to < 0 ? doc.frameCount() - 1 : range.to;
    if (!range.tag.empty()) {
        const AnimationTag* t = findTag(doc, range.tag);
        if (!t) return Error{ErrorCode::NotFound, "No tag named '" + range.tag + "'"};
        from = t->fromFrame;
        to = t->toFrame;
    }
    if (from < 0 || to >= doc.frameCount() || from > to)
        return Error{ErrorCode::InvalidArgument, "Frame range is outside the document"};
    std::vector<int> out;
    for (int f = from; f <= to; ++f) out.push_back(f);
    return out;
}

Result<std::vector<AnimFrame>> renderFrames(const Document& doc, const FrameRange& range, int scale, bool playbackOrder) {
    if (scale < 1 || scale > 64) return Error{ErrorCode::InvalidArgument, "Scale must be 1..64"};
    if (static_cast<long long>(doc.width()) * scale > Image::kMaxDimension ||
        static_cast<long long>(doc.height()) * scale > Image::kMaxDimension)
        return Error{ErrorCode::InvalidArgument, "Scaled size exceeds the image size limit"};
    auto frames = resolveFrames(doc, range);
    if (!frames) return frames.error();
    std::vector<int> order = frames.value();
    if (playbackOrder && !range.tag.empty()) order = tagCycle(*findTag(doc, range.tag));

    std::vector<AnimFrame> out;
    for (int f : order) {
        Image img = doc.composite(f);
        if (scale > 1) img = scaleNearest(img, img.width() * scale, img.height() * scale).value();
        out.push_back({std::move(img), doc.frames()[static_cast<std::size_t>(f)].durationMs});
    }
    return out;
}

Result<SpriteSheet> buildSpriteSheet(const Document& doc, const FrameRange& range, const SheetOptions& o) {
    if (o.borderPadding < 0 || o.spacing < 0 || o.innerPadding < 0) return Error{ErrorCode::InvalidArgument, "Padding cannot be negative"};
    auto rendered = renderFrames(doc, range, o.scale, false);
    if (!rendered) return rendered.error();
    auto indices = resolveFrames(doc, range).value();

    SpriteSheet sheet;
    sheet.frameWidth = doc.width() * o.scale;
    sheet.frameHeight = doc.height() * o.scale;

    // Per-frame content (trimmed or full) and duplicate mapping.
    struct Item { Image img; IntRect source; bool trimmed; int dupOf; };
    std::vector<Item> items;
    for (std::size_t i = 0; i < rendered.value().size(); ++i) {
        const Image& full = rendered.value()[i].image;
        IntRect src{0, 0, full.width(), full.height()};
        bool trimmed = false;
        if (o.trim) {
            const IntRect b = full.opaqueBounds();
            src = b.empty() ? IntRect{0, 0, 1, 1} : b;
            trimmed = !(src == IntRect{0, 0, full.width(), full.height()});
        }
        Image content = full.copy(src);
        int dup = -1;
        if (o.mergeDuplicates)
            for (std::size_t j = 0; j < items.size(); ++j)
                if (items[j].dupOf < 0 && items[j].source == src && items[j].img == content) { dup = static_cast<int>(j); break; }
        items.push_back({std::move(content), src, trimmed, dup});
    }

    // Place unique items.
    const int pad = o.innerPadding, gap = o.spacing, border = o.borderPadding;
    std::vector<IntRect> slots(items.size());
    std::vector<std::size_t> unique;
    for (std::size_t i = 0; i < items.size(); ++i)
        if (items[i].dupOf < 0) unique.push_back(i);
    auto cellW = [&](std::size_t i) { return items[i].img.width() + 2 * pad; };
    auto cellH = [&](std::size_t i) { return items[i].img.height() + 2 * pad; };

    int sheetW = 0, sheetH = 0;
    if (o.layout == SheetLayout::Packed) {
        // Shelf packing, tallest first (deterministic: ties by frame order).
        std::vector<std::size_t> order = unique;
        std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return cellH(a) > cellH(b); });
        long long area = 0;
        int widest = 0;
        for (auto i : order) { area += static_cast<long long>(cellW(i) + gap) * (cellH(i) + gap); widest = std::max(widest, cellW(i)); }
        int target = std::max(widest, static_cast<int>(std::ceil(std::sqrt(static_cast<double>(area)))));
        target = std::min(std::max(target, widest), std::max(widest, o.maxWidth - 2 * border));
        int x = 0, y = 0, shelfH = 0;
        for (auto i : order) {
            if (x > 0 && x + cellW(i) > target) { y += shelfH + gap; x = 0; shelfH = 0; }
            slots[i] = {border + x, border + y, cellW(i), cellH(i)};
            x += cellW(i) + gap;
            shelfH = std::max(shelfH, cellH(i));
            sheetW = std::max(sheetW, border + x - gap);
        }
        sheetH = border + y + shelfH;
    } else {
        // Uniform cells sized to the largest (trimmed) frame.
        int cw = 0, ch = 0;
        for (auto i : unique) { cw = std::max(cw, cellW(i)); ch = std::max(ch, cellH(i)); }
        const int n = static_cast<int>(unique.size());
        int cols = n;
        if (o.layout == SheetLayout::Vertical) cols = 1;
        else if (o.layout == SheetLayout::Grid) cols = o.columns > 0 ? o.columns : static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n))));
        cols = std::max(1, std::min(cols, std::max(1, n)));
        const int rows = (n + cols - 1) / cols;
        for (int k = 0; k < n; ++k) {
            const std::size_t i = unique[static_cast<std::size_t>(k)];
            slots[i] = {border + (k % cols) * (cw + gap), border + (k / cols) * (ch + gap), cellW(i), cellH(i)};
        }
        sheetW = border + cols * cw + (cols - 1) * gap;
        sheetH = border + rows * ch + (rows - 1) * gap;
    }
    sheetW += border;
    sheetH += border;
    if (o.powerOfTwo) { sheetW = nextPow2(sheetW); sheetH = nextPow2(sheetH); }
    auto img = Image::create(std::max(1, sheetW), std::max(1, sheetH));
    if (!img) return Error{ErrorCode::InvalidArgument, "Sprite sheet would be too large (" + std::to_string(sheetW) + "×" +
                                                           std::to_string(sheetH) + "); use Grid/Packed or fewer frames"};
    sheet.image = std::move(img).value();

    for (std::size_t i = 0; i < items.size(); ++i) {
        const std::size_t owner = items[i].dupOf >= 0 ? static_cast<std::size_t>(items[i].dupOf) : i;
        const IntRect cell = slots[owner];
        const IntRect r{cell.x + pad, cell.y + pad, items[owner].img.width(), items[owner].img.height()};
        if (items[i].dupOf < 0) sheet.image.blit(items[i].img, r.x, r.y);
        sheet.frames.push_back({indices[i], r, items[i].source, items[i].trimmed, rendered.value()[i].durationMs});
    }
    return sheet;
}

std::string spriteSheetJson(const Document& doc, const SpriteSheet& sheet, const std::string& imageFile,
                            const std::string& baseName, JsonStyle style) {
    json frames = style == JsonStyle::Hash ? json::object() : json::array();
    for (std::size_t i = 0; i < sheet.frames.size(); ++i) {
        const SheetFrame& f = sheet.frames[i];
        // An empty base name gives Aseprite's "{frame}" naming ("0", "1", ...), which Phaser's createFromAseprite needs.
        const std::string name = baseName.empty() ? std::to_string(i) : baseName + " " + std::to_string(i);
        json entry = {{"frame", {{"x", f.rect.x}, {"y", f.rect.y}, {"w", f.rect.w}, {"h", f.rect.h}}},
                      {"rotated", false},
                      {"trimmed", f.trimmed},
                      {"spriteSourceSize", {{"x", f.source.x}, {"y", f.source.y}, {"w", f.source.w}, {"h", f.source.h}}},
                      {"sourceSize", {{"w", sheet.frameWidth}, {"h", sheet.frameHeight}}},
                      {"duration", f.durationMs}};
        if (style == JsonStyle::Hash) frames[name] = entry;
        else { entry["filename"] = name; frames.push_back(entry); }
    }
    // Tags remapped to exported positions.
    json tags = json::array();
    if (!sheet.frames.empty()) {
        const int first = sheet.frames.front().frame, last = sheet.frames.back().frame;
        for (const auto& t : doc.tags()) {
            if (t.fromFrame < first || t.toFrame > last) continue;
            json jt = {{"name", t.name}, {"from", t.fromFrame - first}, {"to", t.toFrame - first},
                       {"direction", std::string(toString(t.direction))}, {"color", t.color}};
            if (t.repeat > 0) jt["repeat"] = std::to_string(t.repeat);
            tags.push_back(jt);
        }
    }
    json layers = json::array();
    for (const auto& l : doc.layers())
        if (l.kind == LayerKind::Raster)
            layers.push_back({{"name", l.name}, {"opacity", l.opacity}, {"blendMode", std::string(toString(l.blend))}});
    const int scale = doc.width() > 0 ? sheet.frameWidth / doc.width() : 1;
    json meta = {{"app", "https://github.com/AceGears/AceStudio"},
                 {"version", kVersionString},
                 {"image", imageFile},
                 {"format", "RGBA8888"},
                 {"size", {{"w", sheet.image.width()}, {"h", sheet.image.height()}}},
                 {"scale", std::to_string(scale)},
                 {"frameTags", tags},
                 {"layers", layers},
                 {"slices", json::array()}};
    return json{{"frames", frames}, {"meta", meta}}.dump(1, ' ');
}

Result<std::vector<fs::path>> pngSequencePaths(const Document& doc, const FrameRange& range, const fs::path& dir,
                                               const std::string& baseName) {
    auto indices = resolveFrames(doc, range);
    if (!indices) return indices.error();
    const int digits = std::max(3, static_cast<int>(std::to_string(doc.frameCount() - 1).size()));
    std::vector<fs::path> paths;
    for (int index : indices.value()) {
        char num[16];
        std::snprintf(num, sizeof num, "%0*d", digits, index);
        paths.push_back(dir / (baseName + "_" + num + ".png"));
    }
    return paths;
}

Result<std::vector<fs::path>> exportPngSequence(const Document& doc, const FrameRange& range, const fs::path& dir,
                                                const std::string& baseName, int scale, bool overwrite) {
    auto frames = renderFrames(doc, range, scale, false);
    if (!frames) return frames.error();
    auto listed = pngSequencePaths(doc, range, dir, baseName);
    if (!listed) return listed.error();
    const std::vector<fs::path>& paths = listed.value();
    std::error_code ec;
    if (!overwrite)
        for (const auto& p : paths)
            if (fs::exists(p, ec)) return Error{ErrorCode::IoError, p.string() + " already exists"};
    for (std::size_t i = 0; i < paths.size(); ++i)
        if (auto st = writePng(frames.value()[i].image, paths[i]); !st) return st.error();
    return paths;
}

Result<SheetFiles> writeSpriteSheet(const Document& doc, const FrameRange& range, const SheetOptions& opts,
                                    const fs::path& imagePath, std::optional<JsonStyle> json, bool overwrite) {
    SheetFiles out;
    out.image = imagePath;
    if (out.image.extension() != ".png") out.image += ".png";
    if (json) {
        out.data = out.image;
        out.data.replace_extension(".json");
    }
    std::error_code ec;
    if (!overwrite)
        for (const auto& p : {out.image, out.data})
            if (!p.empty() && fs::exists(p, ec)) return Error{ErrorCode::IoError, p.string() + " already exists"};
    auto sheet = buildSpriteSheet(doc, range, opts);
    if (!sheet) return sheet.error();
    auto png = encodePng(sheet.value().image);
    if (!png) return png.error();
    if (auto st = writeFileAtomic(out.image, png.value()); !st) return st.error();
    if (json) {
        const std::string meta = spriteSheetJson(doc, sheet.value(), out.image.filename().string(),
                                                 out.image.stem().string(), *json);
        if (auto st = writeFileAtomic(out.data, std::span(reinterpret_cast<const std::uint8_t*>(meta.data()), meta.size())); !st)
            return st.error();
    }
    out.frames = sheet.value().frames.size();
    out.width = sheet.value().image.width();
    out.height = sheet.value().image.height();
    return out;
}

Result<std::size_t> writeAnimation(const Document& doc, const FrameRange& range, AnimFormat format, const fs::path& path,
                                   int scale, int loopCount, bool overwrite) {
    std::error_code ec;
    if (!overwrite && fs::exists(path, ec)) return Error{ErrorCode::IoError, path.string() + " already exists"};
    auto frames = renderFrames(doc, range, scale, true);
    if (!frames) return frames.error();
    auto bytes = format == AnimFormat::Gif    ? encodeGif(frames.value(), loopCount)
               : format == AnimFormat::Apng   ? encodeApng(frames.value(), loopCount)
                                              : encodeWebp(frames.value(), loopCount);
    if (!bytes) return bytes.error();
    if (auto st = writeFileAtomic(path, bytes.value()); !st) return st.error();
    return frames.value().size();
}

} // namespace acestudio
