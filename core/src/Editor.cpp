#include "acestudio/core/Editor.h"

#include <algorithm>

namespace acestudio {

namespace {

// Generic command built from closures. State the closures need lives in
// captured shared_ptrs so redo can reuse values computed on first apply.
class FnCommand final : public Command {
public:
    using DoFn = std::function<Status(Document&)>;
    using UndoFn = std::function<void(Document&)>;

    FnCommand(std::string label, DoFn doFn, UndoFn undoFn, std::size_t bytes = 0)
        : m_label(std::move(label)), m_do(std::move(doFn)), m_undo(std::move(undoFn)), m_bytes(bytes) {}

    std::string label() const override { return m_label; }
    Status redo(Document& d) override { return m_do(d); }
    void undo(Document& d) override { m_undo(d); }
    std::size_t memoryBytes() const override { return m_bytes; }

private:
    std::string m_label;
    DoFn m_do;
    UndoFn m_undo;
    std::size_t m_bytes;
};

// Replaces a layer's properties (not id/parent/kind). Mergeable per layer+field.
class SetLayerCommand final : public Command {
public:
    SetLayerCommand(std::string label, Layer before, Layer after, int field)
        : m_label(std::move(label)), m_before(std::move(before)), m_after(std::move(after)), m_field(field) {}

    std::string label() const override { return m_label; }
    Status redo(Document& d) override { return assign(d, m_after); }
    void undo(Document& d) override { (void)assign(d, m_before); }
    long long mergeKey() const override {
        return m_field < 0 ? -1 : (static_cast<long long>(m_before.id) << 8) | m_field;
    }
    bool mergeWith(const Command& next) override {
        const auto* o = dynamic_cast<const SetLayerCommand*>(&next);
        if (!o || o->m_before.id != m_before.id || o->m_field != m_field) return false;
        m_after = o->m_after;
        return true;
    }

private:
    static Status assign(Document& d, const Layer& v) {
        Layer* l = d.layer(v.id);
        if (!l) return Error{ErrorCode::NotFound, "Layer no longer exists"};
        l->name = v.name;
        l->visible = v.visible;
        l->locked = v.locked;
        l->alphaLocked = v.alphaLocked;
        l->opacity = v.opacity;
        l->blend = v.blend;
        return {};
    }
    std::string m_label;
    Layer m_before, m_after;
    int m_field;
};

std::size_t celBytes(const Cel& c) { return c.image.byteSize() + sizeof(Cel); }

struct SavedLayer {
    Layer layer;
    std::size_t index = 0;
};

struct SavedCel {
    LayerId layer = kNoLayer;
    int frame = 0;
    Cel cel;
};

// Captures a layer subtree with positions and pixel data for exact restore.
struct LayerSnapshot {
    std::vector<SavedLayer> layers;   // ascending index
    std::vector<SavedCel> cels;
    std::size_t bytes = 0;

    static LayerSnapshot capture(const Document& d, LayerId root) {
        LayerSnapshot s;
        for (LayerId id : d.subtree(root)) {
            s.layers.push_back({*d.layer(id), *d.layerIndex(id)});
            for (const auto& [k, c] : d.cels())
                if (k.layer == id) {
                    s.cels.push_back({k.layer, k.frame, c});
                    s.bytes += celBytes(c);
                }
        }
        std::sort(s.layers.begin(), s.layers.end(), [](auto& a, auto& b) { return a.index < b.index; });
        return s;
    }

    void restore(Document& d) const {
        for (const auto& sl : layers) (void)d.insertLayerAt(sl.layer, sl.index);
        for (const auto& sc : cels) (void)d.setCel(sc.layer, sc.frame, sc.cel);
    }
};

} // namespace

Editor::Editor(Document doc) : m_doc(std::move(doc)) {}

Status Editor::apply(std::unique_ptr<Command> cmd, bool allowMerge) {
    auto st = m_history.push(m_doc, std::move(cmd), allowMerge);
    if (st) notifyChanged();
    return st;
}

void Editor::recordApplied(std::unique_ptr<Command> cmd) {
    m_history.pushApplied(std::move(cmd));
    notifyChanged();
}

Status Editor::undo() {
    auto st = m_history.undo(m_doc);
    if (st) notifyChanged();
    return st;
}

Status Editor::redo() {
    auto st = m_history.redo(m_doc);
    if (st) notifyChanged();
    return st;
}

// ------------------------------------------------------------------ layers

Result<LayerId> Editor::addLayer(std::string name, LayerKind kind, LayerId above) {
    LayerId parent = kNoLayer;
    std::size_t index = m_doc.layers().size();
    if (above != kNoLayer) {
        const Layer* a = m_doc.layer(above);
        if (!a) return Error{ErrorCode::NotFound, "Reference layer not found"};
        parent = a->parent;
        index = *m_doc.layerIndex(above) + 1;
    }
    auto created = std::make_shared<std::optional<Layer>>();
    auto cmd = std::make_unique<FnCommand>(
        kind == LayerKind::Group ? "New Group" : "New Layer",
        [=, name = std::move(name)](Document& d) -> Status {
            if (*created) return d.insertLayerAt(**created, index);
            auto id = d.addLayer(name, kind, parent);
            if (!id) return id.error();
            if (auto st = d.moveLayer(id.value(), index); !st) { (void)d.removeLayer(id.value()); return st; }
            *created = *d.layer(id.value());
            return {};
        },
        [created](Document& d) { (void)d.removeLayer((*created)->id); });
    if (auto st = apply(std::move(cmd)); !st) return st.error();
    return (*created)->id;
}

Status Editor::removeLayer(LayerId id) {
    if (!m_doc.layer(id)) return Error{ErrorCode::NotFound, "No such layer"};
    auto snap = std::make_shared<LayerSnapshot>(LayerSnapshot::capture(m_doc, id));
    const std::size_t bytes = snap->bytes;
    return apply(std::make_unique<FnCommand>(
        "Delete Layer", [id](Document& d) { return d.removeLayer(id); },
        [snap](Document& d) { snap->restore(d); }, bytes));
}

Status Editor::moveLayer(LayerId id, std::size_t newIndex) {
    auto old = m_doc.layerIndex(id);
    if (!old) return Error{ErrorCode::NotFound, "No such layer"};
    if (newIndex >= m_doc.layers().size()) return Error{ErrorCode::InvalidArgument, "Layer index out of range"};
    if (*old == newIndex) return {};
    const std::size_t from = *old;
    return apply(std::make_unique<FnCommand>(
        "Move Layer", [=](Document& d) { return d.moveLayer(id, newIndex); },
        [=](Document& d) { (void)d.moveLayer(id, from); }));
}

namespace {
enum LayerField { kName = 1, kVisible, kLocked, kAlphaLock, kOpacity, kBlend };
}

#define ACE_SET_LAYER(LABEL, FIELD_ENUM, MERGE, ASSIGN)                                         \
    const Layer* cur = m_doc.layer(id);                                                         \
    if (!cur) return Error{ErrorCode::NotFound, "No such layer"};                               \
    Layer after = *cur;                                                                          \
    ASSIGN;                                                                                      \
    return apply(std::make_unique<SetLayerCommand>(LABEL, *cur, after, MERGE ? FIELD_ENUM : -1), MERGE)

Status Editor::renameLayer(LayerId id, std::string name) {
    if (name.empty()) return Error{ErrorCode::InvalidArgument, "Layer name cannot be empty"};
    if (name.size() > 256) return Error{ErrorCode::InvalidArgument, "Layer name is too long"};
    ACE_SET_LAYER("Rename Layer", kName, false, after.name = std::move(name));
}
Status Editor::setLayerVisible(LayerId id, bool v) {
    ACE_SET_LAYER(v ? "Show Layer" : "Hide Layer", kVisible, false, after.visible = v);
}
Status Editor::setLayerLocked(LayerId id, bool v) {
    ACE_SET_LAYER(v ? "Lock Layer" : "Unlock Layer", kLocked, false, after.locked = v);
}
Status Editor::setLayerAlphaLocked(LayerId id, bool v) {
    ACE_SET_LAYER(v ? "Lock Transparency" : "Unlock Transparency", kAlphaLock, false, after.alphaLocked = v);
}
Status Editor::setLayerOpacity(LayerId id, std::uint8_t op, bool mergeable) {
    ACE_SET_LAYER("Layer Opacity", kOpacity, mergeable, after.opacity = op);
}
Status Editor::setLayerBlend(LayerId id, BlendMode mode) {
    ACE_SET_LAYER("Layer Blend Mode", kBlend, false, after.blend = mode);
}
#undef ACE_SET_LAYER

Result<LayerId> Editor::duplicateLayer(LayerId id) {
    const Layer* src = m_doc.layer(id);
    if (!src) return Error{ErrorCode::NotFound, "No such layer"};
    if (src->kind != LayerKind::Raster) return Error{ErrorCode::InvalidArgument, "Only raster layers can be duplicated for now"};
    const std::size_t index = *m_doc.layerIndex(id) + 1;
    auto created = std::make_shared<std::optional<Layer>>();
    std::size_t bytes = 0;
    auto cels = std::make_shared<std::vector<std::pair<int, Cel>>>();
    for (const auto& [k, c] : m_doc.cels())
        if (k.layer == id) { cels->emplace_back(k.frame, c); bytes += celBytes(c); }
    const Layer original = *src;

    auto cmd = std::make_unique<FnCommand>(
        "Duplicate Layer",
        [=](Document& d) -> Status {
            if (!*created) {
                auto nid = d.addLayer(original.name + " copy", LayerKind::Raster, original.parent);
                if (!nid) return nid.error();
                Layer l = original;
                l.id = nid.value();
                l.name = original.name + " copy";
                (void)d.removeLayer(nid.value());
                *created = l;
            }
            if (auto st = d.insertLayerAt(**created, index); !st) return st;
            for (const auto& [f, c] : *cels) (void)d.setCel((*created)->id, f, c);
            return {};
        },
        [created](Document& d) { (void)d.removeLayer((*created)->id); }, bytes);
    if (auto st = apply(std::move(cmd)); !st) return st.error();
    return (*created)->id;
}

Status Editor::mergeDown(LayerId id) {
    const Layer* top = m_doc.layer(id);
    if (!top) return Error{ErrorCode::NotFound, "No such layer"};
    if (top->kind != LayerKind::Raster) return Error{ErrorCode::InvalidArgument, "Only raster layers can be merged"};
    if (!top->visible) return Error{ErrorCode::InvalidArgument, "Show the layer before merging it"};
    // Nearest sibling below.
    const auto& layers = m_doc.layers();
    const std::size_t topIdx = *m_doc.layerIndex(id);
    const Layer* below = nullptr;
    for (std::size_t i = topIdx; i-- > 0;)
        if (layers[i].parent == top->parent) { below = &layers[i]; break; }
    if (!below) return Error{ErrorCode::InvalidArgument, "There is no layer below to merge into"};
    if (below->kind != LayerKind::Raster) return Error{ErrorCode::InvalidArgument, "Cannot merge into a group"};
    if (!m_doc.isLayerEditable(below->id)) return Error{ErrorCode::PermissionDenied, "The layer below is locked"};

    const LayerId belowId = below->id;
    auto topSnap = std::make_shared<LayerSnapshot>(LayerSnapshot::capture(m_doc, id));
    auto belowCels = std::make_shared<std::vector<std::pair<int, std::optional<Cel>>>>();
    std::size_t bytes = topSnap->bytes;
    for (int f = 0; f < m_doc.frameCount(); ++f) {
        const Cel* c = m_doc.cel(belowId, f);
        belowCels->emplace_back(f, c ? std::optional<Cel>(*c) : std::nullopt);
        if (c) bytes += celBytes(*c);
    }

    return apply(std::make_unique<FnCommand>(
        "Merge Down",
        [=](Document& d) -> Status {
            const Layer* t = d.layer(id);
            if (!t) return Error{ErrorCode::NotFound, "Layer no longer exists"};
            for (int f = 0; f < d.frameCount(); ++f) {
                const Cel* tc = d.cel(id, f);
                if (!tc) continue;
                Image base = d.renderLayer(belowId, f);
                blendImage(base, tc->image, tc->x, tc->y, t->blend, t->opacity);
                const IntRect r = base.opaqueBounds();
                if (r.empty()) d.clearCel(belowId, f);
                else (void)d.setCel(belowId, f, Cel{base.copy(r), r.x, r.y});
            }
            return d.removeLayer(id);
        },
        [=](Document& d) {
            topSnap->restore(d);
            for (const auto& [f, c] : *belowCels) {
                if (c) (void)d.setCel(belowId, f, *c);
                else d.clearCel(belowId, f);
            }
        },
        bytes * 2));
}

// ------------------------------------------------------------------ frames

namespace {
std::vector<SavedCel> celsAtFrame(const Document& d, int frame, std::size_t& bytes) {
    std::vector<SavedCel> out;
    for (const auto& [k, c] : d.cels())
        if (k.frame == frame) { out.push_back({k.layer, k.frame, c}); bytes += celBytes(c); }
    return out;
}
} // namespace

Result<int> Editor::insertFrame(int at, int durationMs) {
    auto tags = std::make_shared<std::vector<AnimationTag>>(m_doc.tags());
    auto st = apply(std::make_unique<FnCommand>(
        "New Frame",
        [=](Document& d) -> Status {
            auto r = d.insertFrame(at, durationMs);
            return r ? Status{} : Status{r.error()};
        },
        [=](Document& d) { (void)d.removeFrame(at); (void)d.replaceTags(*tags); }));
    if (!st) return st.error();
    return at;
}

Result<int> Editor::duplicateFrame(int frame) {
    auto tags = std::make_shared<std::vector<AnimationTag>>(m_doc.tags());
    std::size_t bytes = 0;
    (void)celsAtFrame(m_doc, frame, bytes);
    auto st = apply(std::make_unique<FnCommand>(
        "Duplicate Frame",
        [=](Document& d) -> Status {
            auto r = d.duplicateFrame(frame);
            return r ? Status{} : Status{r.error()};
        },
        [=](Document& d) { (void)d.removeFrame(frame + 1); (void)d.replaceTags(*tags); }, bytes));
    if (!st) return st.error();
    return frame + 1;
}

Status Editor::removeFrame(int frame) {
    if (frame < 0 || frame >= m_doc.frameCount()) return Error{ErrorCode::InvalidArgument, "Frame out of range"};
    if (m_doc.frameCount() == 1) return Error{ErrorCode::InvalidArgument, "A document must keep at least one frame"};
    auto tags = std::make_shared<std::vector<AnimationTag>>(m_doc.tags());
    std::size_t bytes = 0;
    auto cels = std::make_shared<std::vector<SavedCel>>(celsAtFrame(m_doc, frame, bytes));
    const int duration = m_doc.frames()[static_cast<std::size_t>(frame)].durationMs;
    return apply(std::make_unique<FnCommand>(
        "Delete Frame", [=](Document& d) { return d.removeFrame(frame); },
        [=](Document& d) {
            (void)d.insertFrame(frame, duration);
            for (const auto& sc : *cels) (void)d.setCel(sc.layer, frame, sc.cel);
            (void)d.replaceTags(*tags);
        },
        bytes));
}

Status Editor::setFrameDuration(int frame, int durationMs) {
    if (frame < 0 || frame >= m_doc.frameCount()) return Error{ErrorCode::InvalidArgument, "Frame out of range"};
    const int old = m_doc.frames()[static_cast<std::size_t>(frame)].durationMs;
    return apply(std::make_unique<FnCommand>(
        "Frame Duration", [=](Document& d) { return d.setFrameDuration(frame, durationMs); },
        [=](Document& d) { (void)d.setFrameDuration(frame, old); }));
}

// ------------------------------------------------------------------ palette

Status Editor::setPaletteColor(std::size_t index, Rgba8 color) {
    if (index >= m_doc.palette().size()) return Error{ErrorCode::InvalidArgument, "Palette index out of range"};
    const Rgba8 old = m_doc.palette().at(index);
    return apply(std::make_unique<FnCommand>(
        "Edit Palette Color", [=](Document& d) { return d.palette().set(index, color); },
        [=](Document& d) { (void)d.palette().set(index, old); }));
}

Status Editor::addPaletteColor(Rgba8 color) {
    if (m_doc.palette().size() >= Palette::kMaxColors) return Error{ErrorCode::InvalidArgument, "Palette is full (256 colors)"};
    const std::size_t index = m_doc.palette().size();
    return apply(std::make_unique<FnCommand>(
        "Add Palette Color", [=](Document& d) { return d.palette().insert(index, color); },
        [=](Document& d) { (void)d.palette().remove(index); }));
}

Status Editor::removePaletteColor(std::size_t index) {
    if (index >= m_doc.palette().size()) return Error{ErrorCode::InvalidArgument, "Palette index out of range"};
    const Rgba8 old = m_doc.palette().at(index);
    return apply(std::make_unique<FnCommand>(
        "Remove Palette Color", [=](Document& d) { return d.palette().remove(index); },
        [=](Document& d) { (void)d.palette().insert(index, old); }));
}

} // namespace acestudio
