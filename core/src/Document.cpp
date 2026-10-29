#include "acestudio/core/Document.h"

#include <algorithm>
#include <set>

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
    for (auto& [k, c] : m_cels) {
        c.x += dx;
        c.y += dy;
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
    l.name = name.empty() ? (kind == LayerKind::Group ? "Group " : "Layer ") + std::to_string(l.id)
                          : std::move(name);
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

    std::map<CelKey, Cel> shifted;
    for (auto& [k, c] : m_cels)
        shifted.emplace(CelKey{k.layer, k.frame >= at ? k.frame + 1 : k.frame}, std::move(c));
    m_cels = std::move(shifted);
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
    std::vector<std::pair<CelKey, Cel>> copies;
    for (const auto& [k, c] : m_cels)
        if (k.frame == frame) copies.emplace_back(CelKey{k.layer, frame + 1}, c);
    for (auto& [k, c] : copies) m_cels[k] = std::move(c);
    return frame + 1;
}

Status Document::removeFrame(int frame) {
    if (frame < 0 || frame >= frameCount()) return Error{ErrorCode::InvalidArgument, "Frame out of range"};
    if (frameCount() == 1) return Error{ErrorCode::InvalidArgument, "A document must keep at least one frame"};

    std::map<CelKey, Cel> shifted;
    for (auto& [k, c] : m_cels) {
        if (k.frame == frame) continue;
        shifted.emplace(CelKey{k.layer, k.frame > frame ? k.frame - 1 : k.frame}, std::move(c));
    }
    m_cels = std::move(shifted);
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
    std::map<CelKey, Cel> moved;
    for (auto& [k, c] : m_cels) moved.emplace(CelKey{k.layer, remap(k.frame)}, std::move(c));
    m_cels = std::move(moved);
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


void Document::compositeChildren(LayerId parent, int frame, Image& out, int ox, int oy, int depth) const {
    if (depth > kMaxLayers) return;   // defensive; validate() rejects cycles
    for (const Layer& l : m_layers) {
        if (l.parent != parent || !l.visible || l.opacity == 0) continue;
        if (l.kind == LayerKind::Raster) {
            if (const Cel* c = cel(l.id, frame)) blendImage(out, c->image, c->x - ox, c->y - oy, l.blend, l.opacity);
        } else {
            Image group(out.width(), out.height());
            compositeChildren(l.id, frame, group, ox, oy, depth + 1);
            blendImage(out, group, 0, 0, l.blend, l.opacity);
        }
    }
}

Image Document::composite(int frame) const {
    Image out(m_width, m_height);
    if (frame < 0 || frame >= frameCount()) return out;
    compositeChildren(kNoLayer, frame, out, 0, 0, 0);
    return out;
}

void Document::compositeRegion(int frame, const IntRect& region, Image& canvas) const {
    const IntRect r = region.intersected({0, 0, m_width, m_height}).intersected(canvas.bounds());
    if (r.empty()) return;
    Image part(r.w, r.h);
    if (frame >= 0 && frame < frameCount()) compositeChildren(kNoLayer, frame, part, r.x, r.y, 0);
    canvas.blit(part, r.x, r.y);
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
