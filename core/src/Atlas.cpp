#include "acestudio/core/Atlas.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <tuple>
#include <cstdint>
#include <map>
#include <numeric>

namespace acestudio {

namespace {

// MaxRects bin with best-short-side-fit placement.
class MaxRectsBin {
public:
    MaxRectsBin(int w, int h) : m_w(w), m_h(h) { m_free.push_back({0, 0, w, h}); }

    // Places a w x h rect; returns false if it does not fit.
    bool insert(int w, int h, IntRect& out) {
        int bestShort = INT32_MAX, bestLong = INT32_MAX;
        IntRect best{};
        for (const IntRect& f : m_free) {
            if (f.w < w || f.h < h) continue;
            const int lw = f.w - w, lh = f.h - h;
            const int s = std::min(lw, lh), l = std::max(lw, lh);
            // Ties broken by position (top, then left) for determinism.
            if (s < bestShort || (s == bestShort && (l < bestLong || (l == bestLong && (f.y < best.y || (f.y == best.y && f.x < best.x)))))) {
                bestShort = s;
                bestLong = l;
                best = {f.x, f.y, w, h};
            }
        }
        if (bestShort == INT32_MAX) return false;
        place(best);
        out = best;
        return true;
    }

private:
    static bool overlaps(const IntRect& a, const IntRect& b) {
        return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
    }
    static bool contains(const IntRect& outer, const IntRect& in) {
        return in.x >= outer.x && in.y >= outer.y && in.x + in.w <= outer.x + outer.w && in.y + in.h <= outer.y + outer.h;
    }
    void place(const IntRect& r) {
        std::vector<IntRect> next;
        next.reserve(m_free.size() + 4);
        for (const IntRect& f : m_free) {
            if (!overlaps(f, r)) { next.push_back(f); continue; }
            if (r.x > f.x) next.push_back({f.x, f.y, r.x - f.x, f.h});                                  // left
            if (r.x + r.w < f.x + f.w) next.push_back({r.x + r.w, f.y, f.x + f.w - (r.x + r.w), f.h});  // right
            if (r.y > f.y) next.push_back({f.x, f.y, f.w, r.y - f.y});                                  // above
            if (r.y + r.h < f.y + f.h) next.push_back({f.x, r.y + r.h, f.w, f.y + f.h - (r.y + r.h)});  // below
        }
        // Drop free rects contained in others (keep the first of equal ones).
        m_free.clear();
        for (std::size_t i = 0; i < next.size(); ++i) {
            bool redundant = false;
            for (std::size_t j = 0; j < next.size() && !redundant; ++j) {
                if (i == j || !contains(next[j], next[i])) continue;
                redundant = !(next[i] == next[j]) || j < i;
            }
            if (!redundant) m_free.push_back(next[i]);
        }
    }

    int m_w, m_h;
    std::vector<IntRect> m_free;
};

// Placement order: larger side, then area, then input position.
std::vector<std::size_t> packOrder(const std::vector<std::pair<int, int>>& sizes) {
    std::vector<std::size_t> order(sizes.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        const auto [aw, ah] = sizes[a];
        const auto [bw, bh] = sizes[b];
        const int am = std::max(aw, ah), bm = std::max(bw, bh);
        if (am != bm) return am > bm;
        return static_cast<long long>(aw) * ah > static_cast<long long>(bw) * bh;
    });
    return order;
}

// Packs as many of `idx` as fit into one bin; returns their rects (by
// position in `idx`; empty rect = did not fit).
std::vector<IntRect> packInto(const std::vector<std::pair<int, int>>& sizes, const std::vector<std::size_t>& idx,
                              int w, int h, int gap) {
    MaxRectsBin bin(w + gap, h + gap);   // the last row/column does not need its gap
    std::vector<IntRect> out(idx.size());
    for (std::size_t k = 0; k < idx.size(); ++k) {
        const auto [sw, sh] = sizes[idx[k]];
        IntRect r;
        if (bin.insert(sw + gap, sh + gap, r)) out[k] = {r.x, r.y, sw, sh};
    }
    return out;
}

int nextPow2(int v) {
    int p = 1;
    while (p < v) p <<= 1;
    return p;
}

// Smallest bin (by area, then height, then width) that holds all `idx`.
bool smallestBin(const std::vector<std::pair<int, int>>& sizes, const std::vector<std::size_t>& idx, int maxW, int maxH,
                 int gap, bool pot, std::vector<IntRect>& rects, int& binW, int& binH) {
    int widest = 1, tallest = 1;
    long long area = 0;
    for (std::size_t i : idx) {
        widest = std::max(widest, sizes[i].first);
        tallest = std::max(tallest, sizes[i].second);
        area += static_cast<long long>(sizes[i].first + gap) * (sizes[i].second + gap);
    }
    if (widest > maxW || tallest > maxH) return false;
    std::vector<int> widths;
    if (pot) {
        for (int w = nextPow2(widest); w <= maxW; w <<= 1) widths.push_back(w);
    } else {
        const int step = std::max(1, std::min(16, widest));
        for (int w = widest; w < maxW; w += step) widths.push_back(w);
        widths.push_back(maxW);
    }
    // Among the packings, the smallest area wins, but a squarer page within
    // 25% of that area is preferred: long strips are awkward to view and some
    // GPUs limit the longest side.
    struct Candidate {
        std::vector<IntRect> rects;
        int w = 0, h = 0;
        long long area = 0;
    };
    std::vector<Candidate> cands;
    long long bestArea = -1;
    for (int w : widths) {
        if (static_cast<long long>(w) * maxH < area) continue;
        auto r = packInto(sizes, idx, w, maxH, gap);
        bool all = true;
        int usedW = 0, usedH = 0;
        for (const IntRect& q : r) {
            if (q.empty()) { all = false; break; }
            usedW = std::max(usedW, q.x + q.w);
            usedH = std::max(usedH, q.y + q.h);
        }
        if (!all) continue;
        if (pot) { usedW = nextPow2(usedW); usedH = nextPow2(usedH); }
        const long long a = static_cast<long long>(usedW) * usedH;
        if (bestArea < 0 || a < bestArea) bestArea = a;
        cands.push_back({std::move(r), usedW, usedH, a});
    }
    if (bestArea < 0) return false;
    const Candidate* pick = nullptr;
    for (const Candidate& c : cands) {
        if (c.area * 4 > bestArea * 5) continue;
        auto key = [](const Candidate& k) { return std::tuple(std::max(k.w, k.h), k.area, k.h); };
        if (!pick || key(c) < key(*pick)) pick = &c;
    }
    rects = pick->rects;
    binW = pick->w;
    binH = pick->h;
    return true;
}

std::uint64_t hashImage(const Image& img) {
    std::uint64_t h = 1469598103934665603ull;
    auto mix = [&](std::uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    mix(static_cast<std::uint64_t>(img.width()));
    mix(static_cast<std::uint64_t>(img.height()));
    for (const Rgba8 p : img.pixels()) mix((static_cast<std::uint64_t>(p.r) << 24) | (p.g << 16) | (p.b << 8) | p.a);
    return h;
}

} // namespace

std::vector<IntRect> packRects(const std::vector<std::pair<int, int>>& sizes, int maxW, int maxH, int gap) {
    if (sizes.empty()) return {};
    std::vector<std::size_t> idx = packOrder(sizes);
    auto placed = packInto(sizes, idx, maxW, maxH, std::max(0, gap));
    std::vector<IntRect> out(sizes.size());
    for (std::size_t k = 0; k < idx.size(); ++k) {
        if (placed[k].empty()) return {};
        out[idx[k]] = placed[k];
    }
    return out;
}

Result<Atlas> packAtlas(const std::vector<AtlasItem>& items, const AtlasOptions& o) {
    if (items.empty()) return Error{ErrorCode::InvalidArgument, "Nothing to pack"};
    if (o.padding < 0 || o.border < 0 || o.extrude < 0) return Error{ErrorCode::InvalidArgument, "Padding, border and extrusion must not be negative"};
    {
        std::map<std::string, int> names;
        for (const auto& it : items)
            if (++names[it.name] > 1) return Error{ErrorCode::InvalidArgument, "Duplicate sprite name: " + it.name};
    }
    const int innerW = o.maxWidth - 2 * o.border, innerH = o.maxHeight - 2 * o.border;
    if (innerW <= 0 || innerH <= 0) return Error{ErrorCode::InvalidArgument, "The page is smaller than its border"};

    Atlas atlas;
    atlas.sprites.resize(items.size());
    // Trim and find duplicates.
    std::vector<Image> content(items.size());
    std::map<std::uint64_t, std::vector<std::size_t>> byHash;
    std::vector<std::size_t> uniqueIdx;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const Image& img = items[i].image;
        if (img.isNull()) return Error{ErrorCode::InvalidArgument, "Sprite \"" + items[i].name + "\" is empty"};
        AtlasSprite& s = atlas.sprites[i];
        s.name = items[i].name;
        s.sourceWidth = img.width();
        s.sourceHeight = img.height();
        IntRect src = img.bounds();
        if (o.trim) {
            src = img.opaqueBounds();
            if (src.empty()) src = {0, 0, 1, 1};
        }
        s.source = src;
        s.trimmed = !(src == img.bounds());
        content[i] = s.trimmed ? img.copy(src) : img;
        if (o.mergeDuplicates) {
            auto& bucket = byHash[hashImage(content[i])];
            for (std::size_t j : bucket)
                if (content[j] == content[i]) { s.duplicateOf = static_cast<int>(j); break; }
            if (s.duplicateOf < 0) bucket.push_back(i);
        }
        if (s.duplicateOf < 0) uniqueIdx.push_back(i);
    }
    // Cell sizes include the extrusion on every side.
    std::vector<std::pair<int, int>> sizes(items.size());
    for (std::size_t i : uniqueIdx) {
        sizes[i] = {content[i].width() + 2 * o.extrude, content[i].height() + 2 * o.extrude};
        if (sizes[i].first > innerW || sizes[i].second > innerH)
            return Error{ErrorCode::InvalidArgument, "Sprite \"" + items[i].name + "\" (" + std::to_string(content[i].width()) + "x" +
                                                       std::to_string(content[i].height()) + ") does not fit on a " +
                                                       std::to_string(o.maxWidth) + "x" + std::to_string(o.maxHeight) + " page"};
    }
    // Pages: fill full-size pages in placement order, then shrink each page
    // to the smallest bin that still holds its sprites.
    std::vector<std::size_t> remaining;
    {
        std::vector<std::pair<int, int>> uniq;
        for (std::size_t i : uniqueIdx) uniq.push_back(sizes[i]);
        for (std::size_t k : packOrder(uniq)) remaining.push_back(uniqueIdx[k]);
    }
    while (!remaining.empty()) {
        const int page = static_cast<int>(atlas.pages.size());
        // Which sprites fit on a full page?
        const auto full = packInto(sizes, remaining, innerW, innerH, o.padding);
        std::vector<std::size_t> here, later;
        for (std::size_t k = 0; k < remaining.size(); ++k) (full[k].empty() ? later : here).push_back(remaining[k]);
        if (here.empty()) return Error{ErrorCode::Internal, "Atlas packing made no progress"};
        if (!later.empty() && !o.multiplePages)
            return Error{ErrorCode::InvalidArgument, std::to_string(later.size()) + " sprite(s) do not fit on one " + std::to_string(o.maxWidth) +
                                                       "x" + std::to_string(o.maxHeight) + " page"};
        std::vector<IntRect> rects;
        int bw = 0, bh = 0;
        if (!smallestBin(sizes, here, innerW, innerH, o.padding, o.powerOfTwo, rects, bw, bh)) {
            rects = packInto(sizes, here, innerW, innerH, o.padding);
            bw = 0; bh = 0;
            for (const IntRect& q : rects) { bw = std::max(bw, q.x + q.w); bh = std::max(bh, q.y + q.h); }
            if (o.powerOfTwo) { bw = nextPow2(bw); bh = nextPow2(bh); }
        }
        int pw = bw + 2 * o.border, ph = bh + 2 * o.border;
        if (o.powerOfTwo) { pw = nextPow2(pw); ph = nextPow2(ph); }
        auto img = Image::create(std::max(1, pw), std::max(1, ph));
        if (!img) return img.error();
        Image pageImg = std::move(img).value();
        for (std::size_t k = 0; k < here.size(); ++k) {
            const std::size_t i = here[k];
            const Image& c = content[i];
            const int x0 = o.border + rects[k].x + o.extrude, y0 = o.border + rects[k].y + o.extrude;
            pageImg.blit(c, x0, y0);
            // Extrude: repeat the edge pixels outwards.
            for (int e = 1; e <= o.extrude; ++e) {
                for (int x = -e; x < c.width() + e; ++x) {
                    const int sx = std::clamp(x, 0, c.width() - 1);
                    pageImg.setPixel(x0 + x, y0 - e, c.pixel(sx, 0));
                    pageImg.setPixel(x0 + x, y0 + c.height() - 1 + e, c.pixel(sx, c.height() - 1));
                }
                for (int y = -e; y < c.height() + e; ++y) {
                    const int sy = std::clamp(y, 0, c.height() - 1);
                    pageImg.setPixel(x0 - e, y0 + y, c.pixel(0, sy));
                    pageImg.setPixel(x0 + c.width() - 1 + e, y0 + y, c.pixel(c.width() - 1, sy));
                }
            }
            atlas.sprites[i].page = page;
            atlas.sprites[i].rect = {x0, y0, c.width(), c.height()};
        }
        atlas.pages.push_back(std::move(pageImg));
        remaining = std::move(later);
    }
    for (auto& s : atlas.sprites)
        if (s.duplicateOf >= 0) {
            s.page = atlas.sprites[static_cast<std::size_t>(s.duplicateOf)].page;
            s.rect = atlas.sprites[static_cast<std::size_t>(s.duplicateOf)].rect;
        }
    return atlas;
}

std::string atlasJson(const Atlas& atlas, const std::vector<std::string>& pageFiles) {
    using nlohmann::ordered_json;
    // With a filename first (multiatlas) or without (hash keyed by name).
    auto frameObj = [](const AtlasSprite& s, bool withName) {
        ordered_json f = ordered_json::object();
        if (withName) f["filename"] = s.name;
        f["frame"] = {{"x", s.rect.x}, {"y", s.rect.y}, {"w", s.rect.w}, {"h", s.rect.h}};
        f["rotated"] = false;
        f["trimmed"] = s.trimmed;
        f["spriteSourceSize"] = {{"x", s.source.x}, {"y", s.source.y}, {"w", s.source.w}, {"h", s.source.h}};
        f["sourceSize"] = {{"w", s.sourceWidth}, {"h", s.sourceHeight}};
        return f;
    };
    auto file = [&](std::size_t p) { return p < pageFiles.size() ? pageFiles[p] : std::string("atlas_") + std::to_string(p) + ".png"; };
    ordered_json meta = {{"app", "AceStudio"}, {"version", "1.0"}, {"format", "RGBA8888"}, {"scale", "1"}};
    ordered_json root;
    if (atlas.pages.size() <= 1) {
        ordered_json frames = ordered_json::object();
        for (const auto& s : atlas.sprites) frames[s.name] = frameObj(s, false);
        root["frames"] = std::move(frames);
        meta["image"] = file(0);
        meta["size"] = {{"w", atlas.pages.empty() ? 0 : atlas.pages[0].width()}, {"h", atlas.pages.empty() ? 0 : atlas.pages[0].height()}};
        root["meta"] = std::move(meta);
    } else {
        ordered_json textures = ordered_json::array();
        for (std::size_t p = 0; p < atlas.pages.size(); ++p) {
            ordered_json t;
            t["image"] = file(p);
            t["format"] = "RGBA8888";
            t["size"] = {{"w", atlas.pages[p].width()}, {"h", atlas.pages[p].height()}};
            t["scale"] = 1;
            ordered_json frames = ordered_json::array();
            for (const auto& s : atlas.sprites)
                if (s.page == static_cast<int>(p)) frames.push_back(frameObj(s, true));
            t["frames"] = std::move(frames);
            textures.push_back(std::move(t));
        }
        root["textures"] = std::move(textures);
        root["meta"] = std::move(meta);
    }
    return root.dump(2) + "\n";
}

} // namespace acestudio
