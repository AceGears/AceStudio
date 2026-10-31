#include "acestudio/core/Document.h"
#include "acestudio/core/Adjust.h"

#include <algorithm>
#include <set>
#include <exception>
#include <system_error>
#include <thread>
#include <vector>

namespace acestudio {

namespace {
constexpr int kMinFrameMs = 1;
constexpr int kMaxFrameMs = 65535;
} // namespace

std::string_view toString(ColorMode m) noexcept {
    switch (m) {
    case ColorMode::Rgba: return "rgba";
    case ColorMode::Indexed: return "indexed";
    case ColorMode::Grayscale: return "grayscale";
    }
    return "rgba";
}

std::optional<ColorMode> colorModeFromString(std::string_view s) noexcept {
    if (s == "rgba") return ColorMode::Rgba;
    if (s == "indexed") return ColorMode::Indexed;
    if (s == "grayscale") return ColorMode::Grayscale;
    return std::nullopt;
}

std::string_view toString(PlaybackDirection d) noexcept {
    switch (d) {
    case PlaybackDirection::Forward: return "forward";
    case PlaybackDirection::Reverse: return "reverse";
    case PlaybackDirection::PingPong: return "pingpong";
    }
    return "forward";
}

std::optional<PlaybackDirection> directionFromString(std::string_view s) noexcept {
    if (s == "forward") return PlaybackDirection::Forward;
    if (s == "reverse") return PlaybackDirection::Reverse;
    if (s == "pingpong") return PlaybackDirection::PingPong;
    return std::nullopt;
}

std::string_view toString(AdjustmentType t) noexcept {
    switch (t) {
    case AdjustmentType::BrightnessContrast: return "brightnessContrast";
    case AdjustmentType::HueSaturation: return "hueSaturation";
    case AdjustmentType::Invert: return "invert";
    case AdjustmentType::Posterize: return "posterize";
    case AdjustmentType::Threshold: return "threshold";
    case AdjustmentType::Levels: return "levels";
    case AdjustmentType::Curves: return "curves";
    }
    return "brightnessContrast";
}

std::optional<AdjustmentType> adjustmentTypeFromString(std::string_view s) noexcept {
    for (auto t : {AdjustmentType::BrightnessContrast, AdjustmentType::HueSaturation, AdjustmentType::Invert,
                   AdjustmentType::Posterize, AdjustmentType::Threshold, AdjustmentType::Levels, AdjustmentType::Curves})
        if (toString(t) == s) return t;
    return std::nullopt;
}

Status validateAdjustment(const Adjustment& a) {
    auto in = [](int v, int lo, int hi) { return v >= lo && v <= hi; };
    bool ok = true;
    switch (a.type) {
    case AdjustmentType::BrightnessContrast: ok = in(a.a, -100, 100) && in(a.b, -100, 100); break;
    case AdjustmentType::HueSaturation: ok = in(a.a, -180, 180) && in(a.b, -100, 100) && in(a.c, -100, 100); break;
    case AdjustmentType::Invert: break;
    case AdjustmentType::Posterize: ok = in(a.a, 2, 64); break;
    case AdjustmentType::Threshold: ok = in(a.a, 0, 255); break;
    case AdjustmentType::Levels: ok = in(a.a, 0, 254) && in(a.b, a.a + 1, 255) && in(a.c, 10, 999); break;
    case AdjustmentType::Curves:
        ok = a.points.size() >= 2 && a.points.size() <= 16;
        for (std::size_t i = 1; ok && i < a.points.size(); ++i) ok = a.points[i].x > a.points[i - 1].x;
        break;
    }
    if (a.type != AdjustmentType::Curves && !a.points.empty()) ok = false;
    return ok ? Status{} : Status{Error{ErrorCode::InvalidArgument, "Adjustment parameters out of range"}};
}

std::string_view toString(TileMode m) noexcept {
    switch (m) {
    case TileMode::None: return "none";
    case TileMode::Horizontal: return "horizontal";
    case TileMode::Vertical: return "vertical";
    case TileMode::Both: return "both";
    }
    return "none";
}

std::optional<TileMode> tileModeFromString(std::string_view s) noexcept {
    for (auto m : {TileMode::None, TileMode::Horizontal, TileMode::Vertical, TileMode::Both})
        if (toString(m) == s) return m;
    return std::nullopt;
}

Result<Document> Document::create(int width, int height, ColorMode mode) {
    if (width <= 0 || height <= 0 || width > Image::kMaxDimension || height > Image::kMaxDimension)
        return Error{ErrorCode::InvalidArgument,
                     "Canvas size must be between 1 and " + std::to_string(Image::kMaxDimension)};
    Document d;
    d.m_width = width;
    d.m_height = height;
    d.m_colorMode = mode;
    d.m_palette = Palette::defaultPalette();
    d.m_frames.push_back(Frame{});
    return d;
}

Status Document::setCanvasSize(int width, int height) {
    if (width <= 0 || height <= 0 || width > Image::kMaxDimension || height > Image::kMaxDimension)
        return Error{ErrorCode::InvalidArgument,
                     "Canvas size must be between 1 and " + std::to_string(Image::kMaxDimension)};
    m_width = width;
    m_height = height;
    return {};
}

void Document::offsetCels(int dx, int dy) noexcept {
    for (auto* map : {&m_cels, &m_maskCels})
        for (auto& [k, c] : *map) {
            c.x += dx;
            c.y += dy;
        }
}

template <typename Fn> void Document::remapFrames(Fn fn) {
    // fn(frame) -> new frame, or -1 to drop the cel. Applied to pixels and masks.
    for (auto* map : {&m_cels, &m_maskCels}) {
        std::map<CelKey, Cel> out;
        for (auto& [k, c] : *map) {
            const int f = fn(k.frame);
            if (f >= 0) out.emplace(CelKey{k.layer, f}, std::move(c));
        }
        *map = std::move(out);
    }
}

Status Document::setPixelAspect(double x, double y) {
    if (!(x > 0.0 && x <= 16.0 && y > 0.0 && y <= 16.0))
        return Error{ErrorCode::InvalidArgument, "Pixel aspect must be in (0, 16]"};
    m_pixelAspectX = x;
    m_pixelAspectY = y;
    return {};
}

// ---------------------------------------------------------------- layers

const Layer* Document::layer(LayerId id) const noexcept {
    auto it = std::find_if(m_layers.begin(), m_layers.end(), [id](const Layer& l) { return l.id == id; });
    return it == m_layers.end() ? nullptr : &*it;
}

Layer* Document::layer(LayerId id) noexcept {
    return const_cast<Layer*>(std::as_const(*this).layer(id));
}

Result<LayerId> Document::addLayer(std::string name, LayerKind kind, LayerId parent) {
    if (m_layers.size() >= static_cast<std::size_t>(kMaxLayers))
        return Error{ErrorCode::InvalidArgument, "Layer limit reached"};
    if (parent != kNoLayer) {
        const Layer* p = layer(parent);
        if (!p || p->kind != LayerKind::Group)
            return Error{ErrorCode::InvalidArgument, "Parent must be an existing group layer"};
    }
    Layer l;
    l.id = m_nextLayerId++;
    l.parent = parent;
    l.kind = kind;
    const char* prefix = kind == LayerKind::Group ? "Group " : kind == LayerKind::Adjustment ? "Adjustment " : "Layer ";
    l.name = name.empty() ? prefix + std::to_string(l.id) : std::move(name);
    m_layers.push_back(std::move(l));
    return m_layers.back().id;
}

Status Document::restoreLayer(const Layer& l) {
    if (l.id == kNoLayer) return Error{ErrorCode::CorruptData, "Layer id 0 is reserved"};
    if (layer(l.id)) return Error{ErrorCode::CorruptData, "Duplicate layer id " + std::to_string(l.id)};
    if (m_layers.size() >= static_cast<std::size_t>(kMaxLayers))
        return Error{ErrorCode::CorruptData, "Too many layers"};
    m_layers.push_back(l);
    m_nextLayerId = std::max(m_nextLayerId, l.id + 1);
    return {};
}

Status Document::removeLayer(LayerId id) {
    if (!layer(id)) return Error{ErrorCode::NotFound, "No such layer"};
    // Collect the layer and all descendants.
    std::set<LayerId> doomed{id};
    bool grew = true;
    while (grew) {
        grew = false;
        for (const auto& l : m_layers)
            if (doomed.count(l.parent) && doomed.insert(l.id).second) grew = true;
    }
    std::erase_if(m_layers, [&](const Layer& l) { return doomed.count(l.id) > 0; });
    std::erase_if(m_cels, [&](const auto& kv) { return doomed.count(kv.first.layer) > 0; });
    std::erase_if(m_maskCels, [&](const auto& kv) { return doomed.count(kv.first.layer) > 0; });
    return {};
}

Status Document::moveLayer(LayerId id, std::size_t newIndex) {
    auto it = std::find_if(m_layers.begin(), m_layers.end(), [id](const Layer& l) { return l.id == id; });
    if (it == m_layers.end()) return Error{ErrorCode::NotFound, "No such layer"};
    if (newIndex >= m_layers.size()) return Error{ErrorCode::InvalidArgument, "Layer index out of range"};
    Layer moved = *it;
    m_layers.erase(it);
    m_layers.insert(m_layers.begin() + static_cast<std::ptrdiff_t>(newIndex), std::move(moved));
    return {};
}

std::optional<std::size_t> Document::layerIndex(LayerId id) const noexcept {
    for (std::size_t i = 0; i < m_layers.size(); ++i)
        if (m_layers[i].id == id) return i;
    return std::nullopt;
}

std::vector<LayerId> Document::subtree(LayerId id) const {
    std::set<LayerId> members{id};
    bool grew = true;
    while (grew) {
        grew = false;
        for (const auto& l : m_layers)
            if (members.count(l.parent) && members.insert(l.id).second) grew = true;
    }
    std::vector<LayerId> out;
    for (const auto& l : m_layers)
        if (members.count(l.id)) out.push_back(l.id);
    return out;
}

Status Document::insertLayerAt(const Layer& l, std::size_t index) {
    if (l.id == kNoLayer || layer(l.id)) return Error{ErrorCode::InvalidArgument, "Layer id is invalid or in use"};
    if (index > m_layers.size()) return Error{ErrorCode::InvalidArgument, "Layer index out of range"};
    if (m_layers.size() >= static_cast<std::size_t>(kMaxLayers)) return Error{ErrorCode::InvalidArgument, "Layer limit reached"};
    if (l.parent != kNoLayer) {
        const Layer* p = layer(l.parent);
        if (!p || p->kind != LayerKind::Group) return Error{ErrorCode::InvalidArgument, "Parent must be a group"};
    }
    m_layers.insert(m_layers.begin() + static_cast<std::ptrdiff_t>(index), l);
    m_nextLayerId = std::max(m_nextLayerId, l.id + 1);
    return {};
}

// ---------------------------------------------------------------- frames

Status Document::setFrameDuration(int frame, int durationMs) {
    if (frame < 0 || frame >= frameCount()) return Error{ErrorCode::InvalidArgument, "Frame out of range"};
    if (durationMs < kMinFrameMs || durationMs > kMaxFrameMs)
        return Error{ErrorCode::InvalidArgument, "Frame duration must be 1..65535 ms"};
    m_frames[static_cast<std::size_t>(frame)].durationMs = durationMs;
    return {};
}

Result<int> Document::insertFrame(int at, int durationMs) {
    if (at < 0 || at > frameCount()) return Error{ErrorCode::InvalidArgument, "Insert position out of range"};
    if (frameCount() >= kMaxFrames) return Error{ErrorCode::InvalidArgument, "Frame limit reached"};
    if (durationMs < kMinFrameMs || durationMs > kMaxFrameMs)
        return Error{ErrorCode::InvalidArgument, "Frame duration must be 1..65535 ms"};

    remapFrames([at](int f) { return f >= at ? f + 1 : f; });
    for (auto& t : m_tags) {
        if (t.fromFrame >= at) ++t.fromFrame;
        if (t.toFrame >= at) ++t.toFrame;
    }
    m_frames.insert(m_frames.begin() + at, Frame{durationMs});
    return at;
}

Result<int> Document::duplicateFrame(int frame) {
    if (frame < 0 || frame >= frameCount()) return Error{ErrorCode::InvalidArgument, "Frame out of range"};
    const int duration = m_frames[static_cast<std::size_t>(frame)].durationMs;
    auto inserted = insertFrame(frame + 1, duration);
    if (!inserted) return inserted.error();
    for (auto* map : {&m_cels, &m_maskCels}) {
        std::vector<std::pair<CelKey, Cel>> copies;
        for (const auto& [k, c] : *map)
            if (k.frame == frame) copies.emplace_back(CelKey{k.layer, frame + 1}, c);
        for (auto& [k, c] : copies) (*map)[k] = std::move(c);
    }
    return frame + 1;
}

Status Document::removeFrame(int frame) {
    if (frame < 0 || frame >= frameCount()) return Error{ErrorCode::InvalidArgument, "Frame out of range"};
    if (frameCount() == 1) return Error{ErrorCode::InvalidArgument, "A document must keep at least one frame"};

    remapFrames([frame](int f) { return f == frame ? -1 : (f > frame ? f - 1 : f); });
    m_frames.erase(m_frames.begin() + frame);

    std::vector<AnimationTag> kept;
    for (auto t : m_tags) {
        if (t.fromFrame == frame && t.toFrame == frame) continue;   // tag covered only this frame
        if (t.fromFrame > frame) --t.fromFrame;
        if (t.toFrame >= frame) --t.toFrame;
        kept.push_back(std::move(t));
    }
    m_tags = std::move(kept);
    return {};
}

Status Document::moveFrame(int from, int to) {
    if (from < 0 || from >= frameCount() || to < 0 || to >= frameCount())
        return Error{ErrorCode::InvalidArgument, "Frame out of range"};
    if (from == to) return {};
    auto remap = [from, to](int f) {
        if (f == from) return to;
        if (from < to && f > from && f <= to) return f - 1;
        if (to < from && f >= to && f < from) return f + 1;
        return f;
    };
    remapFrames(remap);
    const Frame f = m_frames[static_cast<std::size_t>(from)];
    m_frames.erase(m_frames.begin() + from);
    m_frames.insert(m_frames.begin() + to, f);
    return {};
}

// ---------------------------------------------------------------- cels

const Cel* Document::cel(LayerId l, int frame) const noexcept {
    auto it = m_cels.find(CelKey{l, frame});
    return it == m_cels.end() ? nullptr : &it->second;
}

Cel* Document::cel(LayerId l, int frame) noexcept {
    auto it = m_cels.find(CelKey{l, frame});
    return it == m_cels.end() ? nullptr : &it->second;
}

Status Document::setCel(LayerId l, int frame, Cel c) {
    const Layer* lay = layer(l);
    if (!lay) return Error{ErrorCode::NotFound, "No such layer"};
    if (lay->kind != LayerKind::Raster) return Error{ErrorCode::InvalidArgument, "Group layers have no cels"};
    if (frame < 0 || frame >= frameCount()) return Error{ErrorCode::InvalidArgument, "Frame out of range"};
    if (c.image.isNull()) return Error{ErrorCode::InvalidArgument, "Cel image is empty"};
    m_cels[CelKey{l, frame}] = std::move(c);
    return {};
}

void Document::clearCel(LayerId l, int frame) noexcept { m_cels.erase(CelKey{l, frame}); }

const Cel* Document::maskCel(LayerId l, int frame) const noexcept {
    auto it = m_maskCels.find(CelKey{l, frame});
    return it == m_maskCels.end() ? nullptr : &it->second;
}

Cel* Document::maskCel(LayerId l, int frame) noexcept {
    auto it = m_maskCels.find(CelKey{l, frame});
    return it == m_maskCels.end() ? nullptr : &it->second;
}

Status Document::setMaskCel(LayerId l, int frame, Cel c) {
    const Layer* lay = layer(l);
    if (!lay) return Error{ErrorCode::NotFound, "No such layer"};
    if (!lay->hasMask) return Error{ErrorCode::InvalidArgument, "The layer has no mask"};
    if (frame < 0 || frame >= frameCount()) return Error{ErrorCode::InvalidArgument, "Frame out of range"};
    if (c.image.isNull()) return Error{ErrorCode::InvalidArgument, "Mask image is empty"};
    m_maskCels[CelKey{l, frame}] = std::move(c);
    return {};
}

void Document::clearMaskCel(LayerId l, int frame) noexcept { m_maskCels.erase(CelKey{l, frame}); }

std::uint8_t Document::maskValue(LayerId id, int frame, int x, int y) const noexcept {
    const Layer* l = layer(id);
    if (!l || !l->hasMask || !l->maskEnabled) return 255;
    const Cel* c = maskCel(id, frame);
    if (!c || !c->image.contains(x - c->x, y - c->y)) return 255;
    return c->image.pixel(x - c->x, y - c->y).r;
}

Image Document::renderMask(LayerId id, int frame) const {
    Image out(m_width, m_height);
    out.fill(Rgba8{255, 255, 255, 255});
    if (const Cel* c = maskCel(id, frame)) out.blit(c->image, c->x, c->y);
    return out;
}

// ---------------------------------------------------------------- tags

Status Document::addTag(AnimationTag tag) {
    if (tag.name.empty()) return Error{ErrorCode::InvalidArgument, "Tag name is required"};
    if (tag.fromFrame < 0 || tag.toFrame >= frameCount() || tag.fromFrame > tag.toFrame)
        return Error{ErrorCode::InvalidArgument, "Tag frame range is invalid"};
    if (tag.repeat < 0) return Error{ErrorCode::InvalidArgument, "Tag repeat must be >= 0"};
    m_tags.push_back(std::move(tag));
    return {};
}

Status Document::replaceTags(std::vector<AnimationTag> tags) {
    for (const auto& t : tags)
        if (t.name.empty() || t.fromFrame < 0 || t.toFrame >= frameCount() || t.fromFrame > t.toFrame || t.repeat < 0)
            return Error{ErrorCode::InvalidArgument, "Invalid tag '" + t.name + "'"};
    m_tags = std::move(tags);
    return {};
}

Status Document::removeTag(std::size_t index) {
    if (index >= m_tags.size()) return Error{ErrorCode::InvalidArgument, "Tag index out of range"};
    m_tags.erase(m_tags.begin() + static_cast<std::ptrdiff_t>(index));
    return {};
}

// ---------------------------------------------------------------- compositing

bool Document::isLayerEffectivelyVisible(LayerId id) const noexcept {
    const Layer* cur = layer(id);
    for (int depth = 0; cur && depth <= kMaxLayers; ++depth) {
        if (!cur->visible) return false;
        if (cur->parent == kNoLayer) return true;
        cur = layer(cur->parent);
    }
    return false;
}

bool Document::isLayerEditable(LayerId id) const noexcept {
    const Layer* l = layer(id);
    if (!l || l->kind != LayerKind::Raster) return false;
    for (int depth = 0; l && depth <= kMaxLayers; ++depth) {
        if (l->locked) return false;
        if (l->parent == kNoLayer) return true;
        l = layer(l->parent);
    }
    return false;
}


void Document::applyMask(const Layer& l, int frame, Image& img, int ox, int oy) const {
    const Cel* c = maskCel(l.id, frame);
    if (!c) return;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) {
            const int mx = x + ox - c->x, my = y + oy - c->y;
            if (!c->image.contains(mx, my)) continue;
            const unsigned m = c->image.pixel(mx, my).r;
            if (m == 255) continue;
            Rgba8 p = img.pixel(x, y);
            p.a = static_cast<std::uint8_t>((p.a * m + 127) / 255);
            img.setPixel(x, y, p);
        }
}

namespace {
void multiplyAlpha(Image& img, const std::vector<std::uint8_t>& alpha) {
    std::size_t i = 0;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x, ++i) {
            const unsigned a = alpha[i];
            if (a == 255) continue;
            Rgba8 p = img.pixel(x, y);
            p.a = static_cast<std::uint8_t>((p.a * a + 127) / 255);
            img.setPixel(x, y, p);
        }
}

std::vector<std::uint8_t> alphaOf(const Image& img) {
    std::vector<std::uint8_t> a(static_cast<std::size_t>(img.width()) * static_cast<std::size_t>(img.height()));
    std::size_t i = 0;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) a[i++] = img.pixel(x, y).a;
    return a;
}
} // namespace

void Document::compositeChildren(LayerId parent, int frame, Image& out, int ox, int oy, int depth) const {
    if (depth > kMaxLayers) return;   // defensive; validate() rejects cycles
    std::vector<const Layer*> kids;
    for (const Layer& l : m_layers)
        if (l.parent == parent) kids.push_back(&l);
    const std::size_t npx = static_cast<std::size_t>(out.width()) * static_cast<std::size_t>(out.height());

    // Clipping state: alpha of the current base layer (empty = base hidden).
    std::vector<std::uint8_t> baseAlpha;
    bool baseVisible = false;
    bool haveBase = false;   // a clipped layer at the bottom of its group has no base and draws normally
    for (std::size_t i = 0; i < kids.size(); ++i) {
        const Layer& l = *kids[i];
        const bool visible = l.visible && l.opacity > 0;
        const bool nextClipped = i + 1 < kids.size() && kids[i + 1]->clipped;
        const bool clip = l.clipped && haveBase;
        if (clip && !baseVisible) continue;                   // base hidden: clipped layers are too
        if (!clip) { baseVisible = visible; baseAlpha.clear(); haveBase = true; }
        if (!visible) continue;
        const bool masked = l.hasMask && l.maskEnabled && maskCel(l.id, frame);
        const bool clipActive = clip && !baseAlpha.empty();

        if (l.kind == LayerKind::Adjustment) {
            // Blend the adjusted result over `out` by opacity x mask x base alpha.
            Image weight(out.width(), out.height());
            weight.fill(Rgba8{255, 255, 255, 255});
            if (masked) applyMask(l, frame, weight, ox, oy);
            std::vector<std::uint8_t> w = alphaOf(weight);
            if (!clip && nextClipped) baseAlpha = w;          // clipped layers follow the adjustment's mask
            if (clipActive)
                for (std::size_t k = 0; k < npx; ++k) w[k] = static_cast<std::uint8_t>((w[k] * baseAlpha[k] + 127) / 255);
            const PreparedAdjustment prep(l.adjustment);
            std::size_t k = 0;
            for (int y = 0; y < out.height(); ++y)
                for (int x = 0; x < out.width(); ++x, ++k) {
                    const Rgba8 c = out.pixel(x, y);
                    if (c.a == 0) continue;
                    const unsigned t = (static_cast<unsigned>(w[k]) * l.opacity + 127) / 255;
                    if (t == 0) continue;
                    const Rgba8 a = prep.apply(c);
                    auto mix = [t](std::uint8_t p, std::uint8_t q) {
                        return static_cast<std::uint8_t>((p * (255 - t) + q * t + 127) / 255);
                    };
                    out.setPixel(x, y, {mix(c.r, a.r), mix(c.g, a.g), mix(c.b, a.b), c.a});
                }
            continue;
        }

        // Fast path: an unmasked, unclipped raster layer that is not a base.
        if (l.kind == LayerKind::Raster && !masked && !clipActive && !nextClipped) {
            if (const Cel* c = cel(l.id, frame)) blendImage(out, c->image, c->x - ox, c->y - oy, l.blend, l.opacity);
            continue;
        }

        Image content(out.width(), out.height());
        if (l.kind == LayerKind::Raster) {
            if (const Cel* c = cel(l.id, frame)) content.blit(c->image, c->x - ox, c->y - oy);
        } else {
            compositeChildren(l.id, frame, content, ox, oy, depth + 1);
        }
        if (masked) applyMask(l, frame, content, ox, oy);
        if (!clip && nextClipped) baseAlpha = alphaOf(content);
        if (clipActive) multiplyAlpha(content, baseAlpha);
        blendImage(out, content, 0, 0, l.blend, l.opacity);
    }
}

namespace {
// Large areas are split into horizontal bands composited on worker threads.
// The compositor is per-pixel and Document is read-only here, so bands are
// independent and the result is identical to a single-threaded pass.
constexpr long long kParallelMinPixels = 256LL * 256LL;
constexpr int kMinBandRows = 16;

int compositeThreads(const IntRect& r) {
    if (static_cast<long long>(r.w) * r.h < kParallelMinPixels) return 1;
    const unsigned hw = std::thread::hardware_concurrency();
    const int cores = hw == 0 ? 1 : static_cast<int>(std::min(hw, 16u));
    return std::max(1, std::min(cores, r.h / kMinBandRows));
}
} // namespace

Image Document::composite(int frame) const {
    Image out(m_width, m_height);
    if (frame < 0 || frame >= frameCount()) return out;
    compositeRegion(frame, {0, 0, m_width, m_height}, out);
    return out;
}

void Document::compositeRegion(int frame, const IntRect& region, Image& canvas) const {
    const IntRect r = region.intersected({0, 0, m_width, m_height}).intersected(canvas.bounds());
    if (r.empty()) return;
    if (frame < 0 || frame >= frameCount()) return;
    auto band = [&](int y0, int y1) {
        Image part(r.w, y1 - y0);
        compositeChildren(kNoLayer, frame, part, r.x, y0, 0);
        canvas.blit(part, r.x, y0);       // bands cover disjoint rows of `canvas`
    };
    const int n = compositeThreads(r);
    if (n == 1) { band(r.y, r.y + r.h); return; }
    const auto edge = [&](int i) { return r.y + static_cast<int>(static_cast<long long>(r.h) * i / n); };
    std::vector<std::exception_ptr> errors(static_cast<std::size_t>(n));
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(n - 1));
    int started = 1;
    try {
        for (; started < n; ++started)
            workers.emplace_back([&, i = started] {
                try { band(edge(i), edge(i + 1)); } catch (...) { errors[static_cast<std::size_t>(i)] = std::current_exception(); }
            });
    } catch (const std::system_error&) {
        // Could not start another thread: this thread does the remaining bands.
    }
    try {
        band(edge(0), edge(1));
        for (int i = started; i < n; ++i) band(edge(i), edge(i + 1));
    } catch (...) {
        errors[0] = std::current_exception();
    }
    for (auto& t : workers) t.join();
    for (const auto& e : errors)
        if (e) std::rethrow_exception(e);
}

Image Document::renderLayer(LayerId id, int frame) const {
    Image out(m_width, m_height);
    if (const Cel* c = cel(id, frame)) out.blit(c->image, c->x, c->y);
    return out;
}

// ---------------------------------------------------------------- validation

Status Document::validate() const {
    auto corrupt = [](std::string m) { return Status{Error{ErrorCode::CorruptData, std::move(m)}}; };
    if (m_width <= 0 || m_height <= 0 || m_width > Image::kMaxDimension || m_height > Image::kMaxDimension)
        return corrupt("Invalid canvas size");
    if (m_frames.empty() || frameCount() > kMaxFrames) return corrupt("Invalid frame count");
    for (const auto& f : m_frames)
        if (f.durationMs < kMinFrameMs || f.durationMs > kMaxFrameMs) return corrupt("Invalid frame duration");
    if (m_palette.size() > Palette::kMaxColors) return corrupt("Palette exceeds 256 colors");

    std::set<LayerId> ids;
    for (const auto& l : m_layers) {
        if (l.id == kNoLayer || !ids.insert(l.id).second) return corrupt("Invalid or duplicate layer id");
    }
    for (const auto& l : m_layers) {
        if (l.parent == kNoLayer) continue;
        const Layer* p = layer(l.parent);
        if (!p || p->kind != LayerKind::Group) return corrupt("Layer parent is missing or not a group");
        // Cycle check: walking up must terminate.
        const Layer* cur = p;
        int steps = 0;
        while (cur && cur->parent != kNoLayer) {
            if (++steps > kMaxLayers || cur->parent == l.id) return corrupt("Layer hierarchy contains a cycle");
            cur = layer(cur->parent);
        }
    }
    for (const auto& l : m_layers) {
        if (l.kind == LayerKind::Adjustment) {
            if (auto st = validateAdjustment(l.adjustment); !st) return corrupt("Layer '" + l.name + "': " + st.error().message);
        }
        if (l.parent != kNoLayer && layer(l.parent)->kind != LayerKind::Group) return corrupt("Layer parent is not a group");
    }
    for (const auto& [k, c] : m_maskCels) {
        const Layer* l = layer(k.layer);
        if (!l || !l->hasMask) return corrupt("Mask references a missing layer or a layer without a mask");
        if (k.frame < 0 || k.frame >= frameCount()) return corrupt("Mask references a missing frame");
        if (c.image.isNull()) return corrupt("Mask has no image data");
    }
    for (const auto& [k, c] : m_cels) {
        const Layer* l = layer(k.layer);
        if (!l || l->kind != LayerKind::Raster) return corrupt("Cel references a missing or group layer");
        if (k.frame < 0 || k.frame >= frameCount()) return corrupt("Cel references a missing frame");
        if (c.image.isNull()) return corrupt("Cel has no image data");
    }
    for (const auto& t : m_tags)
        if (t.fromFrame < 0 || t.toFrame >= frameCount() || t.fromFrame > t.toFrame || t.repeat < 0)
            return corrupt("Animation tag '" + t.name + "' has an invalid range");
    return {};
}

} // namespace acestudio
