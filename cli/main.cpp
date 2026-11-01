// acestudio-cli — headless access to the AceStudio core.
//
// Every command supports --json for machine-readable output. Exit codes:
//   0 success, 1 usage error, 2 I/O or not found, 3 corrupt or invalid data,
//   4 unsupported (newer) version, 5 internal error.

#include "CliCommon.h"
#include "acestudio/core/AssetFiles.h"
#include "acestudio/core/Document.h"
#include "acestudio/core/Export.h"
#include "acestudio/core/FileUtil.h"
#include "acestudio/core/Log.h"
#include "acestudio/core/PngCodec.h"
#include "acestudio/core/ProjectIO.h"
#include "acestudio/core/Version.h"

#include <nlohmann/json.hpp>

#include <charconv>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using nlohmann::json;
using namespace acestudio;

namespace {

using namespace acestudio::cli;

const char* kUsageText =
    "Usage: acestudio-cli [--json] <command> [options]\n"
    "\n"
    "Commands:\n"
    "  version                         Print version information\n"
    "  new <file> --width W --height H [--frames N] [--color-mode rgba|indexed|grayscale] [--force]\n"
    "                                  Create an empty project\n"
    "  info <file>                     Describe a project\n"
    "  validate <file>                 Check a project for corruption (exit 3 if invalid)\n"
    "  render <file> --out <png> [--frame N] [--scale K] [--force]\n"
    "                                  Flatten one frame to PNG (nearest-neighbour scale)\n"
    "  export <file> --format sheet|gif|apng|webp|png --out <path> [options] [--force]\n"
    "                                  Export frames. Options: --tag NAME | --from N --to M,\n"
    "                                  --scale K, --loop N (0 = forever),\n"
    "                                  sheet: --layout horizontal|vertical|grid|packed --columns N\n"
    "                                         --padding N --spacing N --trim --merge-duplicates --pot\n"
    "                                         --data hash|array (writes <out>.json beside the PNG)\n"
    "                                  png:   --out is a folder; writes <name>_NNN.png per frame\n"
    "                                  --engines godot,unity: with sheet, also a Godot SpriteFrames .tres\n"
    "                                  (one animation per tag) and Unity .meta + .anim files\n"
    "\n"
    "  tileset --base B.png --terrain T1.png[,T2.png,T3.png] --out tiles.png [options]\n"
    "                                  Autotile set (terrain levels over a base), seam-checked.\n"
    "                                  --scheme blob47|wang16|cardinal16 --tile 16 --phases 1..4\n"
    "                                  --roundness 1 --noise 0.3 --noise-scale 4 --seed 1 --no-outline\n"
    "                                  --names base,level1,... --engines godot,unity,tiled --ppu N\n"
    "  seams <tiles.png> --scheme S [--phases N] [--base base.png]\n"
    "                                  Seam check of a tileset (exit 3 when there are seams)\n"
    "  tileset-template --scheme S --tile N --out t.png\n"
    "                                  Guide image for drawing a tileset by hand\n"
    "  atlas <a.png> <b.png> ... --out atlas.png [--max 4096] [--padding 1] [--border 0]\n"
    "        [--extrude 0] [--trim] [--pot] [--rotate] [--no-merge] [--engines unity] [--ppu N]\n"
    "                                  Deterministic atlas + TexturePacker-style JSON\n"
    "  slice <sheet.png> --out <folder> [--grid WxH|auto [--offset X,Y] [--spacing X,Y] [--keep-empty]]\n"
    "        [--background #rrggbb] [--tolerance 8] [--merge 2] [--min-pixels 4]\n"
    "                                  Cut a sheet into sprites (PNGs + .slices.json)\n"
    "  uikit --out ui.png [--scale 1] [--colors base,light,dark,outline,accent,text] [--square]\n"
    "        [--texture pattern.png] [--engines godot,unity]\n"
    "                                  Themed UI kit with 9-slice data (Godot StyleBoxTexture .tres)\n"
    "\n"
    "  ai status                       Show the AI runtime, GPU and available tasks\n"
    "  ai setup --profile P --yes      Create the AI Python environment and install packages\n"
    "                                  P: onnx-windows | onnx-cpu | diffusion-cuda | diffusion-cpu\n"
    "  ai models                       List downloadable models, licences and install state\n"
    "  ai download <id> --accept-license <licence-id>\n"
    "  ai remove-bg <in.png> --out <png> [--model id] [--threshold 0.5] [--cpu]\n"
    "  ai upscale <in.png> --out <png> [--model id] [--tile 192] [--cpu]\n"
    "  ai generate --prompt TEXT --out <png> [--negative TEXT] [--width W --height H] [--steps N]\n"
    "              [--guidance G] [--seed S] [--lora pixel-art-xl] [--pixel 64x64 [--colors 16]]\n"
    "              [--init in.png --strength 0.6] [--tileable x|xy] [--cpu]\n"
    "\n"
    "Existing files are never overwritten unless --force is given.\n";

std::optional<Args> parseArgs(int argc, char** argv, std::string& err) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if (s == "--json") a.json = true;
        else if (s == "--force") a.force = true;
        else if (s == "--trim" || s == "--merge-duplicates" || s == "--pot" || s == "--yes" || s == "--cpu" || s == "--rotate" ||
                 s == "--no-outline" || s == "--no-merge" || s == "--square" || s == "--keep-empty")
            a.flags.insert(s.substr(2));
        else if (s == "-h" || s == "--help") { a.positional = {"help"}; return a; }
        else if (s.rfind("--", 0) == 0) {
            if (i + 1 >= argc) { err = "Missing value for " + s; return std::nullopt; }
            a.options[s.substr(2)] = argv[++i];
        } else a.positional.push_back(std::move(s));
    }
    return a;
}

json summarize(const Document& d, const ProjectInfo& info) {
    json layers = json::array();
    for (const auto& l : d.layers())
        layers.push_back({{"id", l.id}, {"name", l.name}, {"kind", l.kind == LayerKind::Group ? "group" : "raster"},
                          {"visible", l.visible}, {"opacity", l.opacity}, {"blend", toString(l.blend)}});
    json tags = json::array();
    for (const auto& t : d.tags()) tags.push_back({{"name", t.name}, {"from", t.fromFrame}, {"to", t.toFrame}});
    int totalMs = 0;
    for (const auto& f : d.frames()) totalMs += f.durationMs;
    return {{"width", d.width()}, {"height", d.height()}, {"colorMode", toString(d.colorMode())},
            {"frames", d.frameCount()}, {"durationMs", totalMs}, {"layers", layers},
            {"cels", d.cels().size()}, {"tags", tags}, {"paletteColors", d.palette().size()},
            {"schemaVersion", info.schemaVersion}, {"writerVersion", info.writerVersion},
            {"createdUtc", info.createdUtc}, {"modifiedUtc", info.modifiedUtc}};
}

int cmdVersion(const Args& a, Output& out) {
    (void)a;
    return out.success({{"name", kAppName}, {"version", kVersionString}, {"projectSchema", kProjectSchemaVersion}},
                       std::string(kAppName) + " " + kVersionString + " (project schema " +
                           std::to_string(kProjectSchemaVersion) + ")\n");
}

int cmdNew(const Args& a, Output& out) {
    if (a.positional.size() != 2) return out.fail(kUsage, "new: expected exactly one output file");
    const fs::path path = a.positional[1];
    const auto w = optInt(a, "width");
    const auto h = optInt(a, "height");
    if (!w || !h) return out.fail(kUsage, "new: --width and --height are required integers");
    const int frames = optInt(a, "frames").value_or(1);
    if (frames < 1 || frames > Document::kMaxFrames) return out.fail(kUsage, "new: --frames out of range");
    auto mode = colorModeFromString(a.opt("color-mode").value_or("rgba"));
    if (!mode) return out.fail(kUsage, "new: unknown --color-mode");

    std::error_code ec;
    if (fs::exists(path, ec) && !a.force)
        return out.fail(kIo, path.string() + " already exists (use --force to replace it; a .bak copy is kept)");

    auto doc = Document::create(*w, *h, *mode);
    if (!doc) return out.fail(doc.error());
    auto layer = doc.value().addLayer("Layer 1");
    if (!layer) return out.fail(layer.error());
    for (int i = 1; i < frames; ++i)
        if (auto r = doc.value().insertFrame(i); !r) return out.fail(r.error());
    if (auto st = saveProject(doc.value(), path); !st) return out.fail(st.error());
    return out.success({{"path", path.string()}, {"width", *w}, {"height", *h}, {"frames", frames}},
                       "Created " + path.string() + " (" + std::to_string(*w) + "x" + std::to_string(*h) + ", " +
                           std::to_string(frames) + " frame" + (frames == 1 ? "" : "s") + ")\n");
}

int cmdInfo(const Args& a, Output& out, bool validateOnly) {
    if (a.positional.size() != 2) return out.fail(kUsage, std::string(validateOnly ? "validate" : "info") + ": expected one file");
    ProjectInfo info;
    auto doc = loadProject(a.positional[1], &info);
    if (!doc) return out.fail(doc.error());
    if (validateOnly)
        return out.success({{"path", a.positional[1]}, {"valid", true}}, a.positional[1] + ": OK\n");

    json s = summarize(doc.value(), info);
    std::string human = a.positional[1] + "\n" +
        "  Canvas:  " + std::to_string(doc.value().width()) + "x" + std::to_string(doc.value().height()) +
        " (" + std::string(toString(doc.value().colorMode())) + ")\n" +
        "  Frames:  " + std::to_string(doc.value().frameCount()) + " (" + std::to_string(s["durationMs"].get<int>()) + " ms)\n" +
        "  Layers:  " + std::to_string(doc.value().layers().size()) + "\n" +
        "  Tags:    " + std::to_string(doc.value().tags().size()) + "\n" +
        "  Schema:  " + std::to_string(info.schemaVersion) + " (written by " + info.writerVersion + ")\n";
    return out.success(std::move(s), human);
}

int cmdRender(const Args& a, Output& out) {
    if (a.positional.size() != 2) return out.fail(kUsage, "render: expected one project file");
    const auto outPath = a.opt("out");
    if (!outPath) return out.fail(kUsage, "render: --out is required");
    const int frame = optInt(a, "frame").value_or(0);
    const int scale = optInt(a, "scale").value_or(1);
    if (scale < 1 || scale > 64) return out.fail(kUsage, "render: --scale must be 1..64");

    auto doc = loadProject(a.positional[1]);
    if (!doc) return out.fail(doc.error());
    if (frame < 0 || frame >= doc.value().frameCount())
        return out.fail(kUsage, "render: frame " + std::to_string(frame) + " does not exist");
    std::error_code ec;
    if (fs::exists(*outPath, ec) && !a.force) return out.fail(kIo, *outPath + " already exists (use --force)");

    Image flat = doc.value().composite(frame);
    if (scale > 1) {
        auto scaled = Image::create(flat.width() * scale, flat.height() * scale);
        if (!scaled) return out.fail(scaled.error());
        for (int y = 0; y < scaled.value().height(); ++y)
            for (int x = 0; x < scaled.value().width(); ++x)
                scaled.value().setPixel(x, y, flat.pixel(x / scale, y / scale));
        flat = std::move(scaled).value();
    }
    if (auto st = writePng(flat, *outPath); !st) return out.fail(st.error());
    return out.success({{"path", *outPath}, {"width", flat.width()}, {"height", flat.height()}, {"frame", frame}},
                       "Rendered frame " + std::to_string(frame) + " to " + *outPath + "\n");
}

int cmdExport(const Args& a, Output& out) {
    if (a.positional.size() != 2) return out.fail(kUsage, "export: expected one project file");
    const auto format = a.opt("format");
    const auto outPath = a.opt("out");
    if (!format || !outPath) return out.fail(kUsage, "export: --format and --out are required");
    auto doc = loadProject(a.positional[1]);
    if (!doc) return out.fail(doc.error());

    FrameRange range;
    range.tag = a.opt("tag").value_or("");
    range.from = optInt(a, "from").value_or(0);
    range.to = optInt(a, "to").value_or(-1);
    const int scale = optInt(a, "scale").value_or(1);
    const int loop = optInt(a, "loop").value_or(0);
    std::error_code ec;
    const fs::path target = *outPath;


    if (*format == "sheet") {
        SheetOptions o;
        const std::string layout = a.opt("layout").value_or("horizontal");
        if (layout == "horizontal") o.layout = SheetLayout::Horizontal;
        else if (layout == "vertical") o.layout = SheetLayout::Vertical;
        else if (layout == "grid") o.layout = SheetLayout::Grid;
        else if (layout == "packed") o.layout = SheetLayout::Packed;
        else return out.fail(kUsage, "export: unknown --layout " + layout);
        o.columns = optInt(a, "columns").value_or(0);
        o.innerPadding = optInt(a, "padding").value_or(0);
        o.spacing = optInt(a, "spacing").value_or(0);
        o.trim = a.flags.count("trim") > 0;
        o.mergeDuplicates = a.flags.count("merge-duplicates") > 0;
        o.powerOfTwo = a.flags.count("pot") > 0;
        o.scale = scale;
        const std::string style = a.opt("data").value_or("hash");
        if (style != "hash" && style != "array") return out.fail(kUsage, "export: --data must be hash or array");
        if (auto eng = a.opt("engines")) {
            auto targets = parseEngineTargets(*eng);
            if (!targets) return out.fail(targets.error());
            AssetWriteOptions wo;
            wo.engines = targets.value();
            wo.overwrite = a.force;
            wo.pixelsPerUnit = optInt(a, "ppu").value_or(0);
            auto w = writeSheetFiles(doc.value(), range, o, target, wo);
            if (!w) return out.fail(w.error());
            json files = json::array(), notes = json::array();
            std::string human = "Wrote " + std::to_string(w.value().files.size()) + " file(s):\n";
            for (const auto& f : w.value().files) { files.push_back(f.string()); human += "  " + f.string() + "\n"; }
            for (const auto& n : w.value().notes) { notes.push_back(n); human += "Note: " + n + "\n"; }
            return out.success({{"files", files}, {"notes", notes}}, human);
        }
        auto files = writeSpriteSheet(doc.value(), range, o, target, style == "hash" ? JsonStyle::Hash : JsonStyle::Array, a.force);
        if (!files) return out.fail(files.error());
        const SheetFiles& f = files.value();
        return out.success({{"image", f.image.string()}, {"data", f.data.string()}, {"frames", f.frames},
                            {"width", f.width}, {"height", f.height}},
                           "Wrote " + f.image.string() + " and " + f.data.string() + " (" + std::to_string(f.frames) + " frames)\n");
    }
    if (*format == "png") {
        auto paths = exportPngSequence(doc.value(), range, target, fs::path(a.positional[1]).stem().string(), scale, a.force);
        if (!paths) return out.fail(paths.error());
        json list = json::array();
        for (const auto& p : paths.value()) list.push_back(p.string());
        return out.success({{"files", list}}, "Wrote " + std::to_string(paths.value().size()) + " PNG files to " + target.string() + "\n");
    }
    if (*format == "gif" || *format == "apng" || *format == "webp") {
        const AnimFormat fmt = *format == "gif" ? AnimFormat::Gif : *format == "apng" ? AnimFormat::Apng : AnimFormat::Webp;
        auto n = writeAnimation(doc.value(), range, fmt, target, scale, loop, a.force);
        if (!n) return out.fail(n.error());
        std::uintmax_t size = fs::file_size(target, ec);
        return out.success({{"path", target.string()}, {"frames", n.value()}, {"bytes", ec ? 0 : size}},
                           "Wrote " + target.string() + " (" + std::to_string(n.value()) + " frames)\n");
    }
    return out.fail(kUsage, "export: unknown --format " + *format);
}

} // namespace

int main(int argc, char** argv) {
    std::string err;
    auto args = parseArgs(argc, argv, err);
    Output out(args && args->json);
    if (!args) return out.fail(kUsage, err);
    if (args->positional.empty() || args->positional[0] == "help") {
        std::cout << kUsageText;
        return args->positional.empty() ? kUsage : kOk;
    }

    LogConfig cfg;
    cfg.minLevel = LogLevel::Warning;
    cfg.toStderr = !args->json;   // keep stdout clean for JSON consumers
    Logger::instance().configure(cfg);

    try {
        const std::string& cmd = args->positional[0];
        if (cmd == "version") return cmdVersion(*args, out);
        if (cmd == "new") return cmdNew(*args, out);
        if (cmd == "info") return cmdInfo(*args, out, false);
        if (cmd == "validate") return cmdInfo(*args, out, true);
        if (cmd == "render") return cmdRender(*args, out);
        if (cmd == "export") return cmdExport(*args, out);
        if (cmd == "ai") return acestudio::cli::cmdAi(*args, out);
        if (cmd == "tileset") return acestudio::cli::cmdTileset(*args, out);
        if (cmd == "seams") return acestudio::cli::cmdSeams(*args, out);
        if (cmd == "tileset-template") return acestudio::cli::cmdTemplate(*args, out);
        if (cmd == "atlas") return acestudio::cli::cmdAtlas(*args, out);
        if (cmd == "slice") return acestudio::cli::cmdSlice(*args, out);
        if (cmd == "uikit") return acestudio::cli::cmdUiKit(*args, out);
        return out.fail(kUsage, "Unknown command '" + cmd + "'. Run with --help.");
    } catch (const std::exception& e) {
        return out.fail(kInternal, std::string("Unexpected error: ") + e.what());
    }
}
