// Benchmarks for interactive editing, file and export paths. Prints median
// timings; results depend on the machine, so record where they were measured.
//
//   acestudio_bench                       print the table
//   acestudio_bench --json out.json       also write {name: median ms}
//   acestudio_bench --budget budgets.json fail (exit 1) when a median is over
//                                         its budget; budgets are generous
//                                         ceilings that catch regressions of
//                                         several times, not small drift
#include "acestudio/core/AnimCodec.h"
#include "acestudio/core/Animation.h"
#include "acestudio/core/Export.h"
#include "acestudio/core/Paint.h"
#include "acestudio/core/PngCodec.h"
#include "acestudio/core/ProjectIO.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace acestudio;
using Clock = std::chrono::steady_clock;

namespace {

struct Row { std::string name; double ms; };
std::vector<Row> g_rows;

double medianMs(int runs, const std::function<void()>& fn) {
    std::vector<double> t;
    for (int i = 0; i < runs; ++i) {
        const auto a = Clock::now();
        fn();
        t.push_back(std::chrono::duration<double, std::milli>(Clock::now() - a).count());
    }
    std::sort(t.begin(), t.end());
    return t[t.size() / 2];
}

void record(const std::string& name, int runs, const std::function<void()>& fn) {
    const double ms = medianMs(runs, fn);
    std::printf("%-46s %10.2f\n", name.c_str(), ms);
    std::fflush(stdout);
    g_rows.push_back({name, ms});
}

std::string sized(const char* what, int size) {
    return std::string(what) + " " + std::to_string(size) + "x" + std::to_string(size);
}

Editor makeDoc(int size, int layers, int frames = 1) {
    auto d = Document::create(size, size).value();
    for (int i = 0; i < layers; ++i) {
        const auto id = d.addLayer("L" + std::to_string(i)).value();
        Image img(size, size);
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
                if ((x + y + i) % 3 == 0) img.setPixel(x, y, Rgba8{static_cast<std::uint8_t>(i * 12), 80, 160, 200});
        (void)d.setCel(id, 0, Cel{std::move(img), 0, 0});
        if (i % 4 == 1) d.layer(id)->blend = BlendMode::Multiply;
    }
    Editor e(std::move(d));
    for (int f = 1; f < frames; ++f) (void)e.duplicateFrame(0);
    return e;
}

// A pixel-art-like frame: flat colour regions, a few dozen colours.
Image spriteFrame(int size, int f) {
    Image img(size, size);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const int c = ((x + f) / 8 + y / 8) % 24;
            if ((x * 7 + y * 3 + f) % 11 == 0) continue;   // some transparency
            img.setPixel(x, y, Rgba8{static_cast<std::uint8_t>(c * 10), static_cast<std::uint8_t>(255 - c * 9),
                                     static_cast<std::uint8_t>((c * 37) & 255), 255});
        }
    return img;
}

void editingBenchmarks() {
    for (auto [size, layers] : {std::pair{256, 20}, std::pair{1024, 20}}) {
        Editor e = makeDoc(size, layers);
        record("composite " + std::to_string(size) + "x" + std::to_string(size) + ", 20 layers", 15,
               [&] { volatile auto img = e.document().composite(0); (void)img; });

        Image canvas = e.document().composite(0);
        record(sized("composite 32x32 region of", size), 15,
               [&] { e.document().compositeRegion(0, {100, 100, 32, 32}, canvas); });

        const LayerId top = e.document().layers().back().id;
        record(sized("stroke 200px + commit on", size), 9, [&] {
            auto s = PaintSession::begin(e, top, 0, "bench").value();
            FreehandStroke st(*s, Rgba8::opaque(255, 0, 0), false, true);
            for (int i = 0; i < 200; ++i) st.moveTo({i % size, (i * 7) % size});
            s->commit();
        });
        {
            // Onion skin without the app's composite cache: two extra full composites.
            Editor anim = makeDoc(size, layers, 3);
            OnionSkin o;
            record(sized("onion skin 1+1 ghosts (uncached) on", size), 9,
                   [&] { volatile auto img = renderOnionSkin(anim.document(), 1, o); (void)img; });
        }
        record(sized("undo+redo paint step on", size), 9, [&] { (void)e.undo(); (void)e.redo(); });
    }
}

void fileBenchmarks() {
    // A large project: 1024x1024, 20 layers, 8 frames (160 cels).
    Editor e = makeDoc(1024, 20, 8);
    std::vector<std::uint8_t> bytes;
    record("project save 1024x1024, 20 layers x 8 frames", 5,
           [&] { bytes = serializeProject(e.document()).value(); });
    std::printf("%-46s %10.1f MB\n", "  (project size)", static_cast<double>(bytes.size()) / 1e6);
    record("project load 1024x1024, 20 layers x 8 frames", 5,
           [&] { volatile auto d = deserializeProject(bytes); (void)d; });
    // What autosave costs the UI thread: the snapshot copy (the save itself
    // runs in the background).
    record("autosave snapshot 1024x1024, 20 layers x 8 frames", 5,
           [&] { volatile auto d = std::make_shared<Document>(e.document()); (void)d; });

    const Image flat = e.document().composite(0);
    std::vector<std::uint8_t> png;
    record("PNG encode 1024x1024", 9, [&] { png = encodePng(flat).value(); });
    record("PNG decode 1024x1024", 9, [&] { volatile auto i = decodePng(png); (void)i; });
}

void exportBenchmarks() {
    std::vector<AnimFrame> frames;
    for (int f = 0; f < 16; ++f) frames.push_back({spriteFrame(256, f), 100});
    record("GIF encode 16 frames 256x256", 3, [&] { volatile auto b = encodeGif(frames); (void)b; });
    record("APNG encode 16 frames 256x256", 3, [&] { volatile auto b = encodeApng(frames); (void)b; });
    record("WebP encode 16 frames 256x256", 3, [&] { volatile auto b = encodeWebp(frames); (void)b; });
    const auto gif = encodeGif(frames).value();
    record("GIF decode 16 frames 256x256", 5, [&] { volatile auto a = decodeGif(gif); (void)a; });

    auto d = Document::create(64, 64).value();
    const auto layer = d.addLayer("sprite").value();
    Editor e(std::move(d));
    for (int f = 1; f < 64; ++f) (void)e.duplicateFrame(0);
    for (int f = 0; f < 64; ++f) (void)e.setCelImage(layer, f, spriteFrame(64, f % 16));
    SheetOptions packed;
    packed.layout = SheetLayout::Packed;
    packed.trim = true;
    packed.mergeDuplicates = true;
    record("sprite sheet, 64 frames 64x64, packed+trim", 5,
           [&] { volatile auto s = buildSpriteSheet(e.document(), FrameRange{}, packed); (void)s; });
}

} // namespace

int main(int argc, char** argv) {
    const char* jsonOut = nullptr;
    const char* budgetFile = nullptr;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--json") == 0) jsonOut = argv[++i];
        else if (std::strcmp(argv[i], "--budget") == 0) budgetFile = argv[++i];
    }
    std::printf("%-46s %10s\n", "benchmark", "median ms");
    editingBenchmarks();
    fileBenchmarks();
    exportBenchmarks();

    if (jsonOut) {
        nlohmann::json j = nlohmann::json::object();
        for (const auto& r : g_rows) j[r.name] = r.ms;
        std::ofstream(jsonOut) << j.dump(2) << '\n';
    }
    if (!budgetFile) return 0;
    std::ifstream in(budgetFile);
    const auto budgets = nlohmann::json::parse(in, nullptr, false);
    if (budgets.is_discarded() || !budgets.is_object()) {
        std::fprintf(stderr, "Cannot read budgets from %s\n", budgetFile);
        return 2;
    }
    int over = 0, checked = 0, unknown = 0;
    for (const auto& r : g_rows) {
        if (!budgets.contains(r.name)) continue;
        ++checked;
        const double limit = budgets[r.name].get<double>();
        if (r.ms > limit) {
            std::fprintf(stderr, "OVER BUDGET: %s took %.2f ms (budget %.2f ms)\n", r.name.c_str(), r.ms, limit);
            ++over;
        }
    }
    for (auto it = budgets.begin(); it != budgets.end(); ++it) {
        const bool known = std::any_of(g_rows.begin(), g_rows.end(), [&](const Row& r) { return r.name == it.key(); });
        if (!known && it.key().rfind("_", 0) != 0) {
            std::fprintf(stderr, "Budget for an unknown benchmark: %s\n", it.key().c_str());
            ++unknown;
        }
    }
    std::printf("perf budget: %d of %d within budget%s\n", checked - over, checked,
                unknown ? ", and budgets for unknown benchmarks" : "");
    return over || unknown ? 1 : 0;
}
