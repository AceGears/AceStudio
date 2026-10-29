// Micro-benchmarks for interactive editing paths. Prints median timings;
// results depend on the machine, so record where they were measured.
#include "acestudio/core/Animation.h"
#include "acestudio/core/Paint.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <vector>

using namespace acestudio;
using Clock = std::chrono::steady_clock;

static double medianMs(int runs, const std::function<void()>& fn) {
    std::vector<double> t;
    for (int i = 0; i < runs; ++i) {
        const auto a = Clock::now();
        fn();
        t.push_back(std::chrono::duration<double, std::milli>(Clock::now() - a).count());
    }
    std::sort(t.begin(), t.end());
    return t[t.size() / 2];
}

static Editor makeDoc(int size, int layers) {
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
    return Editor(std::move(d));
}

int main() {
    std::printf("%-44s %10s\n", "benchmark", "median ms");
    for (auto [size, layers] : {std::pair{256, 20}, std::pair{1024, 20}}) {
        Editor e = makeDoc(size, layers);
        char name[64];
        std::snprintf(name, sizeof name, "composite %dx%d, %d layers", size, size, layers);
        std::printf("%-44s %10.2f\n", name, medianMs(15, [&] { volatile auto img = e.document().composite(0); (void)img; }));

        Image canvas = e.document().composite(0);
        std::snprintf(name, sizeof name, "composite 32x32 region of %dx%d", size, size);
        std::printf("%-44s %10.2f\n", name, medianMs(15, [&] { e.document().compositeRegion(0, {100, 100, 32, 32}, canvas); }));

        const LayerId top = e.document().layers().back().id;
        std::snprintf(name, sizeof name, "stroke 200px + commit on %dx%d", size, size);
        std::printf("%-44s %10.2f\n", name, medianMs(9, [&] {
            auto s = PaintSession::begin(e, top, 0, "bench").value();
            FreehandStroke st(*s, Rgba8::opaque(255, 0, 0), false, true);
            for (int i = 0; i < 200; ++i) st.moveTo({i % size, (i * 7) % size});
            s->commit();
        }));
        {
            // Onion skin, 1 ghost each side: two extra full composites per frame change.
            Editor anim = makeDoc(size, layers);
            (void)anim.duplicateFrame(0);   // real content in every ghost
            (void)anim.duplicateFrame(0);
            OnionSkin o;
            std::snprintf(name, sizeof name, "onion skin 1+1 ghosts on %dx%d", size, size);
            std::printf("%-44s %10.2f\n", name, medianMs(9, [&] { volatile auto img = renderOnionSkin(anim.document(), 1, o); (void)img; }));
        }
        std::snprintf(name, sizeof name, "undo+redo paint step on %dx%d", size, size);
        std::printf("%-44s %10.2f\n", name, medianMs(9, [&] { (void)e.undo(); (void)e.redo(); }));
    }
    return 0;
}
