#include "acestudio/core/Editor.h"

#include <climits>

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
        l->clipped = v.clipped;
        l->maskEnabled = v.maskEnabled;
        l->adjustment = v.adjustment;
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
    std::vector<SavedCel> masks;
    std::vector<std::pair<CelKey, std::uint32_t>> links;
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
            for (const auto& [k, c] : d.maskCels())
                if (k.layer == id) {
                    s.masks.push_back({k.layer, k.frame, c});
                    s.bytes += celBytes(c);
                }
            for (const auto& [k, g] : d.links())
                if (k.layer == id) s.links.emplace_back(k, g);
        }
        std::sort(s.layers.begin(), s.layers.end(), [](auto& a, auto& b) { return a.index < b.index; });
        return s;
    }

    void restore(Document& d) const {
        for (const auto& sl : layers) (void)d.insertLayerAt(sl.layer, sl.index);
        for (const auto& sc : cels) (void)d.setCel(sc.layer, sc.frame, sc.cel);
        for (const auto& sc : masks) (void)d.setMaskCel(sc.layer, sc.frame, sc.cel);
        if (!links.empty()) {
            auto all = d.links();
            for (const auto& [k, g] : links) all[k] = g;
            d.restoreLinks(std::move(all));
        }
    }
};

// A copy of `d` holding only `below` and `top` (top clipped to below if it
// is clipped), for baking merges exactly as they composite.
Image compositePair(const Document& d, LayerId belowId, LayerId topId, int frame) {
    Document tmp = Document::create(d.width(), d.height()).value();
    (void)tmp.insertFrame(1);   // placeholder frames up to `frame`
    for (int f = tmp.frameCount(); f <= frame; ++f) (void)tmp.insertFrame(f);
    for (LayerId id : {belowId, topId}) {
        Layer l = *d.layer(id);
        l.parent = kNoLayer;
        if (id == belowId) { l.opacity = 255; l.blend = BlendMode::Normal; l.clipped = false; l.visible = true; }
        (void)tmp.restoreLayer(l);
        if (const Cel* c = d.cel(id, frame)) (void)tmp.setCel(id, frame, *c);
        if (const Cel* m = d.maskCel(id, frame)) (void)tmp.setMaskCel(id, frame, *m);
    }
    return tmp.composite(frame);
}

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
    return addLayerWith(std::move(name), kind, above, {}, kind == LayerKind::Group ? "New Group" : "New Layer");
}

Result<LayerId> Editor::addLayerWith(std::string name, LayerKind kind, LayerId above,
                                     std::function<void(Layer&)> customize, const std::string& label) {
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
        label,
        [=, name = std::move(name)](Document& d) -> Status {
            if (*created) return d.insertLayerAt(**created, index);
            auto id = d.addLayer(name, kind, parent);
            if (!id) return id.error();
            if (auto st = d.moveLayer(id.value(), index); !st) { (void)d.removeLayer(id.value()); return st; }
            if (customize) customize(*d.layer(id.value()));
            *created = *d.layer(id.value());
            return {};
        },
        [created](Document& d) { (void)d.removeLayer((*created)->id); });
    if (auto st = apply(std::move(cmd)); !st) return st.error();
    return (*created)->id;
}

Result<LayerId> Editor::addImageLayer(std::string name, Image image, LayerId above, int frame, int x, int y) {
    if (image.isNull()) return Error{ErrorCode::InvalidArgument, "The image is empty"};
    if (frame < 0 || frame >= m_doc.frameCount()) return Error{ErrorCode::InvalidArgument, "Frame out of range"};
    if (name.size() > 256) return Error{ErrorCode::InvalidArgument, "Layer name is too long"};
    LayerId parent = kNoLayer;
    std::size_t index = m_doc.layers().size();
    if (above != kNoLayer) {
        const Layer* a = m_doc.layer(above);
        if (!a) return Error{ErrorCode::NotFound, "Reference layer not found"};
        parent = a->parent;
        index = *m_doc.layerIndex(above) + 1;
    }
    auto created = std::make_shared<std::optional<Layer>>();
    auto pixels = std::make_shared<const Image>(std::move(image));
    const std::size_t bytes = pixels->byteSize();
    auto cmd = std::make_unique<FnCommand>(
        "Insert " + name,
        [=, name = std::move(name)](Document& d) -> Status {
            LayerId id = kNoLayer;
            if (*created) {
                if (auto st = d.insertLayerAt(**created, index); !st) return st;
                id = (*created)->id;
            } else {
                auto r = d.addLayer(name, LayerKind::Raster, parent);
                if (!r) return r.error();
                id = r.value();
                if (auto st = d.moveLayer(id, index); !st) { (void)d.removeLayer(id); return st; }
                *created = *d.layer(id);
            }
            if (auto st = d.setCel(id, frame, Cel{*pixels, x, y}); !st) { (void)d.removeLayer(id); return st; }
            return {};
        },
        [created](Document& d) { (void)d.removeLayer((*created)->id); }, bytes);
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
    if (m_doc.layers().size() + m_doc.subtree(id).size() > static_cast<std::size_t>(Document::kMaxLayers))
        return Error{ErrorCode::InvalidArgument, "Layer limit reached"};
    // The whole subtree (a group with its children), with cels and masks.
    auto snap = std::make_shared<LayerSnapshot>(LayerSnapshot::capture(m_doc, id));
    const std::size_t index = *m_doc.layerIndex(id) + 1;
    auto copies = std::make_shared<LayerSnapshot>();   // assigned ids, filled on first apply

    auto cmd = std::make_unique<FnCommand>(
        src->kind == LayerKind::Group ? "Duplicate Group" : "Duplicate Layer",
        [=](Document& d) -> Status {
            if (copies->layers.empty()) {
                // New ids: allocate by adding and removing placeholders.
                std::map<LayerId, LayerId> idMap;
                std::vector<LayerId> placeholders;
                for (const auto& sl : snap->layers) {
                    auto nid = d.addLayer("tmp");
                    if (!nid) { for (auto p : placeholders) (void)d.removeLayer(p); return nid.error(); }
                    placeholders.push_back(nid.value());
                    idMap[sl.layer.id] = nid.value();
                }
                for (auto p : placeholders) (void)d.removeLayer(p);
                // Root first, then descendants in stack order.
                std::vector<SavedLayer> order = snap->layers;
                std::stable_partition(order.begin(), order.end(), [id](const SavedLayer& sl) { return sl.layer.id == id; });
                std::size_t at = index;
                for (const auto& sl : order) {
                    Layer l = sl.layer;
                    l.id = idMap[l.id];
                    if (sl.layer.id == id) l.name = sl.layer.name + " copy";
                    else l.parent = idMap[l.parent];
                    copies->layers.push_back({l, at++});
                }
                for (const auto& sc : snap->cels) copies->cels.push_back({idMap[sc.layer], sc.frame, sc.cel});
                for (const auto& sc : snap->masks) copies->masks.push_back({idMap[sc.layer], sc.frame, sc.cel});
                // The copy keeps the original's link structure (groups are per layer).
                for (const auto& [k, g] : snap->links) copies->links.emplace_back(CelKey{idMap[k.layer], k.frame}, g);
            }
            for (const auto& sl : copies->layers)
                if (auto st = d.insertLayerAt(sl.layer, sl.index); !st) return st;
            for (const auto& sc : copies->cels) (void)d.setCel(sc.layer, sc.frame, sc.cel);
            for (const auto& sc : copies->masks) (void)d.setMaskCel(sc.layer, sc.frame, sc.cel);
            if (!copies->links.empty()) {
                auto all = d.links();
                for (const auto& [k, g] : copies->links) all[k] = g;
                d.restoreLinks(std::move(all));
            }
            return {};
        },
        [copies](Document& d) { (void)d.removeLayer(copies->layers.front().layer.id); }, snap->bytes);
    if (auto st = apply(std::move(cmd)); !st) return st.error();
    return copies->layers.front().layer.id;
}

bool Editor::canMergeDown(LayerId id, std::string* why) const {
    auto fail = [&](const char* m) { if (why) *why = m; return false; };
    const Layer* top = m_doc.layer(id);
    if (!top) return fail("No such layer");
    if (top->kind == LayerKind::Group) return fail("Groups cannot be merged down yet; merge their layers first");
    if (!top->visible) return fail("Show the layer before merging it");
    const auto& layers = m_doc.layers();
    const Layer* below = nullptr;
    for (std::size_t i = *m_doc.layerIndex(id); i-- > 0;)
        if (layers[i].parent == top->parent) { below = &layers[i]; break; }
    if (!below) return fail("There is no layer below to merge into");
    if (below->kind != LayerKind::Raster) return fail("Can only merge into a raster layer");
    if (!m_doc.isLayerEditable(below->id)) return fail("The layer below is locked");
    return true;
}

Status Editor::mergeDown(LayerId id) {
    if (std::string why; !canMergeDown(id, &why))
        return Error{m_doc.layer(id) ? (why == "The layer below is locked" ? ErrorCode::PermissionDenied : ErrorCode::InvalidArgument)
                                     : ErrorCode::NotFound, why};
    const Layer* top = m_doc.layer(id);
    const auto& layers = m_doc.layers();
    const Layer* below = nullptr;
    for (std::size_t i = *m_doc.layerIndex(id); i-- > 0;)
        if (layers[i].parent == top->parent) { below = &layers[i]; break; }

    const LayerId belowId = below->id;
    // Masks, clipping and adjustments are baked: the merged pixels are what
    // the two layers composite to; the result has no mask.
    auto topSnap = std::make_shared<LayerSnapshot>(LayerSnapshot::capture(m_doc, id));
    auto belowSnap = std::make_shared<LayerSnapshot>(LayerSnapshot::capture(m_doc, belowId));
    auto merged = std::make_shared<std::vector<std::pair<int, std::optional<Cel>>>>();
    for (int f = 0; f < m_doc.frameCount(); ++f) {
        if (!m_doc.cel(id, f) && !m_doc.maskCel(belowId, f) && top->kind == LayerKind::Raster) {
            const Cel* bc = m_doc.cel(belowId, f);
            merged->emplace_back(f, bc ? std::optional<Cel>(*bc) : std::nullopt);
            continue;
        }
        const Image base = compositePair(m_doc, belowId, id, f);
        const IntRect r = base.opaqueBounds();
        merged->emplace_back(f, r.empty() ? std::nullopt : std::optional<Cel>(Cel{base.copy(r), r.x, r.y}));
    }
    std::size_t bytes = topSnap->bytes + belowSnap->bytes;
    for (const auto& [f, c] : *merged) if (c) bytes += celBytes(*c);

    return apply(std::make_unique<FnCommand>(
        "Merge Down",
        [=](Document& d) -> Status {
            if (!d.layer(id) || !d.layer(belowId)) return Error{ErrorCode::NotFound, "Layer no longer exists"};
            for (int f = 0; f < d.frameCount(); ++f) d.clearMaskCel(belowId, f);
            d.layer(belowId)->hasMask = false;
            // Merged frames can differ even where the layer below was linked:
            // unlink, write, then relink the groups whose results are identical.
            std::map<std::uint32_t, std::vector<int>> groups;
            for (const auto& [k, g] : d.links()) if (k.layer == belowId) groups[g].push_back(k.frame);
            for (const auto& [g, fs] : groups) d.unlinkCels(belowId, fs);
            for (const auto& [f, c] : *merged) {
                if (c) (void)d.setCel(belowId, f, *c);
                else d.clearCel(belowId, f);
            }
            for (const auto& [g, fs] : groups) {
                const Cel* first = d.cel(belowId, fs.front());
                bool same = true;
                for (int f : fs) {
                    const Cel* c = d.cel(belowId, f);
                    same = same && (first == nullptr) == (c == nullptr) &&
                           (!first || (first->x == c->x && first->y == c->y && first->image == c->image));
                }
                if (same) (void)d.linkCels(belowId, fs);
            }
            return d.removeLayer(id);
        },
        [=](Document& d) {
            (void)d.removeLayer(belowId);
            belowSnap->restore(d);
            topSnap->restore(d);
        },
        bytes));
}

// ------------------------------------------------------------------ masks, clipping, adjustments

Status Editor::addLayerMask(LayerId id, const Mask* reveal) {
    const Layer* l = m_doc.layer(id);
    if (!l) return Error{ErrorCode::NotFound, "No such layer"};
    if (l->hasMask) return Error{ErrorCode::InvalidArgument, "The layer already has a mask"};
    std::optional<Image> init;
    if (reveal && !reveal->isNull()) {
        Image m(m_doc.width(), m_doc.height(), Rgba8{0, 0, 0, 255});
        for (int y = 0; y < m.height(); ++y)
            for (int x = 0; x < m.width(); ++x)
                if (reveal->get(x, y)) m.setPixel(x, y, {255, 255, 255, 255});
        init = std::move(m);
    }
    const int frames = m_doc.frameCount();
    return apply(std::make_unique<FnCommand>(
        reveal ? "Add Mask from Selection" : "Add Layer Mask",
        [=](Document& d) -> Status {
            Layer* t = d.layer(id);
            if (!t) return Error{ErrorCode::NotFound, "Layer no longer exists"};
            t->hasMask = true;
            t->maskEnabled = true;
            if (init) for (int f = 0; f < frames && f < d.frameCount(); ++f) (void)d.setMaskCel(id, f, Cel{*init, 0, 0});
            return {};
        },
        [=](Document& d) {
            for (int f = 0; f < d.frameCount(); ++f) d.clearMaskCel(id, f);
            if (Layer* t = d.layer(id)) t->hasMask = false;
        },
        init ? init->byteSize() * static_cast<std::size_t>(frames) : 0));
}

Status Editor::removeLayerMask(LayerId id, bool applyToPixels) {
    const Layer* l = m_doc.layer(id);
    if (!l) return Error{ErrorCode::NotFound, "No such layer"};
    if (!l->hasMask) return Error{ErrorCode::InvalidArgument, "The layer has no mask"};
    if (applyToPixels && l->kind != LayerKind::Raster) return Error{ErrorCode::InvalidArgument, "Only a raster layer's mask can be applied"};
    if (applyToPixels && !m_doc.isLayerEditable(id)) return Error{ErrorCode::PermissionDenied, "The layer is locked"};
    auto snap = std::make_shared<LayerSnapshot>(LayerSnapshot::capture(m_doc, id));
    auto baked = std::make_shared<std::vector<std::pair<int, std::optional<Cel>>>>();
    if (applyToPixels) {
        for (int f = 0; f < m_doc.frameCount(); ++f) {
            const Cel* c = m_doc.cel(id, f);
            if (!c) continue;
            Cel out = *c;
            for (int y = 0; y < out.image.height(); ++y)
                for (int x = 0; x < out.image.width(); ++x) {
                    Rgba8 p = out.image.pixel(x, y);
                    const unsigned v = m_doc.maskCel(id, f) ? [&] {
                        const Cel* mc = m_doc.maskCel(id, f);
                        const int mx = out.x + x - mc->x, my = out.y + y - mc->y;
                        return mc->image.contains(mx, my) ? static_cast<unsigned>(mc->image.pixel(mx, my).r) : 255u;
                    }() : 255u;
                    p.a = static_cast<std::uint8_t>((p.a * v + 127) / 255);
                    out.image.setPixel(x, y, p);
                }
            baked->emplace_back(f, out.image.opaqueBounds().empty() ? std::nullopt : std::optional<Cel>(out));
        }
    }
    return apply(std::make_unique<FnCommand>(
        applyToPixels ? "Apply Layer Mask" : "Delete Layer Mask",
        [=](Document& d) -> Status {
            if (!d.layer(id)) return Error{ErrorCode::NotFound, "Layer no longer exists"};
            for (const auto& [f, c] : *baked) {
                if (c) (void)d.setCel(id, f, *c);
                else d.clearCel(id, f);
            }
            for (int f = 0; f < d.frameCount(); ++f) d.clearMaskCel(id, f);
            d.layer(id)->hasMask = false;
            return {};
        },
        [=](Document& d) {
            Layer* t = d.layer(id);
            if (!t) return;
            *t = snap->layers.front().layer;
            for (const auto& sc : snap->cels) (void)d.setCel(sc.layer, sc.frame, sc.cel);
            for (int f = 0; f < d.frameCount(); ++f) d.clearMaskCel(id, f);
            for (const auto& sc : snap->masks) (void)d.setMaskCel(sc.layer, sc.frame, sc.cel);
        },
        snap->bytes * 2));
}

#define ACE_SET_LAYER2(LABEL, FIELD, MERGE, ASSIGN)                                              \
    const Layer* cur = m_doc.layer(id);                                                         \
    if (!cur) return Error{ErrorCode::NotFound, "No such layer"};                               \
    Layer after = *cur;                                                                          \
    ASSIGN;                                                                                      \
    return apply(std::make_unique<SetLayerCommand>(LABEL, *cur, after, MERGE ? FIELD : -1), MERGE)

Status Editor::setLayerMaskEnabled(LayerId id, bool enabled) {
    if (const Layer* l = m_doc.layer(id); l && !l->hasMask) return Error{ErrorCode::InvalidArgument, "The layer has no mask"};
    ACE_SET_LAYER2(enabled ? "Enable Layer Mask" : "Disable Layer Mask", 20, false, after.maskEnabled = enabled);
}

Status Editor::setLayerClipped(LayerId id, bool clipped) {
    ACE_SET_LAYER2(clipped ? "Create Clipping Mask" : "Release Clipping Mask", 21, false, after.clipped = clipped);
}

Status Editor::setLayerAdjustment(LayerId id, Adjustment adj, bool mergeable) {
    if (const Layer* l = m_doc.layer(id); l && l->kind != LayerKind::Adjustment)
        return Error{ErrorCode::InvalidArgument, "Not an adjustment layer"};
    if (auto st = validateAdjustment(adj); !st) return st;
    ACE_SET_LAYER2("Adjustment Settings", 22, mergeable, after.adjustment = adj);
}
#undef ACE_SET_LAYER2

Result<LayerId> Editor::addAdjustmentLayer(Adjustment adj, LayerId above) {
    if (auto st = validateAdjustment(adj); !st) return st.error();
    static const char* names[] = {"Brightness/Contrast", "Hue/Saturation", "Invert", "Posterize", "Threshold", "Levels", "Curves"};
    static_assert(std::size(names) == static_cast<std::size_t>(AdjustmentType::Curves) + 1, "name every adjustment type");
    const std::string name = names[static_cast<int>(adj.type)];
    return addLayerWith(name, LayerKind::Adjustment, above, [adj](Layer& l) { l.adjustment = adj; },
                        "New Adjustment Layer");
}

Status Editor::setTileMode(TileMode mode) {
    const TileMode old = m_doc.tileMode();
    if (old == mode) return {};
    return apply(std::make_unique<FnCommand>(
        mode == TileMode::None ? "Tiled Mode Off" : "Tiled Mode",
        [=](Document& d) -> Status { d.setTileMode(mode); return {}; },
        [=](Document& d) { d.setTileMode(old); }));
}

Status Editor::setSelection(Mask selection, const std::string& label) {
    if (!selection.isNull() && (selection.width() != m_doc.width() || selection.height() != m_doc.height()))
        return Error{ErrorCode::InvalidArgument, "Selection size does not match the canvas"};
    if (!selection.isNull() && selection.count() == 0) selection = Mask();
    auto before = std::make_shared<Mask>(m_doc.selection());
    auto after = std::make_shared<Mask>(std::move(selection));
    if (before->isNull() && after->isNull()) return {};
    return apply(std::make_unique<FnCommand>(
        label, [after](Document& d) -> Status { d.setSelection(*after); return {}; },
        [before](Document& d) { d.setSelection(*before); },
        static_cast<std::size_t>(before->width()) * static_cast<std::size_t>(before->height()) +
            static_cast<std::size_t>(after->width()) * static_cast<std::size_t>(after->height())));
}

// ------------------------------------------------------------------ frames

namespace {
std::vector<SavedCel> celsAtFrame(const Document& d, int frame, std::size_t& bytes, bool masks = false) {
    std::vector<SavedCel> out;
    for (const auto& [k, c] : masks ? d.maskCels() : d.cels())
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

namespace {
// Silences change notifications while a multi-step operation runs; the
// caller notifies once at the end.
struct MuteListener {
    explicit MuteListener(Editor::ChangeListener& l) : slot(l), saved(std::move(l)) { slot = {}; }
    ~MuteListener() { slot = std::move(saved); }
    Editor::ChangeListener& slot;
    Editor::ChangeListener saved;
};

Status checkRange(const Document& d, int first, int last) {
    if (first < 0 || last >= d.frameCount() || first > last) return Error{ErrorCode::InvalidArgument, "Frame range out of range"};
    return {};
}
} // namespace

// Runs `steps` as one undo step: notifications are muted while it runs, a
// failure rolls back what was done, and listeners hear about it once.
Status Editor::grouped(const std::string& label, const std::function<Status()>& steps) {
    const std::size_t mark = m_history.mark();
    Status st;
    {
        MuteListener mute(m_listener);
        st = steps();
        if (!st) while (m_history.mark() > mark) (void)m_history.undo(m_doc);
        else (void)m_history.squash(mark, label);
    }
    notifyChanged();
    return st;
}

Status Editor::removeFrames(int first, int last) {
    if (auto st = checkRange(m_doc, first, last); !st) return st;
    if (last - first + 1 >= m_doc.frameCount()) return Error{ErrorCode::InvalidArgument, "A document must keep at least one frame"};
    if (first == last) return removeFrame(first);
    return grouped("Delete " + std::to_string(last - first + 1) + " Frames", [&]() -> Status {
        for (int f = last; f >= first; --f)
            if (auto st = removeFrame(f); !st) return st;
        return {};
    });
}

Status Editor::removeFrames(const std::vector<int>& framesIn) {
    std::vector<int> frames = framesIn;
    std::sort(frames.begin(), frames.end());
    frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
    if (frames.empty()) return {};
    if (frames.front() < 0 || frames.back() >= m_doc.frameCount()) return Error{ErrorCode::InvalidArgument, "Frame out of range"};
    if (static_cast<int>(frames.size()) >= m_doc.frameCount())
        return Error{ErrorCode::InvalidArgument, "A document must keep at least one frame"};
    if (frames.size() == 1) return removeFrame(frames.front());
    return grouped("Delete " + std::to_string(frames.size()) + " Frames", [&]() -> Status {
        for (auto it = frames.rbegin(); it != frames.rend(); ++it)   // last first, so indices stay valid
            if (auto st = removeFrame(*it); !st) return st;
        return {};
    });
}

Status Editor::setFrameDurations(const std::vector<int>& frames, int durationMs) {
    if (frames.empty()) return {};
    for (int f : frames)
        if (f < 0 || f >= m_doc.frameCount()) return Error{ErrorCode::InvalidArgument, "Frame out of range"};
    if (frames.size() == 1) return setFrameDuration(frames.front(), durationMs);
    return grouped("Frame Durations", [&]() -> Status {
        for (int f : frames)
            if (auto st = setFrameDuration(f, durationMs); !st) return st;
        return {};
    });
}

Result<int> Editor::duplicateFrames(int first, int last) {
    if (auto st = checkRange(m_doc, first, last); !st) return st.error();
    const int n = last - first + 1;
    if (m_doc.frameCount() + n > Document::kMaxFrames) return Error{ErrorCode::InvalidArgument, "Frame limit reached"};
    if (n == 1) return duplicateFrame(first);
    auto st = grouped("Duplicate " + std::to_string(n) + " Frames", [&]() -> Status {
        for (int i = 0; i < n; ++i) {
            // Copy frame first+i next to itself, then carry the copy past the block.
            if (auto r = duplicateFrame(first + i); !r) return r.error();
            if (auto m = moveFrame(first + i + 1, last + 1 + i); !m) return m;
        }
        return {};
    });
    if (!st) return st.error();
    return last + 1;
}

Status Editor::moveFrames(int first, int last, int to) {
    if (auto st = checkRange(m_doc, first, last); !st) return st;
    const int n = last - first + 1;
    if (to < 0 || to + n > m_doc.frameCount()) return Error{ErrorCode::InvalidArgument, "Frame range out of range"};
    if (to == first) return {};
    if (n == 1) return moveFrame(first, to);
    return grouped("Move " + std::to_string(n) + " Frames", [&]() -> Status {
        if (to < first) {
            for (int i = 0; i < n; ++i)
                if (auto st = moveFrame(first + i, to + i); !st) return st;
        } else {
            for (int i = n - 1; i >= 0; --i)
                if (auto st = moveFrame(first + i, to + i); !st) return st;
        }
        return {};
    });
}

Status Editor::removeFrame(int frame) {
    if (frame < 0 || frame >= m_doc.frameCount()) return Error{ErrorCode::InvalidArgument, "Frame out of range"};
    if (m_doc.frameCount() == 1) return Error{ErrorCode::InvalidArgument, "A document must keep at least one frame"};
    auto tags = std::make_shared<std::vector<AnimationTag>>(m_doc.tags());
    std::size_t bytes = 0;
    auto cels = std::make_shared<std::vector<SavedCel>>(celsAtFrame(m_doc, frame, bytes));
    auto masks = std::make_shared<std::vector<SavedCel>>(celsAtFrame(m_doc, frame, bytes, true));
    const int duration = m_doc.frames()[static_cast<std::size_t>(frame)].durationMs;
    auto links = std::make_shared<std::map<CelKey, std::uint32_t>>(m_doc.links());
    return apply(std::make_unique<FnCommand>(
        "Delete Frame", [=](Document& d) { return d.removeFrame(frame); },
        [=](Document& d) {
            (void)d.insertFrame(frame, duration);
            for (const auto& sc : *cels) (void)d.setCel(sc.layer, frame, sc.cel);
            for (const auto& sc : *masks) (void)d.setMaskCel(sc.layer, frame, sc.cel);
            (void)d.replaceTags(*tags);
            d.restoreLinks(*links);   // frame keys are back where they were
        },
        bytes));
}

Status Editor::setCelImage(LayerId layer, int frame, Image image, int x, int y) {
    const Layer* l = m_doc.layer(layer);
    if (!l || l->kind != LayerKind::Raster) return Error{ErrorCode::InvalidArgument, "Select a raster layer"};
    if (!m_doc.isLayerEditable(layer)) return Error{ErrorCode::PermissionDenied, "The layer is locked"};
    if (frame < 0 || frame >= m_doc.frameCount()) return Error{ErrorCode::InvalidArgument, "Frame out of range"};
    std::optional<Cel> before;
    if (const Cel* c = m_doc.cel(layer, frame)) before = *c;
    std::optional<Cel> after;
    if (!image.isNull()) after = Cel{std::move(image), x, y};
    const std::size_t bytes = (before ? celBytes(*before) : 0) + (after ? celBytes(*after) : 0);
    return apply(std::make_unique<FnCommand>(
        "Set Cel",
        [=](Document& d) -> Status {
            if (after) return d.setCel(layer, frame, *after);
            d.clearCel(layer, frame);
            return {};
        },
        [=](Document& d) {
            if (before) (void)d.setCel(layer, frame, *before);
            else d.clearCel(layer, frame);
        },
        bytes));
}

Result<LayerId> Editor::addAnimationLayer(std::string name, const std::vector<Image>& frames, int startFrame,
                                          int durationMs, const std::string& tagName) {
    if (frames.empty()) return Error{ErrorCode::InvalidArgument, "No frames to add"};
    if (startFrame < 0 || startFrame > m_doc.frameCount()) return Error{ErrorCode::InvalidArgument, "Frame out of range"};
    if (startFrame + static_cast<int>(frames.size()) > Document::kMaxFrames) return Error{ErrorCode::InvalidArgument, "Frame limit reached"};
    LayerId created = kNoLayer;
    const std::string label = "Add " + name;
    auto st = grouped(label, [&]() -> Status {
        while (m_doc.frameCount() < startFrame + static_cast<int>(frames.size()))
            if (auto r = insertFrame(m_doc.frameCount(), durationMs); !r) return r.error();
        LayerId top = m_doc.layers().empty() ? kNoLayer : m_doc.layers().back().id;
        while (top != kNoLayer && m_doc.layer(top)->parent != kNoLayer) top = m_doc.layer(top)->parent;   // top level
        auto id = addLayer(name, LayerKind::Raster, top);
        if (!id) return id.error();
        created = id.value();
        for (std::size_t i = 0; i < frames.size(); ++i) {
            if (frames[i].isNull() || frames[i].isFullyTransparent()) continue;
            if (auto s2 = setCelImage(created, startFrame + static_cast<int>(i), frames[i]); !s2) return s2;
        }
        if (!tagName.empty()) {
            AnimationTag t;
            t.name = tagName.substr(0, 64);
            t.fromFrame = startFrame;
            t.toFrame = startFrame + static_cast<int>(frames.size()) - 1;
            if (auto r = addTag(t); !r) return r.error();
        }
        return {};
    });
    if (!st) return st.error();
    return created;
}

Status Editor::linkCels(LayerId layer, const std::vector<int>& frames) {
    const Layer* l = m_doc.layer(layer);
    if (!l || l->kind != LayerKind::Raster) return Error{ErrorCode::InvalidArgument, "Select a raster layer to link its cels"};
    if (!m_doc.isLayerEditable(layer)) return Error{ErrorCode::PermissionDenied, "The layer is locked"};
    {   // the same checks Document::linkCels makes, before recording anything
        std::vector<int> u = frames;
        std::sort(u.begin(), u.end());
        u.erase(std::unique(u.begin(), u.end()), u.end());
        if (u.size() < 2) return Error{ErrorCode::InvalidArgument, "Select at least two frames to link"};
        if (u.front() < 0 || u.back() >= m_doc.frameCount()) return Error{ErrorCode::InvalidArgument, "Frame out of range"};
    }
    auto beforeLinks = std::make_shared<std::map<CelKey, std::uint32_t>>(m_doc.links());
    auto beforeCels = std::make_shared<std::vector<SavedCel>>();
    std::size_t bytes = 0;
    for (const auto& [k, c] : m_doc.cels())
        if (k.layer == layer) { beforeCels->push_back({k.layer, k.frame, c}); bytes += celBytes(c); }
    return apply(std::make_unique<FnCommand>(
        "Link Cels", [=](Document& d) { return d.linkCels(layer, frames); },
        [=](Document& d) {
            d.restoreLinks({});   // no propagation while the cels are put back
            for (int f = 0; f < d.frameCount(); ++f) d.clearCel(layer, f);
            for (const auto& sc : *beforeCels) (void)d.setCel(sc.layer, sc.frame, sc.cel);
            d.restoreLinks(*beforeLinks);
        },
        bytes));
}

Status Editor::unlinkCels(LayerId layer, const std::vector<int>& frames) {
    bool any = false;
    for (int f : frames) any = any || m_doc.celLink(layer, f) != 0;
    if (!any) return Error{ErrorCode::InvalidArgument, "Those cels are not linked"};
    auto before = std::make_shared<std::map<CelKey, std::uint32_t>>(m_doc.links());
    return apply(std::make_unique<FnCommand>(
        "Unlink Cels", [=](Document& d) -> Status { d.unlinkCels(layer, frames); return {}; },
        [=](Document& d) { d.restoreLinks(*before); }));
}

Status Editor::setFrameDuration(int frame, int durationMs) {
    if (frame < 0 || frame >= m_doc.frameCount()) return Error{ErrorCode::InvalidArgument, "Frame out of range"};
    const int old = m_doc.frames()[static_cast<std::size_t>(frame)].durationMs;
    return apply(std::make_unique<FnCommand>(
        "Frame Duration", [=](Document& d) { return d.setFrameDuration(frame, durationMs); },
        [=](Document& d) { (void)d.setFrameDuration(frame, old); }));
}

// ------------------------------------------------------------------ canvas

namespace {
struct CelSnapshot {
    std::vector<SavedCel> cels;
    std::vector<SavedCel> masks;
    Mask selection;
    std::size_t bytes = 0;
    static CelSnapshot capture(const Document& d) {
        CelSnapshot s;
        for (const auto& [k, c] : d.cels()) {
            s.cels.push_back({k.layer, k.frame, c});
            s.bytes += celBytes(c);
        }
        for (const auto& [k, c] : d.maskCels()) {
            s.masks.push_back({k.layer, k.frame, c});
            s.bytes += celBytes(c);
        }
        s.selection = d.selection();
        return s;
    }
    void restore(Document& d) const {
        std::vector<CelKey> keys, maskKeys;
        for (const auto& [k, c] : d.cels()) keys.push_back(k);
        for (const auto& [k, c] : d.maskCels()) maskKeys.push_back(k);
        for (const auto& k : keys) d.clearCel(k.layer, k.frame);
        for (const auto& k : maskKeys) d.clearMaskCel(k.layer, k.frame);
        for (const auto& sc : cels) (void)d.setCel(sc.layer, sc.frame, sc.cel);
        for (const auto& sc : masks) (void)d.setMaskCel(sc.layer, sc.frame, sc.cel);
        d.setSelection(selection);
    }
};

// Selection <-> image, so canvas transforms move the selection exactly like pixels.
Image selectionImage(const Mask& m) {
    Image img(m.width(), m.height());
    for (int y = 0; y < m.height(); ++y)
        for (int x = 0; x < m.width(); ++x)
            if (m.get(x, y)) img.setPixel(x, y, {255, 255, 255, 255});
    return img;
}
Mask selectionFromCel(const Cel& c, int w, int h) {
    Mask m(w, h);
    for (int y = 0; y < c.image.height(); ++y)
        for (int x = 0; x < c.image.width(); ++x)
            if (c.image.pixel(x, y).a) m.set(c.x + x, c.y + y, true);
    return m.count() ? m : Mask();
}

// Applies a per-cel geometric transform to every cel and changes the canvas
// size; undone by restoring the exact cels and the previous size.
Status applyCanvasTransform(Editor& e, const std::string& label, int newW, int newH,
                            std::function<Cel(const Cel&, int oldW, int oldH)> fn) {
    const Document& d = e.document();
    auto snap = std::make_shared<CelSnapshot>(CelSnapshot::capture(d));
    const int oldW = d.width(), oldH = d.height();
    return e.apply(std::make_unique<FnCommand>(
        label,
        [=](Document& doc) -> Status {
            if (auto st = doc.setCanvasSize(newW, newH); !st) return st;
            for (const auto& sc : snap->cels) (void)doc.setCel(sc.layer, sc.frame, fn(sc.cel, oldW, oldH));
            for (const auto& sc : snap->masks) {
                // Masks are white outside their cel: keep that by transforming
                // a full-canvas mask, then clipping to the new canvas.
                Image full(oldW, oldH, Rgba8{255, 255, 255, 255});
                full.blit(sc.cel.image, sc.cel.x, sc.cel.y);
                Cel moved = fn(Cel{std::move(full), 0, 0}, oldW, oldH);
                Image canvas(newW, newH, Rgba8{255, 255, 255, 255});
                canvas.blit(moved.image, moved.x, moved.y);
                (void)doc.setMaskCel(sc.layer, sc.frame, Cel{std::move(canvas), 0, 0});
            }
            if (!snap->selection.isNull())
                doc.setSelection(selectionFromCel(fn(Cel{selectionImage(snap->selection), 0, 0}, oldW, oldH), newW, newH));
            return {};
        },
        [=](Document& doc) {
            (void)doc.setCanvasSize(oldW, oldH);
            snap->restore(doc);
        },
        snap->bytes * 2));
}
} // namespace

Status Editor::resizeCanvas(int width, int height, int offsetX, int offsetY) {
    if (width <= 0 || height <= 0 || width > Image::kMaxDimension || height > Image::kMaxDimension)
        return Error{ErrorCode::InvalidArgument, "Canvas size must be between 1 and " + std::to_string(Image::kMaxDimension)};
    if (width == m_doc.width() && height == m_doc.height() && offsetX == 0 && offsetY == 0) return {};
    return applyCanvasTransform(*this, "Canvas Size", width, height, [=](const Cel& c, int, int) {
        return Cel{c.image, c.x + offsetX, c.y + offsetY};
    });
}

IntRect Editor::contentBounds() const {
    int x0 = INT_MAX, y0 = INT_MAX, x1 = INT_MIN, y1 = INT_MIN;
    for (const auto& [key, cel] : m_doc.cels()) {
        const Layer* l = m_doc.layer(key.layer);
        if (!l || l->kind != LayerKind::Raster) continue;
        const IntRect b = cel.image.opaqueBounds();
        if (b.empty()) continue;
        x0 = std::min(x0, cel.x + b.x);
        y0 = std::min(y0, cel.y + b.y);
        x1 = std::max(x1, cel.x + b.x + b.w);
        y1 = std::max(y1, cel.y + b.y + b.h);
    }
    if (x1 <= x0 || y1 <= y0) return {};
    return {x0, y0, x1 - x0, y1 - y0};
}

Status Editor::fitCanvasToContent() {
    const IntRect b = contentBounds();
    if (b.empty()) return Error{ErrorCode::InvalidArgument, "The document has no pixels to fit"};
    if (b.w > Image::kMaxDimension || b.h > Image::kMaxDimension)
        return Error{ErrorCode::InvalidArgument, "The content is larger than the maximum canvas size"};
    return resizeCanvas(b.w, b.h, -b.x, -b.y);
}

Status Editor::cropCanvas(const IntRect& rect) {
    const IntRect r = rect.intersected({0, 0, m_doc.width(), m_doc.height()});
    if (r.empty()) return Error{ErrorCode::InvalidArgument, "Nothing to crop to"};
    return resizeCanvas(r.w, r.h, -r.x, -r.y);
}

Status Editor::flipCanvas(bool horizontal) {
    return applyCanvasTransform(*this, horizontal ? "Flip Canvas Horizontal" : "Flip Canvas Vertical",
                                m_doc.width(), m_doc.height(), [=](const Cel& c, int W, int H) {
        if (horizontal) return Cel{flipHorizontal(c.image), W - (c.x + c.image.width()), c.y};
        return Cel{flipVertical(c.image), c.x, H - (c.y + c.image.height())};
    });
}

Status Editor::rotateCanvas90(bool clockwise) {
    return applyCanvasTransform(*this, clockwise ? "Rotate Canvas 90° CW" : "Rotate Canvas 90° CCW",
                                m_doc.height(), m_doc.width(), [=](const Cel& c, int W, int H) {
        if (clockwise) return Cel{rotate90(c.image, true), H - (c.y + c.image.height()), c.x};
        return Cel{rotate90(c.image, false), c.y, W - (c.x + c.image.width())};
    });
}

Status Editor::rotateCanvas180() {
    return applyCanvasTransform(*this, "Rotate Canvas 180°", m_doc.width(), m_doc.height(), [](const Cel& c, int W, int H) {
        return Cel{rotate180(c.image), W - (c.x + c.image.width()), H - (c.y + c.image.height())};
    });
}

// ------------------------------------------------------------------ layer transforms

Status Editor::transformLayer(LayerId layer, int frame, bool allFrames, const Mask* selection,
                              const std::string& label, const ImageFn& fn, Point offset, std::optional<IntRect> areaIn) {
    const Layer* l = m_doc.layer(layer);
    if (!l || l->kind != LayerKind::Raster) return Error{ErrorCode::InvalidArgument, "Select a raster layer to transform"};
    if (!m_doc.isLayerEditable(layer)) return Error{ErrorCode::PermissionDenied, "The layer is locked"};
    if (frame < 0 || frame >= m_doc.frameCount()) return Error{ErrorCode::InvalidArgument, "Frame out of range"};

    const bool useSel = selection && !selection->isNull() && selection->count() > 0;
    const IntRect area = areaIn && !areaIn->empty() ? *areaIn
                        : useSel ? selection->bounds() : IntRect{0, 0, m_doc.width(), m_doc.height()};

    struct Change { int frame; std::optional<Cel> before, after; };
    auto changes = std::make_shared<std::vector<Change>>();
    std::size_t bytes = 0;
    const int f0 = allFrames ? 0 : frame, f1 = allFrames ? m_doc.frameCount() - 1 : frame;
    for (int f = f0; f <= f1; ++f) {
        const Cel* c = m_doc.cel(layer, f);
        if (!c) continue;
        // Lift the selected block (the selection, or the canvas area).
        Image block(area.w, area.h);
        block.blit(c->image, c->x - area.x, c->y - area.y);
        if (useSel)
            for (int y = 0; y < block.height(); ++y)
                for (int x = 0; x < block.width(); ++x)
                    if (!selection->get(area.x + x, area.y + y)) block.setPixel(x, y, Rgba8::transparent());
        const Image result = fn(block);
        // Work on the union of the cel and the pasted result, so pixels past the
        // canvas edge (kept from imports or earlier transforms) are never lost.
        const int px = area.x + (area.w - result.width()) / 2 + offset.x;
        const int py = area.y + (area.h - result.height()) / 2 + offset.y;
        const int ux = std::min(c->x, px), uy = std::min(c->y, py);
        const int uw = std::max(c->x + c->image.width(), px + result.width()) - ux;
        const int uh = std::max(c->y + c->image.height(), py + result.height()) - uy;
        if (uw > Image::kMaxDimension || uh > Image::kMaxDimension)
            return Error{ErrorCode::InvalidArgument, "The transformed layer would be too large"};
        Image work(uw, uh);
        work.blit(c->image, c->x - ux, c->y - uy);
        // Clear the lifted pixels, then paste the result centred on the area.
        for (int y = area.y; y < area.y + area.h; ++y)
            for (int x = area.x; x < area.x + area.w; ++x)
                if (!useSel || selection->get(x, y))
                    if (work.contains(x - ux, y - uy)) work.setPixel(x - ux, y - uy, Rgba8::transparent());
        for (int y = 0; y < result.height(); ++y)
            for (int x = 0; x < result.width(); ++x) {
                const Rgba8 p = result.pixel(x, y);
                if (p.a) work.setPixel(px + x - ux, py + y - uy, p);
            }
        const IntRect bounds = work.opaqueBounds();
        std::optional<Cel> after;
        if (!bounds.empty()) after = Cel{work.copy(bounds), bounds.x + ux, bounds.y + uy};
        bytes += celBytes(*c) + (after ? celBytes(*after) : 0);
        changes->push_back({f, *c, std::move(after)});
    }
    if (changes->empty()) return Error{ErrorCode::InvalidArgument, "The layer is empty"};

    return apply(std::make_unique<FnCommand>(
        label,
        [=](Document& d) -> Status {
            for (const auto& ch : *changes) {
                if (ch.after) { if (auto st = d.setCel(layer, ch.frame, *ch.after); !st) return st; }
                else d.clearCel(layer, ch.frame);
            }
            return {};
        },
        [=](Document& d) {
            for (const auto& ch : *changes) {
                if (ch.before) (void)d.setCel(layer, ch.frame, *ch.before);
                else d.clearCel(layer, ch.frame);
            }
        },
        bytes));
}

Status Editor::moveFrame(int from, int to) {
    if (from < 0 || from >= m_doc.frameCount() || to < 0 || to >= m_doc.frameCount())
        return Error{ErrorCode::InvalidArgument, "Frame out of range"};
    if (from == to) return {};
    return apply(std::make_unique<FnCommand>(
        "Move Frame", [=](Document& d) { return d.moveFrame(from, to); },
        [=](Document& d) { (void)d.moveFrame(to, from); }));
}

Status Editor::setFrameDurations(int first, int last, int durationMs) {
    if (first < 0 || last >= m_doc.frameCount() || first > last) return Error{ErrorCode::InvalidArgument, "Frame range out of range"};
    if (durationMs < 1 || durationMs > 65535) return Error{ErrorCode::InvalidArgument, "Frame duration must be 1..65535 ms"};
    std::vector<int> old;
    for (int f = first; f <= last; ++f) old.push_back(m_doc.frames()[static_cast<std::size_t>(f)].durationMs);
    return apply(std::make_unique<FnCommand>(
        "Frame Durations",
        [=](Document& d) -> Status {
            for (int f = first; f <= last; ++f)
                if (auto st = d.setFrameDuration(f, durationMs); !st) return st;
            return {};
        },
        [=](Document& d) {
            for (int f = first; f <= last; ++f) (void)d.setFrameDuration(f, old[static_cast<std::size_t>(f - first)]);
        }));
}

namespace {
Status tagsCommand(Editor& e, const std::string& label, std::vector<AnimationTag> after) {
    auto before = std::make_shared<std::vector<AnimationTag>>(e.document().tags());
    auto next = std::make_shared<std::vector<AnimationTag>>(std::move(after));
    return e.apply(std::make_unique<FnCommand>(
        label, [=](Document& d) { return d.replaceTags(*next); },
        [=](Document& d) { (void)d.replaceTags(*before); }));
}
Status validateTag(const Document& d, const AnimationTag& t) {
    if (t.name.empty()) return Error{ErrorCode::InvalidArgument, "Tag name is required"};
    if (t.fromFrame < 0 || t.toFrame >= d.frameCount() || t.fromFrame > t.toFrame)
        return Error{ErrorCode::InvalidArgument, "Tag frames must be within 1.." + std::to_string(d.frameCount())};
    if (t.repeat < 0) return Error{ErrorCode::InvalidArgument, "Repeat count must be 0 (forever) or more"};
    return {};
}
} // namespace

Result<std::size_t> Editor::addTag(AnimationTag tag) {
    if (auto st = validateTag(m_doc, tag); !st) return st.error();
    auto tags = m_doc.tags();
    tags.push_back(std::move(tag));
    if (auto st = tagsCommand(*this, "New Tag", std::move(tags)); !st) return st.error();
    return m_doc.tags().size() - 1;
}

Status Editor::updateTag(std::size_t index, AnimationTag tag) {
    if (index >= m_doc.tags().size()) return Error{ErrorCode::InvalidArgument, "No such tag"};
    if (auto st = validateTag(m_doc, tag); !st) return st;
    auto tags = m_doc.tags();
    tags[index] = std::move(tag);
    return tagsCommand(*this, "Edit Tag", std::move(tags));
}

Status Editor::removeTag(std::size_t index) {
    if (index >= m_doc.tags().size()) return Error{ErrorCode::InvalidArgument, "No such tag"};
    auto tags = m_doc.tags();
    tags.erase(tags.begin() + static_cast<std::ptrdiff_t>(index));
    return tagsCommand(*this, "Delete Tag", std::move(tags));
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

Status Editor::replacePalette(Palette palette, const std::string& label) {
    if (palette.size() > Palette::kMaxColors) return Error{ErrorCode::InvalidArgument, "Palette has more than 256 colours"};
    auto before = std::make_shared<Palette>(m_doc.palette());
    auto after = std::make_shared<Palette>(std::move(palette));
    return apply(std::make_unique<FnCommand>(
        label, [=](Document& d) { d.palette() = *after; return Status{}; },
        [=](Document& d) { d.palette() = *before; }));
}

namespace {
Rgba8 nearestOpaque(const std::vector<Rgba8>& cols, int r, int g, int b) {
    long best = -1;
    Rgba8 out{};
    for (const auto& c : cols) {
        if (c.a == 0) continue;
        const long dr = c.r - r, dg = c.g - g, db = c.b - b;
        const long d = dr * dr + dg * dg + db * db;
        if (best < 0 || d < best) { best = d; out = {c.r, c.g, c.b, 255}; }
    }
    return out;
}
} // namespace

Status Editor::convertColorMode(ColorMode mode, bool dither) {
    const ColorMode oldMode = m_doc.colorMode();
    const auto& pal = m_doc.palette().colors();
    if (mode == ColorMode::Indexed &&
        std::none_of(pal.begin(), pal.end(), [](Rgba8 c) { return c.a > 0; }))
        return Error{ErrorCode::InvalidArgument, "The palette needs at least one opaque colour before converting to indexed"};

    auto snap = std::make_shared<CelSnapshot>(CelSnapshot::capture(m_doc));
    auto converted = std::make_shared<std::vector<SavedCel>>();
    for (const auto& sc : snap->cels) {
        Cel c = sc.cel;
        if (mode != ColorMode::Rgba) {
            for (int y = 0; y < c.image.height(); ++y)
                for (int x = 0; x < c.image.width(); ++x) {
                    const Rgba8 p = c.image.pixel(x, y);
                    if (p.a == 0) continue;
                    if (mode == ColorMode::Grayscale) {
                        const auto l = p.luma();
                        c.image.setPixel(x, y, {l, l, l, p.a});
                    } else if (p.a < 128) {
                        c.image.setPixel(x, y, Rgba8::transparent());
                    } else {
                        // Ordered dither: nudge the colour by a position-dependent
                        // offset (±24) before choosing the nearest palette entry.
                        const int off = dither ? (bayer4(c.x + x, c.y + y) - 8) * 3 : 0;
                        c.image.setPixel(x, y, nearestOpaque(pal, p.r + off, p.g + off, p.b + off));
                    }
                }
        }
        converted->push_back({sc.layer, sc.frame, std::move(c)});
    }
    const char* names[] = {"Convert to RGBA", "Convert to Indexed", "Convert to Grayscale"};
    return apply(std::make_unique<FnCommand>(
        names[static_cast<int>(mode)],
        [=](Document& d) -> Status {
            for (const auto& sc : *converted) (void)d.setCel(sc.layer, sc.frame, sc.cel);
            d.setColorMode(mode);
            return {};
        },
        [=](Document& d) {
            snap->restore(d);
            d.setColorMode(oldMode);
        },
        snap->bytes * 2));
}

} // namespace acestudio
