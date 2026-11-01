#include "acestudio/core/Script.h"

#include "acestudio/core/AssetFiles.h"
#include "acestudio/core/BlendMode.h"
#include "acestudio/core/AssetTools.h"
#include "acestudio/core/Atlas.h"
#include "acestudio/core/Autotile.h"
#include "acestudio/core/Document.h"
#include "acestudio/core/Editor.h"
#include "acestudio/core/FileUtil.h"
#include "acestudio/core/PngCodec.h"
#include "acestudio/core/ProjectIO.h"
#include "acestudio/core/Transform.h"
#include "acestudio/core/Version.h"

#include <sol/sol.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <sstream>
#include <stdexcept>

namespace acestudio {

namespace fs = std::filesystem;

namespace {

struct Runtime {
    ScriptOptions opts;
    std::size_t used = 0;
    std::chrono::steady_clock::time_point start;
    ScriptResult result;
    std::vector<std::unique_ptr<Editor>> owned;   // documents the script created or opened
    std::vector<fs::path> roots;                  // canonical allowed roots
    std::string stopped;                          // set once a limit or cancel stops the script
    // Memory outside the Lua heap, counted against the same limit: images
    // held by the script, and the documents it created, opened or grew.
    std::size_t native = 0;
    std::size_t docs = 0;
    std::size_t hostStart = 0;                    // the host document's size at the start
    lua_State* L = nullptr;
};

thread_local Runtime* tl_runtime = nullptr;

std::size_t total(const Runtime& rt) { return rt.used + rt.native + rt.docs; }

void* limitedAlloc(void* ud, void* ptr, std::size_t osize, std::size_t nsize) {
    auto* rt = static_cast<Runtime*>(ud);
    if (ptr == nullptr) osize = 0;   // osize is a type tag for new blocks
    if (nsize == 0) {
        std::free(ptr);
        rt->used -= osize;
        return nullptr;
    }
    if (nsize > osize && total(*rt) - osize + nsize > rt->opts.limits.memoryBytes) return nullptr;   // Lua raises "not enough memory"
    void* p = std::realloc(ptr, nsize);
    if (p) rt->used = rt->used - osize + nsize;
    return p;
}

Runtime* runtimeOf(lua_State* L) {
    void* ud = nullptr;
    lua_getallocf(L, &ud);
    return static_cast<Runtime*>(ud);
}

// Stops are sticky: once set, every check raises again, and the sandbox's
// pcall / xpcall / coroutine.resume re-raise, so a script cannot catch them.
void countHook(lua_State* L, lua_Debug*) {
    Runtime* rt = runtimeOf(L);
    if (rt->stopped.empty()) {
        if (rt->opts.cancel && rt->opts.cancel->load()) {
            rt->stopped = "the script was cancelled";
        } else if (rt->opts.limits.seconds > 0) {
            const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - rt->start).count();
            if (s > rt->opts.limits.seconds)
                rt->stopped = "the script ran longer than " + std::to_string(static_cast<int>(rt->opts.limits.seconds)) + " s";
        }
        if (!rt->stopped.empty()) lua_sethook(L, countHook, LUA_MASKCOUNT, 100);
    }
    if (!rt->stopped.empty()) luaL_error(L, "%s", rt->stopped.c_str());
}

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }

[[noreturn]] void outOfMemory(const Runtime& rt) {
    fail("not enough memory (images and documents count too; the limit is " + std::to_string(rt.opts.limits.memoryBytes >> 20) + " MB)");
}

// Fails before allocating `bytes` more native memory that would not fit.
// Lua's collector does not see native memory, so unreachable images are
// collected first before the limit applies.
void ensureRoom(std::size_t bytes) {
    Runtime* rt = tl_runtime;
    if (!rt || total(*rt) + bytes <= rt->opts.limits.memoryBytes) return;
    if (rt->L) lua_gc(rt->L, LUA_GCCOLLECT);
    if (total(*rt) + bytes > rt->opts.limits.memoryBytes) outOfMemory(*rt);
}

// The native memory of one script-held image, counted while it lives.
class Charge {
public:
    Charge() = default;
    explicit Charge(std::size_t bytes) { take(bytes); }
    Charge(const Charge& o) { take(o.m_bytes); }
    Charge(Charge&& o) noexcept : m_rt(o.m_rt), m_bytes(o.m_bytes) { o.m_bytes = 0; }
    Charge& operator=(const Charge& o) {
        if (this != &o) {
            Charge c(o);
            *this = std::move(c);
        }
        return *this;
    }
    Charge& operator=(Charge&& o) noexcept {
        if (this != &o) {
            release();
            m_rt = o.m_rt;
            m_bytes = o.m_bytes;
            o.m_bytes = 0;
        }
        return *this;
    }
    ~Charge() { release(); }

private:
    void take(std::size_t bytes) {
        m_rt = tl_runtime;
        if (!m_rt || bytes == 0) return;
        ensureRoom(bytes);
        m_rt->native += bytes;
        m_bytes = bytes;
    }
    void release() noexcept {
        if (m_bytes && m_rt && m_rt == tl_runtime) m_rt->native -= m_bytes;
        m_bytes = 0;
    }
    Runtime* m_rt = nullptr;
    std::size_t m_bytes = 0;
};

template <typename T>
T unwrap(Result<T> r) {
    if (!r) fail(r.error().message);
    return std::move(r).value();
}
void check(const Status& s) {
    if (!s) fail(s.error().message);
}

// ---------------------------------------------------------------- paths

fs::path resolvePath(Runtime& rt, const std::string& p) {
    if (rt.roots.empty()) fail("this script may not read or write files");
    fs::path path{std::u8string(p.begin(), p.end())};
    if (path.is_relative()) path = rt.roots.front() / path;
    std::error_code ec;
    path = fs::weakly_canonical(path, ec);
    if (ec) fail("bad path: " + p);
    for (const fs::path& root : rt.roots) {
        auto r = root.begin(), q = path.begin();
        for (; r != root.end() && q != path.end() && *r == *q; ++r, ++q) {}
        if (r == root.end()) return path;
    }
    fail("the script may not use files outside the allowed folders: " + p);
}

// A path the script writes to: inside the allowed folders, and only when
// writing is allowed (the licence's trial may have ended).
fs::path writePath(Runtime& rt, const std::string& p) {
    if (!rt.opts.readOnlyReason.empty()) fail(rt.opts.readOnlyReason);
    return resolvePath(rt, p);
}

void noteWritten(Runtime& rt, const fs::path& p) { rt.result.written.push_back(p); }

// ---------------------------------------------------------------- colours

int clampByte(double v) { return v < 0 ? 0 : v > 255 ? 255 : static_cast<int>(v + 0.5); }

Rgba8 toColor(const sol::object& o) {
    if (o.is<std::string>()) {
        auto c = Rgba8::fromHex(o.as<std::string>());
        if (!c) fail("bad colour \"" + o.as<std::string>() + "\" (use \"#rrggbb\" or \"#rrggbbaa\")");
        return *c;
    }
    if (o.is<sol::table>()) {
        sol::table t = o.as<sol::table>();
        auto get = [&](const char* k, int i, int def) {
            sol::object v = t[k].get<sol::object>();
            if (v.valid() && v.is<double>()) return clampByte(v.as<double>());
            v = t[i].get<sol::object>();
            if (v.valid() && v.is<double>()) return clampByte(v.as<double>());
            return def;
        };
        return {static_cast<std::uint8_t>(get("r", 1, 0)), static_cast<std::uint8_t>(get("g", 2, 0)), static_cast<std::uint8_t>(get("b", 3, 0)),
                static_cast<std::uint8_t>(get("a", 4, 255))};
    }
    fail("expected a colour (\"#rrggbb\" or {r, g, b, a})");
}

// ---------------------------------------------------------------- types

struct LImage {
    LImage() = default;
    explicit LImage(Image i) : img(std::move(i)), charge(img.byteSize()) {}
    Image img;
    Charge charge;
};

struct LDoc {
    Editor* ed = nullptr;
    bool host = false;   // the app's document: edits mark the result as changed
};

struct LTileset {
    std::vector<Tileset> levels;
};

// The image in a table field, or null.
template <typename Proxy>
const LImage* imageField(Proxy&& proxy) {
    sol::object o = proxy.template get<sol::object>();
    if (!o.valid() || !o.is<LImage>()) return nullptr;
    return &o.as<const LImage&>();
}

void requireCanvas(const LImage& i, int x, int y) {
    if (!i.img.contains(x, y)) fail("pixel (" + std::to_string(x) + ", " + std::to_string(y) + ") is outside the " +
                                    std::to_string(i.img.width()) + "x" + std::to_string(i.img.height()) + " image");
}

Rgba8 over(Rgba8 dst, Rgba8 src) {
    if (src.a == 255 || dst.a == 0) return src;
    if (src.a == 0) return dst;
    const int a = src.a, b = dst.a * (255 - a) / 255, oa = a + b;
    auto ch = [&](int s, int d) { return static_cast<std::uint8_t>((s * a + d * b + oa / 2) / oa); };
    return {ch(src.r, dst.r), ch(src.g, dst.g), ch(src.b, dst.b), static_cast<std::uint8_t>(oa)};
}

Editor& editorOf(LDoc& d) {
    if (!d.ed) fail("the document is closed");
    return *d.ed;
}

std::size_t docBytes(const Editor& ed) {
    std::size_t n = ed.history().memoryBytes();
    const Document& doc = ed.document();
    for (const auto& l : doc.layers())
        for (int f = 0; f < doc.frameCount(); ++f)
            if (const Cel* c = doc.cel(l.id, f)) n += c->image.byteSize();
    return n;
}

// Recounts the documents' memory after a change; fails when over the limit
// (the change is then rolled back with the rest of the script).
void recountDocs(Runtime& rt) {
    std::size_t n = 0;
    for (const auto& e : rt.owned) n += docBytes(*e);
    if (rt.opts.document) {
        const std::size_t host = docBytes(*rt.opts.document);
        if (host > rt.hostStart) n += host - rt.hostStart;
    }
    rt.docs = n;
    if (total(rt) > rt.opts.limits.memoryBytes) outOfMemory(rt);
}

void touched(Runtime& rt, LDoc& d) {
    if (d.host) rt.result.documentChanged = true;
    recountDocs(rt);
}

LayerId layerArg(Editor& ed, int index) {
    const auto& layers = ed.document().layers();
    if (index < 0 || index >= static_cast<int>(layers.size())) fail("there is no layer " + std::to_string(index));
    return layers[static_cast<std::size_t>(index)].id;
}

void frameArg(Editor& ed, int frame) {
    if (frame < 0 || frame >= ed.document().frameCount()) fail("there is no frame " + std::to_string(frame));
}

std::optional<AutotileScheme> schemeArg(const std::string& s) { return autotileSchemeFromString(s); }

unsigned enginesFrom(const sol::table& opts) {
    sol::optional<std::string> e = opts["engines"];
    return e ? unwrap(parseEngineTargets(*e)) : kEngineNone;
}

void bind(sol::state_view lua, Runtime& rt) {
    sol::table ace = lua.create_named_table("ace");
    ace["version"] = std::string(kVersionString);
    ace["lua"] = luaVersion();

    auto emit = [&rt](const std::string& s) {
        rt.result.output += s;
        if (rt.opts.output) rt.opts.output(s);
    };
    lua.set_function("print", [emit](sol::variadic_args va, sol::this_state ts) {
        sol::state_view lv(ts);
        sol::protected_function tostr = lv["tostring"].get<sol::protected_function>();
        std::string line;
        bool first = true;
        for (auto v : va) {
            if (!first) line += "\t";
            first = false;
            sol::protected_function_result r = tostr(v);
            line += r.valid() ? r.get<std::string>() : std::string("?");
        }
        emit(line + "\n");
    });
    ace["log"] = lua["print"];

    // ---- Image
    lua.new_usertype<LImage>("Image", sol::no_constructor,
        "width", sol::readonly_property([](const LImage& i) { return i.img.width(); }),
        "height", sol::readonly_property([](const LImage& i) { return i.img.height(); }),
        "get", [](const LImage& i, int x, int y) {
            requireCanvas(i, x, y);
            const Rgba8 c = i.img.pixel(x, y);
            return std::make_tuple(int(c.r), int(c.g), int(c.b), int(c.a));
        },
        "hex", [](const LImage& i, int x, int y) {
            requireCanvas(i, x, y);
            std::string h = i.img.pixel(x, y).toHex(true);
            for (char& ch : h) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return h;
        },
        "set", [](LImage& i, int x, int y, sol::object c) {
            requireCanvas(i, x, y);
            i.img.setPixel(x, y, toColor(c));
        },
        "fill", [](LImage& i, sol::object c) { i.img.fill(toColor(c)); },
        "rect", [](LImage& i, int x, int y, int w, int h, sol::object c) { i.img.fillRect({x, y, w, h}, toColor(c)); },
        "line", [](LImage& i, int x0, int y0, int x1, int y1, sol::object c) {
            const Rgba8 col = toColor(c);
            const int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
            int err = dx + dy;
            for (int guard = 0; guard < 1 << 20; ++guard) {
                i.img.setPixel(x0, y0, col);
                if (x0 == x1 && y0 == y1) break;
                const int e2 = 2 * err;
                if (e2 >= dy) { err += dy; x0 += sx; }
                if (e2 <= dx) { err += dx; y0 += sy; }
            }
        },
        "copy", [](const LImage& i, sol::optional<int> x, sol::optional<int> y, sol::optional<int> w, sol::optional<int> h) {
            if (!x) return LImage{i.img};
            return LImage{i.img.copy({*x, y.value_or(0), w.value_or(i.img.width()), h.value_or(i.img.height())})};
        },
        "blit", [](LImage& i, const LImage& src, int x, int y) { i.img.blit(src.img, x, y); },
        "draw", [](LImage& i, const LImage& src, int x, int y) {
            for (int sy = 0; sy < src.img.height(); ++sy)
                for (int sx = 0; sx < src.img.width(); ++sx)
                    if (i.img.contains(x + sx, y + sy)) i.img.setPixel(x + sx, y + sy, over(i.img.pixel(x + sx, y + sy), src.img.pixel(sx, sy)));
        },
        "scale", [](const LImage& i, int w, int h) {
            if (w > 0 && h > 0) ensureRoom(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
            return LImage{unwrap(scaleNearest(i.img, w, h))};
        },
        "flip", [](const LImage& i, sol::optional<std::string> axis) {
            return LImage{axis.value_or("h") == "v" ? flipVertical(i.img) : flipHorizontal(i.img)};
        },
        "rotate", [](const LImage& i, int degrees) {
            const int d = ((degrees % 360) + 360) % 360;
            if (d == 90) return LImage{rotate90(i.img, true)};
            if (d == 180) return LImage{rotate180(i.img)};
            if (d == 270) return LImage{rotate90(i.img, false)};
            if (d == 0) return LImage{i.img};
            fail("rotate takes 90, 180 or 270");
        },
        "outline", [](const LImage& i, sol::object c, sol::optional<bool> diagonal) {
            return LImage{outline(i.img, toColor(c), OutlineMode::Outside, diagonal.value_or(false))};
        },
        "save", [&rt](const LImage& i, const std::string& path) {
            const fs::path p = writePath(rt, path);
            check(writePng(i.img, p));
            noteWritten(rt, p);
        });
    ace["image"] = [](int w, int h, sol::optional<sol::object> c) {
        if (w < 1 || h < 1 || w > 16384 || h > 16384) fail("image size must be 1..16384");
        ensureRoom(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
        Image img(w, h);
        if (c && c->valid() && c->get_type() != sol::type::lua_nil) img.fill(toColor(*c));
        return LImage{std::move(img)};
    };
    ace["load_png"] = [&rt](const std::string& path) { return LImage{unwrap(readPng(resolvePath(rt, path)))}; };
    ace["write_text"] = [&rt](const std::string& path, const std::string& text) {
        const fs::path p = writePath(rt, path);
        check(writeTextFile(p, text));
        noteWritten(rt, p);
    };

    // ---- Document
    lua.new_usertype<LDoc>("Document", sol::no_constructor,
        "width", sol::readonly_property([](LDoc& d) { return editorOf(d).document().width(); }),
        "height", sol::readonly_property([](LDoc& d) { return editorOf(d).document().height(); }),
        "frames", sol::readonly_property([](LDoc& d) { return editorOf(d).document().frameCount(); }),
        "layers", [](LDoc& d, sol::this_state ts) {
            sol::state_view lv(ts);
            sol::table t = lv.create_table();
            int i = 0;
            for (const auto& l : editorOf(d).document().layers()) {
                sol::table e = lv.create_table_with("index", i, "name", l.name, "visible", l.visible, "opacity", int(l.opacity),
                                                    "group", l.kind == LayerKind::Group);
                e["locked"] = l.locked;
                e["blend"] = std::string(toString(l.blend));
                t[i + 1] = e;
                ++i;
            }
            return t;
        },
        "add_layer", [&rt](LDoc& d, sol::optional<std::string> name) {
            Editor& ed = editorOf(d);
            unwrap(ed.addLayer(name.value_or("Layer")));
            touched(rt, d);
            // New layers go on top: report its index.
            const auto& layers = ed.document().layers();
            return static_cast<int>(layers.size()) - 1;
        },
        "cel", [](LDoc& d, int layer, int frame) {
            Editor& ed = editorOf(d);
            frameArg(ed, frame);
            const LayerId id = layerArg(ed, layer);
            Image full(ed.document().width(), ed.document().height());
            if (const Cel* c = ed.document().cel(id, frame)) full.blit(c->image, c->x, c->y);
            return LImage{std::move(full)};
        },
        "set_cel", [&rt](LDoc& d, int layer, int frame, const LImage& img, sol::optional<int> x, sol::optional<int> y) {
            Editor& ed = editorOf(d);
            frameArg(ed, frame);
            check(ed.setCelImage(layerArg(ed, layer), frame, img.img, x.value_or(0), y.value_or(0)));
            touched(rt, d);
        },
        "set_layer", [&rt](LDoc& d, int layer, sol::table props) {
            Editor& ed = editorOf(d);
            const LayerId id = layerArg(ed, layer);
            if (auto n = props.get<sol::optional<std::string>>(std::string("name"))) check(ed.renameLayer(id, *n));
            if (auto v = props.get<sol::optional<bool>>(std::string("visible"))) check(ed.setLayerVisible(id, *v));
            if (auto l = props.get<sol::optional<bool>>(std::string("locked"))) check(ed.setLayerLocked(id, *l));
            if (auto o = props.get<sol::optional<double>>(std::string("opacity"))) {
                if (*o < 0 || *o > 255) fail("opacity is 0..255");
                check(ed.setLayerOpacity(id, static_cast<std::uint8_t>(*o + 0.5)));
            }
            if (auto b = props.get<sol::optional<std::string>>(std::string("blend"))) {
                auto mode = blendModeFromString(*b);
                if (!mode) fail("unknown blend mode \"" + *b + "\"");
                check(ed.setLayerBlend(id, *mode));
            }
            touched(rt, d);
        },
        "remove_layer", [&rt](LDoc& d, int layer) {
            Editor& ed = editorOf(d);
            if (ed.document().layers().size() < 2) fail("a document keeps at least one layer");
            check(ed.removeLayer(layerArg(ed, layer)));
            touched(rt, d);
        },
        "remove_frame", [&rt](LDoc& d, int frame) {
            Editor& ed = editorOf(d);
            frameArg(ed, frame);
            if (ed.document().frameCount() < 2) fail("a document keeps at least one frame");
            check(ed.removeFrame(frame));
            touched(rt, d);
        },
        "palette", [](LDoc& d, sol::this_state ts) {
            sol::state_view lv(ts);
            sol::table t = lv.create_table();
            int i = 1;
            for (const Rgba8 c : editorOf(d).document().palette().colors()) {
                std::string h = c.toHex(true);
                for (char& ch : h) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                t[i++] = h;
            }
            return t;
        },
        "set_palette", [&rt](LDoc& d, sol::table colors) {
            Editor& ed = editorOf(d);
            Palette p(ed.document().palette().name());
            for (std::size_t i = 1; i <= colors.size(); ++i) check(p.add(toColor(colors[i].get<sol::object>())));
            check(ed.replacePalette(std::move(p), "Script: Set Palette"));
            touched(rt, d);
        },
        "add_frame", [&rt](LDoc& d, sol::optional<int> ms) {
            Editor& ed = editorOf(d);
            const int at = unwrap(ed.insertFrame(ed.document().frameCount(), ms.value_or(100)));
            touched(rt, d);
            return at;
        },
        "duration", [](LDoc& d, int frame) {
            Editor& ed = editorOf(d);
            frameArg(ed, frame);
            return ed.document().frames()[static_cast<std::size_t>(frame)].durationMs;
        },
        "set_duration", [&rt](LDoc& d, int frame, int ms) {
            Editor& ed = editorOf(d);
            frameArg(ed, frame);
            check(ed.setFrameDuration(frame, ms));
            touched(rt, d);
        },
        "add_tag", [&rt](LDoc& d, const std::string& name, int from, int to, sol::optional<std::string> dir) {
            Editor& ed = editorOf(d);
            AnimationTag t;
            t.name = name;
            t.fromFrame = from;
            t.toFrame = to;
            if (dir) {
                auto pd = directionFromString(*dir);
                if (!pd) fail("direction must be forward, reverse or pingpong");
                t.direction = *pd;
            }
            unwrap(ed.addTag(t));
            touched(rt, d);
        },
        "tags", [](LDoc& d, sol::this_state ts) {
            sol::state_view lv(ts);
            sol::table t = lv.create_table();
            int i = 1;
            for (const auto& tag : editorOf(d).document().tags())
                t[i++] = lv.create_table_with("name", tag.name, "from", tag.fromFrame, "to", tag.toFrame, "direction", std::string(toString(tag.direction)));
            return t;
        },
        "composite", [](LDoc& d, sol::optional<int> frame) {
            Editor& ed = editorOf(d);
            const int f = frame.value_or(0);
            frameArg(ed, f);
            return LImage{ed.document().composite(f)};
        },
        "save", [&rt](LDoc& d, const std::string& path) {
            const fs::path p = writePath(rt, path);
            check(saveProject(editorOf(d).document(), p));
            noteWritten(rt, p);
        },
        "export_sheet", [&rt](LDoc& d, const std::string& path, sol::optional<sol::table> opts) {
            SheetOptions so;
            AssetWriteOptions wo;
            FrameRange range;
            if (opts) {
                const std::string layout = (*opts)["layout"].get_or(std::string("horizontal"));
                so.layout = layout == "grid" ? SheetLayout::Grid : layout == "vertical" ? SheetLayout::Vertical
                          : layout == "packed" ? SheetLayout::Packed : SheetLayout::Horizontal;
                so.trim = (*opts)["trim"].get_or(false);
                so.scale = (*opts)["scale"].get_or(1);
                range.tag = (*opts)["tag"].get_or(std::string());
                wo.engines = enginesFrom(*opts);
                wo.overwrite = (*opts)["overwrite"].get_or(false);
            }
            const WrittenFiles w = unwrap(writeSheetFiles(editorOf(d).document(), range, so, writePath(rt, path), wo));
            for (const auto& f : w.files) noteWritten(rt, f);
            return static_cast<int>(w.files.size());
        });
    ace["document"] = [&rt]() -> sol::object {
        (void)rt;
        return sol::nil;   // replaced below when the host has one
    };
    if (rt.opts.document) {
        LDoc host{rt.opts.document, true};
        ace["document"] = [host]() { return host; };
    }
    ace["new_document"] = [&rt](int w, int h, sol::optional<int> frames) {
        auto doc = unwrap(Document::create(w, h));
        unwrap(doc.addLayer("Layer 1"));
        for (int i = 1; i < frames.value_or(1); ++i) unwrap(doc.insertFrame(i));
        rt.owned.push_back(std::make_unique<Editor>(std::move(doc)));
        recountDocs(rt);
        return LDoc{rt.owned.back().get(), false};
    };
    ace["open"] = [&rt](const std::string& path) {
        rt.owned.push_back(std::make_unique<Editor>(unwrap(loadProject(resolvePath(rt, path)))));
        recountDocs(rt);
        return LDoc{rt.owned.back().get(), false};
    };

    // ---- Assets
    lua.new_usertype<LTileset>("Tileset", sol::no_constructor,
        "levels", sol::readonly_property([](const LTileset& t) { return static_cast<int>(t.levels.size()); }),
        "image", [](const LTileset& t, sol::optional<int> level) {
            const int l = level.value_or(1);
            if (l < 1 || l > static_cast<int>(t.levels.size())) fail("there is no level " + std::to_string(l));
            return LImage{t.levels[static_cast<std::size_t>(l - 1)].image};
        },
        "seams", [](const LTileset& t) {
            int n = 0;
            for (const auto& lv : t.levels) n += static_cast<int>(unwrap(checkSeams(lv)).issues.size());
            return n;
        },
        "write", [&rt](const LTileset& t, const std::string& path, sol::optional<sol::table> opts) {
            AssetWriteOptions wo;
            GodotTerrainNames names;
            if (opts) {
                wo.engines = enginesFrom(*opts);
                wo.overwrite = (*opts)["overwrite"].get_or(false);
                if (sol::optional<sol::table> n = (*opts)["names"]) {
                    names.base = (*n)[1].get_or(std::string("base"));
                    for (std::size_t i = 2; i <= n->size(); ++i) names.levels.push_back((*n)[i].get_or(std::string()));
                }
            }
            const WrittenFiles w = unwrap(writeTilesetFiles(t.levels, writePath(rt, path), wo, names));
            for (const auto& f : w.files) noteWritten(rt, f);
            return static_cast<int>(w.files.size());
        });
    ace["tileset"] = [](sol::table o) {
        const LImage* base = imageField(o["base"]);
        sol::optional<sol::table> terrains = o["terrains"];
        if (!base || !terrains || terrains->size() == 0) fail("tileset needs base = image and terrains = {image, ...}");
        const auto scheme = schemeArg(o["scheme"].get_or(std::string("blob47")));
        if (!scheme) fail("scheme must be blob47, wang16 or cardinal16");
        TilesetStyle st;
        st.roundness = o["roundness"].get_or(1.0);
        st.edgeNoise = o["noise"].get_or(0.3);
        st.noiseScale = o["noise_scale"].get_or(4);
        st.outline = o["outline"].get_or(true);
        st.seed = static_cast<std::uint32_t>(o["seed"].get_or(1));
        const int tile = o["tile"].get_or(0);
        LTileset t;
        for (std::size_t i = 1; i <= terrains->size(); ++i) {
            const LImage* tex = imageField((*terrains)[i]);
            if (!tex) fail("terrains must all be images");
            t.levels.push_back(unwrap(generateTileset(*scheme, i == 1 ? base->img : Image(), tex->img, st, tile)));
        }
        return t;
    };
    ace["check_seams"] = [](const LImage& img, sol::optional<std::string> scheme, sol::optional<int> phases) {
        const auto s = schemeArg(scheme.value_or("blob47"));
        if (!s) fail("scheme must be blob47, wang16 or cardinal16");
        Tileset t;
        t.scheme = *s;
        t.phases = phases.value_or(1);
        t.image = img.img;
        t.tileSize = img.img.width() / (tilesetGrid(*s).first * std::max(1, t.phases));
        const SeamReport r = unwrap(checkSeams(t));
        return std::make_tuple(static_cast<int>(r.issues.size()), r.pairsChecked);
    };
    ace["atlas"] = [](sol::table items, sol::optional<sol::table> opts, sol::this_state ts) {
        std::vector<AtlasItem> list;
        for (std::size_t i = 1; i <= items.size(); ++i) {
            sol::optional<sol::table> it = items[i];
            if (!it) fail("atlas items are {name = ..., image = ...}");
            const LImage* img = imageField((*it)["image"]);
            if (!img) fail("atlas item " + std::to_string(i) + " has no image");
            list.push_back({(*it)["name"].get_or("sprite_" + std::to_string(i - 1)), img->img});
        }
        AtlasOptions o;
        o.multiplePages = false;
        if (opts) {
            o.padding = (*opts)["padding"].get_or(1);
            o.extrude = (*opts)["extrude"].get_or(0);
            o.trim = (*opts)["trim"].get_or(false);
            o.allowRotation = (*opts)["rotate"].get_or(false);
            o.maxWidth = o.maxHeight = (*opts)["max"].get_or(4096);
        }
        const Atlas a = unwrap(packAtlas(list, o));
        sol::state_view lv(ts);
        sol::table sprites = lv.create_table();
        for (std::size_t i = 0; i < a.sprites.size(); ++i) {
            const auto& s = a.sprites[i];
            sprites[i + 1] = lv.create_table_with("name", s.name, "x", s.rect.x, "y", s.rect.y, "w", s.rect.w, "h", s.rect.h, "rotated", s.rotated);
        }
        return lv.create_table_with("image", LImage{a.pages.front()}, "sprites", sprites, "json", atlasJson(a, {"atlas.png"}));
    };
    ace["slice"] = [](const LImage& img, sol::optional<sol::table> opts, sol::this_state ts) {
        SliceOptions so;
        std::vector<SlicedSprite> slices;
        std::string grid;
        if (opts) {
            if (sol::optional<sol::object> bg = (*opts)["background"]; bg && bg->get_type() != sol::type::lua_nil) so.background = toColor(*bg);
            so.tolerance = (*opts)["tolerance"].get_or(8);
            so.mergeDistance = (*opts)["merge"].get_or(2);
            grid = (*opts)["grid"].get_or(std::string());
        }
        if (grid == "auto") {
            const GridGuess g = detectGrid(img.img, so);
            if (!g.found) fail("no regular grid found");
            slices = sliceGrid(img.img, g, true, so);
        } else if (!grid.empty()) {
            const auto x = grid.find('x');
            if (x == std::string::npos) fail("grid is \"WxH\" or \"auto\"");
            GridGuess g;
            g.cellWidth = std::stoi(grid.substr(0, x));
            g.cellHeight = std::stoi(grid.substr(x + 1));
            if (g.cellWidth < 1 || g.cellHeight < 1) fail("grid cells must be at least 1x1");
            g.columns = img.img.width() / g.cellWidth;
            g.rows = img.img.height() / g.cellHeight;
            g.found = true;
            slices = sliceGrid(img.img, g, true, so);
        } else {
            slices = detectSprites(img.img, so);
        }
        sol::state_view lv(ts);
        sol::table t = lv.create_table();
        for (std::size_t i = 0; i < slices.size(); ++i)
            t[i + 1] = lv.create_table_with("name", slices[i].name, "x", slices[i].rect.x, "y", slices[i].rect.y, "w", slices[i].rect.w, "h", slices[i].rect.h);
        return t;
    };
    ace["write_uikit"] = [&rt](const std::string& path, sol::optional<sol::table> opts) {
        UiTheme th;
        AssetWriteOptions wo;
        if (opts) {
            th.scale = (*opts)["scale"].get_or(1);
            th.rounded = (*opts)["rounded"].get_or(true);
            Rgba8* slots[] = {&th.base, &th.light, &th.dark, &th.outline, &th.accent, &th.text};
            const char* keys[] = {"base", "light", "dark", "outline", "accent", "text"};
            for (int i = 0; i < 6; ++i)
                if (sol::optional<sol::object> c = (*opts)[keys[i]]; c && c->get_type() != sol::type::lua_nil) *slots[i] = toColor(*c);
            wo.engines = enginesFrom(*opts);
            wo.overwrite = (*opts)["overwrite"].get_or(false);
        }
        const WrittenFiles w = unwrap(writeUiKitFiles(generateUiKit(th), writePath(rt, path), wo));
        for (const auto& f : w.files) noteWritten(rt, f);
        return static_cast<int>(w.files.size());
    };

    // require: plain .lua modules from the allowed folders ("a.b" -> a/b.lua),
    // run in the sandbox like the script itself. Loaded modules are cached.
    lua.set_function("__ace_module_source", [&rt](const std::string& name) {
        if (name.empty() || name.size() > 200 || name.front() == '.' || name.back() == '.' || name.find("..") != std::string::npos)
            fail("bad module name \"" + name + "\"");
        for (char ch : name)
            if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '.' || ch == '-')) fail("bad module name \"" + name + "\"");
        std::string rel = name;
        std::replace(rel.begin(), rel.end(), '.', '/');
        rel += ".lua";
        if (rt.roots.empty()) fail("module '" + name + "' not found (this script may not read files)");
        for (const fs::path& root : rt.roots) {
            std::error_code ec;
            const std::u8string u = (root / fs::path(std::u8string(rel.begin(), rel.end()))).u8string();
            const fs::path file = resolvePath(rt, std::string(u.begin(), u.end()));   // no way out through links
            if (!fs::is_regular_file(file, ec)) continue;
            auto bytes = unwrap(readFileBytes(file, 4u * 1024u * 1024u));
            return std::make_tuple(std::string(bytes.begin(), bytes.end()), "@" + rel);
        }
        fail("module '" + name + "' not found in the allowed folders (looked for " + rel + ")");
    });
    lua.safe_script(R"(
        local source, load_text = __ace_module_source, load
        __ace_module_source = nil
        local loaded, loading = {}, {}
        require = function(name)
          if type(name) ~= "string" then error("require takes a module name", 2) end
          if loaded[name] ~= nil then return loaded[name] end
          if loading[name] then error("require: module '" .. name .. "' requires itself", 2) end
          local code, chunk = source(name)
          local f, err = load_text(code, chunk, "t")
          if not f then error(err, 2) end
          loading[name] = true
          local r = f(name)
          loading[name] = nil
          if r == nil then r = true end
          loaded[name] = r
          return r
        end
    )", sol::script_throw_on_error, "=sandbox");

    sol::table args = lua.create_table();
    for (std::size_t i = 0; i < rt.opts.args.size(); ++i) args[i + 1] = rt.opts.args[i];
    lua["arg"] = args;
}

void sandbox(sol::state_view lua) {
    lua.open_libraries(sol::lib::base, sol::lib::string, sol::lib::table, sol::lib::math, sol::lib::utf8, sol::lib::coroutine,
                       sol::lib::os);
    lua["dofile"] = sol::nil;
    lua["loadfile"] = sol::nil;
    // load: text chunks only (precompiled bytecode can crash Lua). Written in
    // Lua so that a missing env argument stays missing.
    lua.safe_script("local raw = load; load = function(chunk, name, mode, ...) return raw(chunk, name, 't', ...) end",
                    sol::script_throw_on_error, "=sandbox");
    // Protected calls re-raise a limit stop or cancel (see countHook).
    lua.set_function("__ace_stopped", [](sol::this_state ts) {
        const Runtime* rt = runtimeOf(ts.lua_state());
        return rt->stopped.empty() ? sol::optional<std::string>() : sol::optional<std::string>(rt->stopped);
    });
    lua.safe_script(R"(
        local stopped = __ace_stopped
        __ace_stopped = nil
        local pack, unpack = table.pack, table.unpack
        local function guard(r)
          local why = stopped()
          if why then error(why, 0) end
          return unpack(r, 1, r.n)
        end
        local raw_pcall, raw_xpcall, raw_resume = pcall, xpcall, coroutine.resume
        pcall = function(...) return guard(pack(raw_pcall(...))) end
        xpcall = function(...) return guard(pack(raw_xpcall(...))) end
        coroutine.resume = function(...) return guard(pack(raw_resume(...))) end
    )", sol::script_throw_on_error, "=sandbox");
    sol::table os = lua["os"].get<sol::table>();
    sol::table safeOs = lua.create_table();
    for (const char* f : {"time", "clock", "date", "difftime"}) safeOs[f] = os[f];
    lua["os"] = safeOs;
    lua["string"]["dump"] = sol::nil;
}

} // namespace

std::string luaVersion() { return std::string("Lua ") + LUA_VERSION_MAJOR "." LUA_VERSION_MINOR "." LUA_VERSION_RELEASE; }

Result<ScriptResult> runScript(const std::string& source, const std::string& chunkName, const ScriptOptions& opts) {
    Runtime rt;
    rt.opts = opts;
    rt.start = std::chrono::steady_clock::now();
    for (const auto& r : opts.allowedRoots) {
        std::error_code ec;
        fs::path c = fs::weakly_canonical(r, ec);
        if (!ec) rt.roots.push_back(c);
    }
    if (opts.document) rt.hostStart = docBytes(*opts.document);
    lua_State* L = lua_newstate(limitedAlloc, &rt);
    if (!L) return Error{ErrorCode::Internal, "Could not start Lua"};
    tl_runtime = &rt;
    rt.L = L;
    // The editor command group: the host document changes in one undo step.
    std::size_t mark = 0;
    if (opts.document) mark = opts.document->history().mark();
    std::string failure;
    try {
        sol::state_view lua(L);
        sandbox(lua);
        bind(lua, rt);
        lua_sethook(L, countHook, LUA_MASKCOUNT, 10000);
        sol::protected_function_result r = lua.safe_script(source, sol::script_pass_on_error, "@" + chunkName);
        if (!r.valid()) {
            sol::error e = r;
            failure = e.what();
        }
        if (!rt.stopped.empty()) failure = rt.stopped;
    } catch (const std::exception& e) {
        failure = e.what();
    } catch (...) {
        failure = "the script stopped with an unknown error";
    }
    rt.owned.clear();
    lua_close(L);
    tl_runtime = nullptr;
    if (opts.document) {
        Editor& ed = *opts.document;
        if (!failure.empty()) {
            // Roll back everything the script did to the host document.
            while (ed.history().mark() > mark && ed.history().canUndo()) (void)ed.undo();
        } else if (ed.history().mark() > mark + 1) {
            (void)ed.history().squash(mark, "Run Script");
        }
    }
    if (!failure.empty()) {
        if (failure.find("not enough memory") != std::string::npos && failure.find("count too") == std::string::npos)
            failure += " (the script used more than " + std::to_string(opts.limits.memoryBytes >> 20) + " MB)";
        return Error{ErrorCode::InvalidArgument, failure};
    }
    return std::move(rt.result);
}

Result<ScriptResult> runScriptFile(const fs::path& file, ScriptOptions opts) {
    auto bytes = readFileBytes(file, 16u * 1024u * 1024u);
    if (!bytes) return bytes.error();
    const std::string source(bytes.value().begin(), bytes.value().end());
    if (opts.allowedRoots.empty()) {
        std::error_code ec;
        opts.allowedRoots.push_back(fs::absolute(file, ec).parent_path());
    }
    return runScript(source, file.filename().string(), opts);
}

} // namespace acestudio
