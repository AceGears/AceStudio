#include "AceTools.h"

#include "acestudio/core/FileUtil.h"
#include "acestudio/core/Subprocess.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <random>
#include <thread>

namespace acestudio::mcp {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

std::string base64(const std::vector<std::uint8_t>& bytes) {
    static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const unsigned v = (unsigned(bytes[i]) << 16) | (unsigned(bytes[i + 1]) << 8) | bytes[i + 2];
        out += k[v >> 18];
        out += k[(v >> 12) & 63];
        out += k[(v >> 6) & 63];
        out += k[v & 63];
    }
    if (i + 1 == bytes.size()) {
        const unsigned v = unsigned(bytes[i]) << 16;
        out += k[v >> 18];
        out += k[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == bytes.size()) {
        const unsigned v = (unsigned(bytes[i]) << 16) | (unsigned(bytes[i + 1]) << 8);
        out += k[v >> 18];
        out += k[(v >> 12) & 63];
        out += k[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

Result<fs::path> resolveInRoots(const std::vector<fs::path>& roots, const std::string& p) {
    if (roots.empty()) return Error{ErrorCode::PermissionDenied, "No folders are allowed (start acestudio-mcp with --root)"};
    if (p.empty()) return Error{ErrorCode::InvalidArgument, "Empty path"};
    fs::path path{std::u8string(p.begin(), p.end())};
    if (path.is_relative()) path = roots.front() / path;
    std::error_code ec;
    path = fs::weakly_canonical(path, ec);
    if (ec) return Error{ErrorCode::InvalidArgument, "Bad path: " + p};
    for (const fs::path& root : roots) {
        auto r = root.begin(), q = path.begin();
        for (; r != root.end() && q != path.end() && *r == *q; ++r, ++q) {}
        if (r == root.end()) return path;
    }
    std::string allowed;
    for (const auto& r : roots) allowed += (allowed.empty() ? "" : ", ") + r.string();
    return Error{ErrorCode::PermissionDenied, p + " is outside the folders this server may use (" + allowed + ")"};
}

namespace {

std::string utf8(const fs::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

// ---------------------------------------------------------------- schemas

json obj(json properties, std::vector<std::string> required = {}) {
    json s{{"type", "object"}, {"properties", std::move(properties)}, {"additionalProperties", false}};
    if (!required.empty()) s["required"] = required;
    return s;
}
json str(const std::string& d) { return {{"type", "string"}, {"description", d}}; }
json integer(const std::string& d, std::optional<int> min = {}, std::optional<int> max = {}) {
    json j{{"type", "integer"}, {"description", d}};
    if (min) j["minimum"] = *min;
    if (max) j["maximum"] = *max;
    return j;
}
json number(const std::string& d) { return {{"type", "number"}, {"description", d}}; }
json boolean(const std::string& d) { return {{"type", "boolean"}, {"description", d}}; }
json strings(const std::string& d) { return {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", d}}; }
json oneOf(const std::string& d, std::vector<std::string> values) { return {{"type", "string"}, {"enum", values}, {"description", d}}; }

const char* kPathNote = " Relative paths are inside the server's first --root folder.";
const json kEngines = str("Also write engine files: comma list of godot, unity, tiled (e.g. \"godot,unity\")");
const json kOverwrite = boolean("Replace existing files (default false: the tool refuses to overwrite)");

json readOnly() { return {{"readOnlyHint", true}, {"destructiveHint", false}, {"openWorldHint", false}}; }
json writes() { return {{"readOnlyHint", false}, {"destructiveHint", false}, {"idempotentHint", true}, {"openWorldHint", false}}; }

// ---------------------------------------------------------------- CLI runner

struct CliOut {
    int exitCode = 0;
    json result;          // the CLI's --json object
    std::string stderrText;
};

Result<CliOut> runCli(const ToolEnv& env, std::vector<std::string> args, const ToolCall& call,
                      std::chrono::milliseconds timeout = std::chrono::minutes(10)) {
    ProcessSpec spec;
    spec.program = env.cli;
    spec.args.push_back("--json");
    for (auto& a : args) spec.args.push_back(std::move(a));
    spec.workingDir = env.roots.empty() ? fs::path() : env.roots.front();
    auto proc = Subprocess::start(spec);
    if (!proc) return Error{proc.error().code, "Cannot run acestudio-cli (" + utf8(env.cli) + "): " + proc.error().message};
    Subprocess& p = *proc.value();
    p.closeStdin();
    const auto start = std::chrono::steady_clock::now();
    std::vector<std::string> out;
    std::string err;
    for (;;) {
        for (auto& l : p.takeLines(Subprocess::Stream::Stdout)) out.push_back(std::move(l));
        for (auto& l : p.takeLines(Subprocess::Stream::Stderr)) {
            // AI jobs report {"progress": 0..1, "message": ...} lines.
            if (!l.empty() && l.front() == '{') {
                const json pj = json::parse(l, nullptr, false);
                if (pj.is_object() && pj.contains("progress")) {
                    const double pr = pj["progress"].is_number() ? pj["progress"].get<double>() : -1.0;
                    if (call.progress && pr >= 0) call.progress(pr * 100.0, 100.0, pj.value("message", std::string()));
                    if (call.log) call.log("debug", pj.value("message", std::string()));
                    continue;
                }
            }
            if (call.log && !l.empty()) call.log("debug", l);
            err += l + "\n";
        }
        if (p.exitCode()) break;
        if (call.cancel && call.cancel->load()) {
            p.kill();
            return Error{ErrorCode::Cancelled, "Cancelled"};
        }
        if (std::chrono::steady_clock::now() - start > timeout) {
            p.kill();
            return Error{ErrorCode::Cancelled, "acestudio-cli did not finish in time"};
        }
        (void)p.waitForLine(50ms);
    }
    for (auto& l : p.takeLines(Subprocess::Stream::Stdout)) out.push_back(std::move(l));
    for (auto& l : p.takeLines(Subprocess::Stream::Stderr)) err += l + "\n";
    CliOut r;
    r.exitCode = *p.exitCode();
    r.stderrText = err;
    for (auto it = out.rbegin(); it != out.rend(); ++it) {
        auto j = json::parse(*it, nullptr, false);
        if (j.is_object()) {
            r.result = std::move(j);
            break;
        }
    }
    if (r.result.is_null())
        return Error{ErrorCode::Internal, "acestudio-cli gave no JSON result (exit " + std::to_string(r.exitCode) + ")" +
                                              (err.empty() ? std::string() : ": " + err)};
    return r;
}

bool hasArg(const std::vector<std::string>& args, const char* a) { return std::find(args.begin(), args.end(), a) != args.end(); }

// Runs the CLI; when it refuses to replace existing files, asks the user
// (elicitation) whether to replace them, and runs again with --force if so.
// In the 2026 era the question goes out as input_required and this returns
// while the client asks; the retry carries the answer.
Result<CliOut> runCliConfirm(const ToolEnv& env, std::vector<std::string> args, const ToolCall& call,
                             std::chrono::milliseconds timeout = std::chrono::minutes(10)) {
    auto r = runCli(env, args, call, timeout);
    if (!r || r.value().result.value("ok", false) || hasArg(args, "--force") || !call.input ||
        !call.input->supports("elicitation/create"))
        return r;
    const std::string err = r.value().result.value("error", std::string());
    if (err.find("already exists") == std::string::npos) return r;
    json params{{"message", err.substr(0, err.find(" (use")) + ". Replace the existing files?"},
                {"requestedSchema",
                 {{"type", "object"},
                  {"properties", {{"replace", {{"type", "boolean"}, {"title", "Replace the existing files"}, {"default", false}}}}},
                  {"required", {"replace"}}}}};
    auto answer = call.input->ask("replace_existing", "elicitation/create", params);
    if (!answer) return r;   // modern: asked (input_required); legacy: the client could not answer
    if (answer->value("action", std::string()) != "accept" || !answer->value("content", json::object()).value("replace", false)) {
        r.value().result["error"] = err + " — kept, as you chose";
        return r;
    }
    args.push_back("--force");
    return runCli(env, args, call, timeout);
}

// The CLI result as a tool result: text summary + structured content.
ToolResult fromCli(const Result<CliOut>& r, const std::string& okText = {}) {
    if (!r) return ToolResult::text(r.error().message, true);
    const CliOut& c = r.value();
    if (!c.result.value("ok", false)) {
        std::string msg = c.result.value("error", std::string("failed"));
        return ToolResult::text(msg, true);
    }
    json data = c.result;
    data.erase("ok");
    ToolResult t = ToolResult::text(okText.empty() ? data.dump(2) : okText + "\n" + data.dump(2));
    t.structured = data;
    return t;
}

// Argument helpers: each returns false and sets `error` on a bad path.
struct ArgBuilder {
    const ToolEnv& env;
    const json& a;
    std::vector<std::string> args;
    std::string error;

    bool path(const std::string& key, const char* flag = nullptr) {
        if (!a.contains(key) || a[key].is_null()) return true;
        auto p = resolveInRoots(env.roots, a[key].get<std::string>());
        if (!p) {
            error = p.error().message;
            return false;
        }
        if (flag) args.push_back(flag);
        args.push_back(utf8(p.value()));
        return true;
    }
    bool paths(const std::string& key, const char* flag, char sep) {
        if (!a.contains(key)) return true;
        std::string joined;
        for (const auto& v : a[key]) {
            if (!v.is_string()) {
                error = key + " must be a list of paths";
                return false;
            }
            auto p = resolveInRoots(env.roots, v.get<std::string>());
            if (!p) {
                error = p.error().message;
                return false;
            }
            if (sep) joined += (joined.empty() ? "" : std::string(1, sep)) + utf8(p.value());
            else args.push_back(utf8(p.value()));
        }
        if (sep && !joined.empty()) {
            if (flag) args.push_back(flag);
            args.push_back(joined);
        }
        return true;
    }
    void opt(const std::string& key, const char* flag) {
        if (!a.contains(key) || a[key].is_null()) return;
        args.push_back(flag);
        const json& v = a[key];
        if (v.is_string()) args.push_back(v.get<std::string>());
        else if (v.is_number_integer()) args.push_back(std::to_string(v.get<long long>()));
        else if (v.is_number()) args.push_back(std::to_string(v.get<double>()));
        else args.push_back(v.dump());
    }
    void flag(const std::string& key, const char* f) {
        if (a.value(key, false)) args.push_back(f);
    }
    void overwrite() { flag("overwrite", "--force"); }
};

ToolResult badArgs(const std::string& e) { return ToolResult::text(e, true); }

Result<std::string> pngBase64(const fs::path& p) {
    auto bytes = readFileBytes(p, 32u * 1024u * 1024u);
    if (!bytes) return bytes.error();
    return base64(bytes.value());
}

fs::path tempFile(const std::string& ext) {
    std::random_device rd;
    return fs::temp_directory_path() / ("acestudio-mcp-" + std::to_string(rd()) + std::to_string(rd()) + ext);
}

} // namespace

namespace {
std::vector<Tool> aceStudioTools(const ToolEnv& envIn) {
    auto envPtr = std::make_shared<ToolEnv>(envIn);
    std::vector<Tool> t;

    // ---------------------------------------------------------- projects
    t.push_back({"document_info", "Describe a project",
                 std::string("Size, colour mode, frames, layers, tags and slices of an AceStudio project (.acestudio).") + kPathNote,
                 obj({{"path", str("The .acestudio file")}}, {"path"}), readOnly(),
                 [envPtr](const json& a, const ToolCall& call) {
                     ArgBuilder b{*envPtr, a, {"info"}, {}};
                     if (!b.path("path")) return badArgs(b.error);
                     return fromCli(runCli(*envPtr, b.args, call));
                 }});
    t.push_back({"document_new", "Create a project",
                 std::string("Creates an empty AceStudio project with one layer.") + kPathNote,
                 obj({{"path", str("The .acestudio file to create")},
                      {"width", integer("Canvas width in pixels", 1, 16384)},
                      {"height", integer("Canvas height in pixels", 1, 16384)},
                      {"frames", integer("Number of frames (default 1)", 1, 10000)},
                      {"color_mode", oneOf("Colour mode (default rgba)", {"rgba", "indexed", "grayscale"})},
                      {"overwrite", kOverwrite}},
                     {"path", "width", "height"}),
                 writes(),
                 [envPtr](const json& a, const ToolCall& call) {
                     ArgBuilder b{*envPtr, a, {"new"}, {}};
                     if (!b.path("path")) return badArgs(b.error);
                     b.opt("width", "--width");
                     b.opt("height", "--height");
                     b.opt("frames", "--frames");
                     b.opt("color_mode", "--color-mode");
                     b.overwrite();
                     return fromCli(runCliConfirm(*envPtr, b.args, call));
                 }});
    t.push_back({"render_frame", "Render a frame",
                 std::string("Flattens one frame of a project and returns it as a PNG image (nearest-neighbour scale). "
                             "Optionally also saves it.") + kPathNote,
                 obj({{"path", str("The .acestudio file")},
                      {"frame", integer("Frame index, 0-based (default 0)", 0)},
                      {"scale", integer("Integer scale for viewing (default: up to 8x, about 512 px)", 1, 64)},
                      {"out", str("Optional PNG to save")},
                      {"overwrite", kOverwrite}},
                     {"path"}),
                 readOnly(),
                 [envPtr](const json& a, const ToolCall& call) {
                     const ToolEnv& e = *envPtr;
                     ArgBuilder b{e, a, {"render"}, {}};
                     if (!b.path("path")) return badArgs(b.error);
                     fs::path out;
                     bool temp = false;
                     if (a.contains("out")) {
                         auto p = resolveInRoots(e.roots, a["out"].get<std::string>());
                         if (!p) return badArgs(p.error().message);
                         out = p.value();
                         b.overwrite();
                     } else {
                         out = tempFile(".png");
                         temp = true;
                     }
                     int scale = a.value("scale", 0);
                     if (scale == 0) {
                         // Pick a viewing scale from the canvas size.
                         auto info = runCli(e, {"info", b.args[1]}, call);
                         int w = 64, h = 64;
                         if (info && info.value().result.value("ok", false)) {
                             w = info.value().result.value("width", 64);
                             h = info.value().result.value("height", 64);
                         }
                         scale = std::clamp(512 / std::max({w, h, 1}), 1, 8);
                     }
                     b.args.insert(b.args.end(), {"--out", utf8(out), "--scale", std::to_string(scale)});
                     if (a.contains("frame")) b.opt("frame", "--frame");
                     ToolResult r = fromCli(runCli(e, b.args, call));
                     if (!r.isError) {
                         auto img = pngBase64(out);
                         if (img) r.addImage(img.value());
                         else r = ToolResult::text(img.error().message, true);
                     }
                     if (temp) {
                         std::error_code ec;
                         fs::remove(out, ec);
                         if (r.structured) {
                             r.structured->erase("path");
                             r.content[0]["text"] = r.structured->dump(2);
                         }
                     }
                     return r;
                 }});
    t.push_back({"export_animation", "Export frames",
                 std::string("Exports a project: a sprite sheet PNG + JSON (optionally with Godot SpriteFrames and Unity "
                             ".meta/.anim files), a GIF, APNG, WebP, or a PNG sequence.") + kPathNote,
                 obj({{"path", str("The .acestudio file")},
                      {"format", oneOf("Output format", {"sheet", "gif", "apng", "webp", "png"})},
                      {"out", str("Output file (png: a folder)")},
                      {"tag", str("Only the frames of this animation tag")},
                      {"from", integer("First frame (0-based)", 0)},
                      {"to", integer("Last frame (0-based)", 0)},
                      {"scale", integer("Integer scale", 1, 64)},
                      {"layout", oneOf("Sheet layout", {"horizontal", "vertical", "grid", "packed"})},
                      {"columns", integer("Grid columns", 1)},
                      {"padding", integer("Sheet padding", 0)},
                      {"trim", boolean("Trim transparent borders (sheet)")},
                      {"engines", kEngines},
                      {"overwrite", kOverwrite}},
                     {"path", "format", "out"}),
                 writes(),
                 [envPtr](const json& a, const ToolCall& call) {
                     ArgBuilder b{*envPtr, a, {"export"}, {}};
                     if (!b.path("path") || !b.path("out", "--out")) return badArgs(b.error);
                     b.opt("format", "--format");
                     for (const auto& [k, f] : std::vector<std::pair<std::string, const char*>>{
                              {"tag", "--tag"}, {"from", "--from"}, {"to", "--to"}, {"scale", "--scale"}, {"layout", "--layout"},
                              {"columns", "--columns"}, {"padding", "--padding"}, {"engines", "--engines"}})
                         b.opt(k, f);
                     b.flag("trim", "--trim");
                     b.overwrite();
                     return fromCli(runCliConfirm(*envPtr, b.args, call));
                 }});

    // ---------------------------------------------------------- asset tools
    t.push_back({"tileset_generate", "Generate an autotile tileset",
                 std::string("Builds a seam-checked autotile set from textures: a base texture and one or more terrain "
                             "textures layered over it (Blob 47, Wang 16 or Cardinal 16). Writes the tileset PNG(s), a "
                             "JSON description and optional Godot TileSet .tres, Unity .meta and Tiled .tsx files.") + kPathNote,
                 obj({{"base", str("Base (level 0) texture PNG")},
                      {"terrains", strings("Terrain texture PNGs, lowest level first (1-3)")},
                      {"out", str("Output tileset PNG")},
                      {"scheme", oneOf("Autotile scheme (default blob47)", {"blob47", "wang16", "cardinal16"})},
                      {"tile", integer("Tile size in pixels (default: the texture size)", 4, 512)},
                      {"phases", integer("Texture phases (variants), 1-4", 1, 4)},
                      {"roundness", number("0 = straight, 1 = rounded transitions")},
                      {"noise", number("Edge raggedness 0..0.9")},
                      {"seed", integer("Random seed")},
                      {"outline", boolean("Dark line along the terrain edge (default true)")},
                      {"names", strings("Terrain names: base first, then one per terrain")},
                      {"engines", kEngines},
                      {"overwrite", kOverwrite}},
                     {"base", "terrains", "out"}),
                 writes(),
                 [envPtr](const json& a, const ToolCall& call) {
                     ArgBuilder b{*envPtr, a, {"tileset"}, {}};
                     if (!b.path("base", "--base") || !b.paths("terrains", "--terrain", ',') || !b.path("out", "--out"))
                         return badArgs(b.error);
                     for (const auto& [k, f] : std::vector<std::pair<std::string, const char*>>{
                              {"scheme", "--scheme"}, {"tile", "--tile"}, {"phases", "--phases"}, {"roundness", "--roundness"},
                              {"noise", "--noise"}, {"seed", "--seed"}, {"engines", "--engines"}})
                         b.opt(k, f);
                     if (a.contains("outline") && !a["outline"].get<bool>()) b.args.push_back("--no-outline");
                     if (a.contains("names")) {
                         std::string joined;
                         for (const auto& n : a["names"]) joined += (joined.empty() ? "" : ",") + n.get<std::string>();
                         b.args.insert(b.args.end(), {"--names", joined});
                     }
                     b.overwrite();
                     return fromCli(runCliConfirm(*envPtr, b.args, call));
                 }});
    t.push_back({"tileset_check_seams", "Check tileset seams",
                 std::string("Checks every pair of tiles that can touch in an autotile set and reports visible seams.") + kPathNote,
                 obj({{"image", str("Tileset PNG")},
                      {"scheme", oneOf("Autotile scheme", {"blob47", "wang16", "cardinal16"})},
                      {"phases", integer("Phases in the image (default 1)", 1, 4)},
                      {"base", str("Base texture PNG for a transparent upper level")}},
                     {"image", "scheme"}),
                 readOnly(),
                 [envPtr](const json& a, const ToolCall& call) {
                     ArgBuilder b{*envPtr, a, {"seams"}, {}};
                     if (!b.path("image") || !b.path("base", "--base")) return badArgs(b.error);
                     b.opt("scheme", "--scheme");
                     b.opt("phases", "--phases");
                     auto r = runCli(*envPtr, b.args, call);
                     // Exit 3 = seams found: a result, not a failure.
                     if (r && r.value().exitCode == 3 && r.value().result.contains("issues")) {
                         json data = r.value().result;
                         data.erase("ok");
                         ToolResult t2 = ToolResult::text("Seams found:\n" + data.dump(2));
                         t2.structured = data;
                         return t2;
                     }
                     return fromCli(r);
                 }});
    t.push_back({"atlas_pack", "Pack a sprite atlas",
                 std::string("Packs PNG sprites into a deterministic atlas with TexturePacker-style JSON (and optional "
                             "Unity .meta).") + kPathNote,
                 obj({{"images", strings("Sprite PNGs")},
                      {"out", str("Output atlas PNG")},
                      {"max", integer("Maximum page size (default 4096)", 16, 16384)},
                      {"padding", integer("Pixels between sprites (default 1)", 0, 64)},
                      {"extrude", integer("Edge extrusion in pixels", 0, 16)},
                      {"trim", boolean("Trim transparent borders")},
                      {"rotate", boolean("Allow 90° rotation")},
                      {"pot", boolean("Power-of-two pages")},
                      {"engines", kEngines},
                      {"overwrite", kOverwrite}},
                     {"images", "out"}),
                 writes(),
                 [envPtr](const json& a, const ToolCall& call) {
                     ArgBuilder b{*envPtr, a, {"atlas"}, {}};
                     if (!b.paths("images", nullptr, 0) || !b.path("out", "--out")) return badArgs(b.error);
                     for (const auto& [k, f] : std::vector<std::pair<std::string, const char*>>{
                              {"max", "--max"}, {"padding", "--padding"}, {"extrude", "--extrude"}, {"engines", "--engines"}})
                         b.opt(k, f);
                     b.flag("trim", "--trim");
                     b.flag("rotate", "--rotate");
                     b.flag("pot", "--pot");
                     b.overwrite();
                     return fromCli(runCliConfirm(*envPtr, b.args, call));
                 }});
    t.push_back({"sprites_slice", "Slice a sprite sheet",
                 std::string("Cuts a sheet into sprites, by a grid or by detecting separate sprites on a background. "
                             "Writes one PNG per sprite and a .slices.json.") + kPathNote,
                 obj({{"image", str("Sheet PNG")},
                      {"out", str("Output folder")},
                      {"grid", str("\"WxH\" cell size, or \"auto\" to detect a grid; omit to detect sprites")},
                      {"background", str("Background colour #rrggbb (default: transparent or the corner colour)")},
                      {"tolerance", integer("Background colour tolerance (default 8)", 0, 255)},
                      {"merge", integer("Merge parts closer than this many pixels (default 2)", 0, 64)},
                      {"overwrite", kOverwrite}},
                     {"image", "out"}),
                 writes(),
                 [envPtr](const json& a, const ToolCall& call) {
                     ArgBuilder b{*envPtr, a, {"slice"}, {}};
                     if (!b.path("image") || !b.path("out", "--out")) return badArgs(b.error);
                     for (const auto& [k, f] : std::vector<std::pair<std::string, const char*>>{
                              {"grid", "--grid"}, {"background", "--background"}, {"tolerance", "--tolerance"}, {"merge", "--merge"}})
                         b.opt(k, f);
                     b.overwrite();
                     return fromCli(runCliConfirm(*envPtr, b.args, call));
                 }});
    t.push_back({"uikit_generate", "Generate a UI kit",
                 std::string("Draws a themed pixel UI kit (panels, buttons in states, checkboxes, sliders, ...) with 9-slice "
                             "data, plus optional Godot StyleBoxTexture .tres and Unity .meta files.") + kPathNote,
                 obj({{"out", str("Output PNG")},
                      {"scale", integer("Pixel scale 1-8", 1, 8)},
                      {"colors", strings("Six colours #rrggbb: base, light, dark, outline, accent, text")},
                      {"square", boolean("Square corners instead of rounded")},
                      {"engines", kEngines},
                      {"overwrite", kOverwrite}},
                     {"out"}),
                 writes(),
                 [envPtr](const json& a, const ToolCall& call) {
                     ArgBuilder b{*envPtr, a, {"uikit"}, {}};
                     if (!b.path("out", "--out")) return badArgs(b.error);
                     b.opt("scale", "--scale");
                     b.opt("engines", "--engines");
                     if (a.contains("colors")) {
                         std::string joined;
                         for (const auto& c : a["colors"]) joined += (joined.empty() ? "" : ",") + c.get<std::string>();
                         b.args.insert(b.args.end(), {"--colors", joined});
                     }
                     b.flag("square", "--square");
                     b.overwrite();
                     return fromCli(runCliConfirm(*envPtr, b.args, call));
                 }});

    // ---------------------------------------------------------- Lua
    t.push_back({"run_lua", "Run a Lua script",
                 std::string("Runs a sandboxed Lua 5.4 script with the AceStudio `ace` API (images, documents, tilesets, "
                             "atlases, slicing, UI kits; see docs/lua-api.md). The script can read and write files only "
                             "inside the server's folders; relative paths resolve against the first one. print() output is "
                             "returned. With `document`, ace.document() is that project, and `save` keeps its changes.") + kPathNote,
                 obj({{"code", str("Lua source code")},
                      {"args", strings("Strings visible to the script as `arg`")},
                      {"document", str("Optional .acestudio file for ace.document()")},
                      {"save", str("Where to save the document if the script changed it")},
                      {"timeout", integer("Time limit in seconds (default 30)", 1, 3600)},
                      {"overwrite", kOverwrite}},
                     {"code"}),
                 writes(),
                 [envPtr](const json& a, const ToolCall& call) {
                     const ToolEnv& e = *envPtr;
                     if (e.roots.empty()) return badArgs("No folders are allowed (start acestudio-mcp with --root)");
                     const fs::path script = tempFile(".lua");
                     {
                         std::ofstream f(script, std::ios::binary);
                         f << a["code"].get<std::string>();
                         if (!f) return badArgs("Cannot write a temporary script file");
                     }
                     ArgBuilder b{e, a, {"run", utf8(script)}, {}};
                     if (a.contains("args"))
                         for (const auto& s : a["args"]) b.args.push_back(s.get<std::string>());
                     b.args.insert(b.args.end(), {"--base", utf8(e.roots.front())});
                     std::string allow;
                     for (const auto& r : e.roots) allow += (allow.empty() ? "" : ";") + utf8(r);
                     b.args.insert(b.args.end(), {"--allow", allow});
                     if (!b.path("document", "--doc") || !b.path("save", "--save")) {
                         std::error_code ec;
                         fs::remove(script, ec);
                         return badArgs(b.error);
                     }
                     b.opt("timeout", "--timeout");
                     b.overwrite();
                     auto r = runCli(e, b.args, call, std::chrono::seconds(a.value("timeout", 30) + 30));
                     std::error_code ec;
                     fs::remove(script, ec);
                     if (!r) return ToolResult::text(r.error().message, true);
                     const json& res = r.value().result;
                     if (!res.value("ok", false)) {
                         // Script errors name the temp file: show "script:LINE" instead.
                         std::string msg = res.value("error", std::string("failed"));
                         const std::string tmpName = script.filename().string();
                         for (std::size_t pos; (pos = msg.find(tmpName)) != std::string::npos;) msg.replace(pos, tmpName.size(), "script");
                         return ToolResult::text(msg, true);
                     }
                     json data = res;
                     data.erase("ok");
                     std::string text = res.value("output", std::string());
                     if (text.empty()) text = "(no output)";
                     ToolResult t2 = ToolResult::text(text);
                     json summary = data;
                     summary.erase("output");
                     t2.addText(summary.dump(2));
                     t2.structured = data;
                     return t2;
                 }});

    // ---------------------------------------------------------- AI
    t.push_back({"ai_status", "AI runtime status",
                 "Reports the local AI runtime (Python environment, GPU) and which AI tasks and models are installed.",
                 obj(json::object()), readOnly(),
                 [envPtr](const json&, const ToolCall& call) { return fromCli(runCli(*envPtr, {"ai", "status"}, call, 2min)); }});
    t.push_back({"ai_generate_image", "Generate an image with local AI",
                 std::string("Generates an image with the locally installed diffusion model (no cloud). Can snap the result "
                             "to pixel art. Slow: seconds to minutes depending on the GPU.") + kPathNote,
                 obj({{"prompt", str("What to draw")},
                      {"out", str("Output PNG")},
                      {"negative", str("What to avoid")},
                      {"width", integer("Width (default 1024)", 256, 2048)},
                      {"height", integer("Height (default 1024)", 256, 2048)},
                      {"steps", integer("Diffusion steps (default 30)", 1, 150)},
                      {"seed", integer("Seed (-1 = random)")},
                      {"pixel", str("Snap to pixel art of this size, e.g. \"64x64\"")},
                      {"colors", integer("Palette size for pixel snapping", 2, 256)},
                      {"tileable", oneOf("Seamless tiling", {"none", "x", "xy"})},
                      {"lora", str("Installed LoRA id, e.g. pixel-art-xl")},
                      {"model", str("Installed model id (default: the first one)")},
                      {"overwrite", kOverwrite}},
                     {"prompt", "out"}),
                 {{"readOnlyHint", false}, {"destructiveHint", false}, {"idempotentHint", false}, {"openWorldHint", false}},
                 [envPtr](const json& a, const ToolCall& call) {
                     ArgBuilder b{*envPtr, a, {"ai", "generate"}, {}};
                     if (!b.path("out", "--out")) return badArgs(b.error);
                     for (const auto& [k, f] : std::vector<std::pair<std::string, const char*>>{
                              {"prompt", "--prompt"}, {"negative", "--negative"}, {"width", "--width"}, {"height", "--height"},
                              {"steps", "--steps"}, {"seed", "--seed"}, {"pixel", "--pixel"}, {"colors", "--colors"},
                              {"tileable", "--tileable"}, {"lora", "--lora"}, {"model", "--model"}})
                         b.opt(k, f);
                     b.overwrite();
                     ToolResult r = fromCli(runCliConfirm(*envPtr, b.args, call, 60min));
                     if (!r.isError && r.structured && r.structured->contains("path"))
                         if (auto img = pngBase64(fs::path(r.structured->at("path").get<std::string>())); img && img.value().size() < 6'000'000)
                             r.addImage(img.value());
                     return r;
                 }});
    for (const auto& [name, verb, title, what] : std::vector<std::tuple<std::string, std::string, std::string, std::string>>{
             {"ai_remove_background", "remove-bg", "Remove an image background", "Cuts the subject out of an image with the local BiRefNet model; the background becomes transparent."},
             {"ai_upscale", "upscale", "Upscale an image", "Upscales an image 4x with the local Real-ESRGAN model."}}) {
        t.push_back({name, title, what + kPathNote,
                     obj({{"input", str("Input PNG")}, {"out", str("Output PNG")}, {"model", str("Installed model id")}, {"overwrite", kOverwrite}},
                         {"input", "out"}),
                     writes(),
                     [envPtr, verb = verb](const json& a, const ToolCall& call) {
                         ArgBuilder b{*envPtr, a, {"ai", verb}, {}};
                         if (!b.path("input") || !b.path("out", "--out")) return badArgs(b.error);
                         b.opt("model", "--model");
                         b.overwrite();
                         return fromCli(runCliConfirm(*envPtr, b.args, call, 30min));
                     }});
    }

    // ---------------------------------------------------------- live app
    auto bridgeResult = [envPtr](const std::string& method, const json& params, int timeoutMs) -> Result<json> {
        if (!envPtr->bridge) return Error{ErrorCode::NotFound, "No app bridge in this build"};
        return envPtr->bridge(method, params, timeoutMs);
    };
    const json docArg = {{"description", "Which open document: its tab index (0 = first, see app_documents) or its file path. "
                                         "Default: the current tab"}};
    auto docParams = [](const json& a) {
        json p = json::object();
        if (a.contains("document") && !a["document"].is_null()) p["document"] = a["document"];
        return p;
    };
    t.push_back({"app_documents", "List the open documents",
                 "Lists the documents open in the running AceStudio app (tab index, name, file, size, frames, whether it "
                 "has unsaved changes, which one is current).",
                 obj(json::object()), readOnly(),
                 [bridgeResult](const json&, const ToolCall&) {
                     auto r = bridgeResult("documents", json::object(), 10000);
                     if (!r) return ToolResult::text(r.error().message, true);
                     ToolResult t2 = ToolResult::text(r.value().dump(2));
                     t2.structured = r.value();
                     return t2;
                 }});
    t.push_back({"app_document", "Look at an open document",
                 "Describes a document open in the running AceStudio app (size, frames, layers, tags, file) and returns "
                 "its current frame as an image. Fails if AceStudio is not running or has no document open.",
                 obj({{"document", docArg}}), readOnly(),
                 [bridgeResult, docParams](const json& a, const ToolCall&) {
                     auto r = bridgeResult("document", docParams(a), 10000);
                     if (!r) return ToolResult::text(r.error().message, true);
                     json data = r.value();
                     std::string png = data.value("png", std::string());
                     data.erase("png");
                     ToolResult t2 = ToolResult::text(data.dump(2));
                     t2.structured = data;
                     if (!png.empty()) t2.addImage(png);
                     return t2;
                 }});
    t.push_back({"app_run_lua", "Run Lua in the app",
                 "Runs a sandboxed Lua script on a document open in the running AceStudio app (ace.document()). The "
                 "user sees the changes immediately and can take them back with one Edit > Undo. Files: the document's "
                 "folder and the app's scripts folder only.",
                 obj({{"code", str("Lua source code")}, {"args", strings("Strings visible as `arg`")}, {"document", docArg}}, {"code"}),
                 {{"readOnlyHint", false}, {"destructiveHint", false}, {"idempotentHint", false}, {"openWorldHint", false}},
                 [envPtr, bridgeResult, docParams](const json& a, const ToolCall&) {
                     json params = docParams(a);
                     params["code"] = a["code"];
                     params["args"] = a.value("args", json::array());
                     auto r = bridgeResult("run_lua", params, 120000);
                     if (!r) return ToolResult::text(r.error().message, true);
                     json data = r.value();
                     const std::string out = data.value("output", std::string());
                     if (data.contains("error")) {
                         ToolResult t2 = ToolResult::text((out.empty() ? std::string() : out + "\n") + "error: " + data["error"].get<std::string>(), true);
                         t2.structured = data;
                         return t2;
                     }
                     ToolResult t2 = ToolResult::text(out.empty() ? "(no output)" : out);
                     json summary = data;
                     summary.erase("output");
                     t2.addText(summary.dump(2));
                     t2.structured = data;
                     if (data.value("documentChanged", false) && envPtr->resourceChanged) {
                         envPtr->resourceChanged(kAppDocumentUri);
                         envPtr->resourceChanged(kAppFrameUri);
                     }
                     return t2;
                 }});
    t.push_back({"app_open_file", "Open a file in the app",
                 std::string("Opens a project or PNG in the running AceStudio app so the user can see and edit it.") + kPathNote,
                 obj({{"path", str("File to open (.acestudio or .png)")}}, {"path"}),
                 {{"readOnlyHint", true}, {"destructiveHint", false}, {"openWorldHint", false}},
                 [envPtr, bridgeResult](const json& a, const ToolCall&) {
                     auto p = resolveInRoots(envPtr->roots, a["path"].get<std::string>());
                     if (!p) return badArgs(p.error().message);
                     auto r = bridgeResult("open", json{{"path", utf8(p.value())}}, 30000);
                     if (!r) return ToolResult::text(r.error().message, true);
                     ToolResult t2 = ToolResult::text("Opened " + utf8(p.value()));
                     t2.structured = r.value();
                     if (envPtr->resourceChanged) {
                         envPtr->resourceChanged(kAppDocumentUri);
                         envPtr->resourceChanged(kAppFrameUri);
                     }
                     return t2;
                 }});
    return t;
}
} // namespace

namespace {

const std::vector<std::string> kEngineValues{"godot", "unity", "tiled", "godot,unity", "godot,unity,tiled"};

std::vector<std::string> prefixed(const std::vector<std::string>& all, const std::string& value) {
    std::vector<std::string> out;
    for (const auto& v : all)
        if (v.rfind(value, 0) == 0) out.push_back(v);
    return out;
}

json userText(const std::string& text) { return {{"role", "user"}, {"content", textBlock(text)}}; }

} // namespace

const std::string& luaApiReference() {
    static const unsigned char bytes[] = {
#include "LuaApiDoc.inc"
    };
    static const std::string doc(reinterpret_cast<const char*>(bytes), sizeof bytes);
    return doc;
}

std::vector<std::string> completeFiles(const std::vector<fs::path>& roots, const std::string& prefix,
                                       const std::vector<std::string>& extensions, std::size_t max) {
    std::vector<std::string> out;
    if (roots.empty()) return out;
    std::error_code ec;
    fs::recursive_directory_iterator it(roots.front(), fs::directory_options::skip_permission_denied, ec), end;
    for (std::size_t seen = 0; it != end && seen < 20000 && out.size() < max; it.increment(ec), ++seen) {
        if (ec) break;
        if (it.depth() > 4) {
            it.disable_recursion_pending();
            continue;
        }
        const std::string name = it->path().filename().string();
        if (it->is_directory(ec)) {
            if (!name.empty() && (name[0] == '.' || name == "node_modules" || name == "Library")) it.disable_recursion_pending();
            continue;
        }
        std::string ext = it->path().extension().string();
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (std::find(extensions.begin(), extensions.end(), ext) == extensions.end()) continue;
        std::string rel = fs::relative(it->path(), roots.front(), ec).generic_string();
        if (ec || rel.rfind(prefix, 0) != 0) continue;
        out.push_back(rel);
    }
    std::sort(out.begin(), out.end());
    return out;
}

ServerContent aceStudioContent(const ToolEnv& envIn) {
    ServerContent c;
    c.tools = aceStudioTools(envIn);
    // Long-running tools run as tasks for clients with the tasks extension
    // (they poll tasks/get instead of holding a request open for minutes).
    for (auto& tool : c.tools)
        if (tool.name == "ai_generate_image" || tool.name == "ai_remove_background" || tool.name == "ai_upscale" ||
            tool.name == "tileset_generate" || tool.name == "export_animation")
            tool.taskSupport = "optional";
    auto envPtr = std::make_shared<ToolEnv>(envIn);

    // ---------------------------------------------------------- resources
    c.resources.push_back({"acestudio://docs/lua-api", "lua-api", "AceStudio Lua API",
                           "The sandboxed Lua 5.4 `ace` API used by run_lua and app_run_lua: images, documents, tilesets, "
                           "atlases, slicing, UI kits.",
                           "text/markdown",
                           [](const std::string& uri, InputContext&) -> Result<json> {
                               return json::array({{{"uri", uri}, {"mimeType", "text/markdown"}, {"text", luaApiReference()}}});
                           },
                           3600000, true});
    c.resources.push_back({kAppDocumentUri, "app-document", "Open document",
                           "The document in the running AceStudio app's current tab (size, frames, layers, tags, file). "
                           "Subscribe to hear when it changes.",
                           "application/json",
                           [envPtr](const std::string& uri, InputContext&) -> Result<json> {
                               // Not running is a state of this resource, not a missing resource.
                               auto r = envPtr->bridge ? envPtr->bridge("document", json::object(), 10000)
                                                       : Result<json>(Error{ErrorCode::NotFound, "no app bridge"});
                               json d = r ? r.value() : json{{"open", false}, {"appRunning", false}, {"message", r.error().message}};
                               if (r) d["appRunning"] = true;
                               d.erase("png");
                               return json::array({{{"uri", uri}, {"mimeType", "application/json"}, {"text", d.dump(2)}}});
                           },
                           0, false});
    c.resources.push_back({kAppFrameUri, "app-frame", "Open document's current frame",
                           "The current frame of the open document, flattened, as a PNG (scaled up for small canvases).",
                           "image/png",
                           [envPtr](const std::string& uri, InputContext&) -> Result<json> {
                               if (!envPtr->bridge) return Error{ErrorCode::NotFound, "no app bridge"};
                               auto r = envPtr->bridge("document", json::object(), 10000);
                               if (!r) return Error{ErrorCode::NotFound, r.error().message};
                               if (!r.value().value("open", false)) return Error{ErrorCode::NotFound, "no document is open"};
                               return json::array({{{"uri", uri}, {"mimeType", "image/png"}, {"blob", r.value().value("png", std::string())}}});
                           },
                           0, false});

    // ---------------------------------------------------------- templates
    auto projectFiles = [envPtr](const std::string&, const std::string& value, const json&) {
        return completeFiles(envPtr->roots, value, {".acestudio"});
    };
    c.templates.push_back({"acestudio://project/{+path}", "project", "Project",
                           "An .acestudio project under the server's folders: size, colour mode, frames, layers, tags.",
                           "application/json",
                           [envPtr](const std::string& uri, const std::map<std::string, std::string>& vars, InputContext&) -> Result<json> {
                               auto p = resolveInRoots(envPtr->roots, vars.at("path"));
                               if (!p) return Error{ErrorCode::NotFound, p.error().message};
                               std::error_code ec;
                               if (!fs::is_regular_file(p.value(), ec)) return Error{ErrorCode::NotFound, vars.at("path") + " does not exist"};
                               ToolCall none;
                               auto r = runCli(*envPtr, {"info", utf8(p.value())}, none);
                               if (!r) return r.error();
                               if (!r.value().result.value("ok", false))
                                   return Error{ErrorCode::InvalidArgument, r.value().result.value("error", std::string("cannot read the project"))};
                               json d = r.value().result;
                               d.erase("ok");
                               return json::array({{{"uri", uri}, {"mimeType", "application/json"}, {"text", d.dump(2)}}});
                           },
                           projectFiles, 0, false});
    c.templates.push_back({"acestudio://project/{+path}/frame/{frame}.png", "project-frame", "Project frame",
                           "One frame of an .acestudio project, flattened, as a PNG (frame index from 0).",
                           "image/png",
                           [envPtr](const std::string& uri, const std::map<std::string, std::string>& vars, InputContext&) -> Result<json> {
                               auto p = resolveInRoots(envPtr->roots, vars.at("path"));
                               if (!p) return Error{ErrorCode::NotFound, p.error().message};
                               std::error_code ec;
                               if (!fs::is_regular_file(p.value(), ec)) return Error{ErrorCode::NotFound, vars.at("path") + " does not exist"};
                               const std::string frame = vars.at("frame");
                               if (frame.empty() || frame.find_first_not_of("0123456789") != std::string::npos)
                                   return Error{ErrorCode::NotFound, "frame must be a number"};
                               const fs::path out = tempFile(".png");
                               ToolCall none;
                               auto r = runCli(*envPtr, {"render", utf8(p.value()), "--out", utf8(out), "--frame", frame}, none);
                               auto img = (r && r.value().result.value("ok", false)) ? pngBase64(out) : Result<std::string>(Error{ErrorCode::NotFound,
                                   r ? r.value().result.value("error", std::string("cannot render")) : r.error().message});
                               fs::remove(out, ec);
                               if (!img) return img.error();
                               return json::array({{{"uri", uri}, {"mimeType", "image/png"}, {"blob", img.value()}}});
                           },
                           [envPtr](const std::string& name, const std::string& value, const json& ctx) {
                               std::vector<std::string> out;
                               if (name == "path") return completeFiles(envPtr->roots, value, {".acestudio"});
                               if (name == "frame") {
                                   int frames = 1;
                                   if (ctx.contains("path"))
                                       if (auto p = resolveInRoots(envPtr->roots, ctx["path"].get<std::string>())) {
                                           ToolCall none;
                                           if (auto r = runCli(*envPtr, {"info", utf8(p.value())}, none); r && r.value().result.value("ok", false))
                                               frames = r.value().result.value("frames", 1);
                                       }
                                   for (int i = 0; i < std::min(frames, 100); ++i)
                                       if (std::to_string(i).rfind(value, 0) == 0) out.push_back(std::to_string(i));
                               }
                               return out;
                           },
                           0, false});

    // ---------------------------------------------------------- prompts
    auto pngs = [envPtr](const std::string& name, const std::string& value, const json&) -> std::vector<std::string> {
        if (name == "engine" || name == "engines") return prefixed(kEngineValues, value);
        if (name == "scheme") return prefixed({"blob47", "wang16", "cardinal16"}, value);
        return completeFiles(envPtr->roots, value, {".png"});
    };
    c.prompts.push_back({"autotile_terrain", "Autotile terrain for a game engine",
                         "Builds a seamless autotile tileset from texture images and exports it for an engine, then checks it.",
                         {{"base", "Base (ground) texture PNG", true},
                          {"terrains", "Terrain texture PNG(s) drawn over the base, comma separated, lowest first", true},
                          {"engine", "Engine files to write: godot, unity, tiled (comma separated)", false},
                          {"scheme", "blob47 (8-neighbour, default), wang16 (corners / dual grid) or cardinal16 (4-neighbour)", false}},
                         [](const json& a, InputContext&) -> Result<json> {
                             const std::string engine = a.value("engine", std::string("godot"));
                             const std::string scheme = a.value("scheme", std::string("blob47"));
                             const std::string terrains = a["terrains"].get<std::string>();
                             return json{{"description", "Autotile terrain"},
                                         {"messages",
                                          {userText("Make an autotile tileset with AceStudio. Base texture: " + a["base"].get<std::string>() +
                                                    "; terrains (lowest first): " + terrains + "; scheme: " + scheme + "; engines: " + engine +
                                                    ".\n\n1. Call tileset_generate with base, terrains (as a list), scheme, and engines \"" + engine +
                                                    "\". Use tile 16 unless the textures suggest another size.\n2. Call tileset_check_seams on each "
                                                    "level PNG it wrote and report any seams.\n3. List the engine files written and say how to use "
                                                    "them (Godot: open the .tres TileSet and paint with terrains; Unity: drag the RuleTile asset "
                                                    "into a Tile Palette; Tiled: use the Wang set in the .tsx).")}}};
                         },
                         pngs});
    c.prompts.push_back({"export_for_engine", "Export a sprite for a game engine",
                         "Exports an AceStudio project as a sprite sheet with engine files (Godot SpriteFrames, Unity sprites "
                         "and clips) and checks the result.",
                         {{"path", "The .acestudio project", true}, {"engine", "godot, unity or both (comma separated)", true}},
                         [](const json& a, InputContext&) -> Result<json> {
                             return json{{"description", "Export for an engine"},
                                         {"messages",
                                          {userText("Export " + a["path"].get<std::string>() + " for " + a["engine"].get<std::string>() +
                                                    " with AceStudio.\n\n1. Call document_info to see its frames and animation tags.\n"
                                                    "2. Call export_animation with format \"sheet\", layout \"grid\", engines \"" +
                                                    a["engine"].get<std::string>() + "\" and an out path next to the project.\n"
                                                    "3. Call render_frame on frame 0 to show the sprite, and list the files written: one "
                                                    "animation per tag.")}}};
                         },
                         [envPtr](const std::string& name, const std::string& value, const json&) -> std::vector<std::string> {
                             if (name == "engine") return prefixed(kEngineValues, value);
                             return completeFiles(envPtr->roots, value, {".acestudio"});
                         }});
    c.prompts.push_back({"lua_script", "Write an AceStudio Lua script",
                         "Writes and runs a sandboxed Lua script for a task, with the Lua API reference attached.",
                         {{"task", "What the script should do", true},
                          {"target", "\"file\" (run_lua on files) or \"app\" (app_run_lua on the open document); default file", false}},
                         [](const json& a, InputContext&) -> Result<json> {
                             const bool app = a.value("target", std::string("file")) == "app";
                             return json{{"description", "Lua script"},
                                         {"messages",
                                          {{{"role", "user"},
                                            {"content",
                                             {{"type", "resource"},
                                              {"resource", {{"uri", "acestudio://docs/lua-api"}, {"mimeType", "text/markdown"}, {"text", luaApiReference()}}}}}},
                                           userText("Using the AceStudio Lua API above, write a script for this task: " + a["task"].get<std::string>() +
                                                    "\n\nRun it with " + (app ? "app_run_lua (it edits the document open in AceStudio; one undo step)"
                                                                              : "run_lua (files only inside the server's folders)") +
                                                    ". Frames, layers and pixels are 0-based. If it fails, read the error (it has the line "
                                                    "number), fix the script and run it again.")}}};
                         },
                         [](const std::string& name, const std::string& value, const json&) -> std::vector<std::string> {
                             if (name == "target") return prefixed({"file", "app"}, value);
                             return {};
                         }});
    return c;
}

} // namespace acestudio::mcp
