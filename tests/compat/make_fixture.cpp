// Builds a project with an *older* AceStudio core and saves it together with
// that version's own renders of every frame. The files under tests/data/compat
// were produced by compiling this against past commits (see the README there);
// the current build must open them and render the same pixels.
//
//   ERA 1: 0.1.0 (schema 1) layers, groups, blend modes, opacity, cel offsets,
//          frames with durations, tags
//   ERA 2: 0.5.0 (schema 2) + layer masks, clipping, adjustment layers, tile mode
//   ERA 3: 0.8.1 (schema 3) + Levels/Curves, linked cels, palette
#include "acestudio/core/Document.h"
#include "acestudio/core/PngCodec.h"
#include "acestudio/core/ProjectIO.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#ifndef ERA
#error "define ERA"
#endif

using namespace acestudio;

template <typename T> static T must(Result<T> r, const char* what) {
    if (!r) { std::fprintf(stderr, "%s: %s\n", what, r.error().message.c_str()); std::exit(1); }
    return std::move(r).value();
}
static void must(Status s, const char* what) {
    if (!s) { std::fprintf(stderr, "%s: %s\n", what, s.error().message.c_str()); std::exit(1); }
}

static Image pattern(int w, int h, int seed, std::uint8_t alpha) {
    Image img(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if ((x * 3 + y * 5 + seed) % 7 == 0) continue;
            img.setPixel(x, y, Rgba8{static_cast<std::uint8_t>((x * 16 + seed * 40) & 255),
                                     static_cast<std::uint8_t>((y * 16 + seed * 70) & 255),
                                     static_cast<std::uint8_t>((x * y + seed * 30) & 255), alpha});
        }
    return img;
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const std::filesystem::path out = argv[1];
    std::filesystem::create_directories(out);
    const std::string base = "era" + std::to_string(ERA);

    Document d = must(Document::create(32, 24), "create");
    must(d.insertFrame(1, 150), "frame 1");
    must(d.insertFrame(2, 80), "frame 2");
    must(d.setFrameDuration(0, 120), "duration");

    const LayerId bg = must(d.addLayer("Background"), "bg");
    for (int f = 0; f < 3; ++f) must(d.setCel(bg, f, Cel{Image(32, 24, Rgba8{30, 40, 60, 255}), 0, 0}), "bg cel");

    const LayerId group = must(d.addLayer("Group", LayerKind::Group), "group");
    d.layer(group)->opacity = 200;
    const BlendMode modes[] = {BlendMode::Multiply, BlendMode::Screen, BlendMode::Overlay, BlendMode::Darken,
                               BlendMode::Lighten, BlendMode::Add, BlendMode::Subtract, BlendMode::Difference};
    int seed = 1;
    for (BlendMode m : modes) {
        const LayerId l = must(d.addLayer(std::string(toString(m)), LayerKind::Raster, group), "blend layer");
        d.layer(l)->blend = m;
        d.layer(l)->opacity = static_cast<std::uint8_t>(120 + seed * 15);
        for (int f = 0; f < 3; ++f)
            must(d.setCel(l, f, Cel{pattern(12, 10, seed + f, static_cast<std::uint8_t>(150 + seed * 10)), (seed * 3 + f) % 20, (seed * 2) % 14}), "cel");
        ++seed;
    }
    const LayerId hidden = must(d.addLayer("Hidden"), "hidden");
    d.layer(hidden)->visible = false;
    must(d.setCel(hidden, 0, Cel{Image(32, 24, Rgba8{255, 0, 0, 255}), 0, 0}), "hidden cel");

    const LayerId top = must(d.addLayer("Sprite"), "sprite");
    for (int f = 0; f < 3; ++f) must(d.setCel(top, f, Cel{pattern(8, 8, 20 + f, 255), 4 + f * 6, 8}), "sprite cel");

    AnimationTag walk;
    walk.name = "walk";
    walk.fromFrame = 0;
    walk.toFrame = 2;
    walk.direction = PlaybackDirection::PingPong;
    walk.repeat = 2;
    walk.color = "#FF8800";
    must(d.addTag(walk), "tag");

#if ERA >= 2
    // Masked layer, clipped layer, adjustment layers, tile mode.
    const LayerId masked = must(d.addLayer("Masked"), "masked");
    must(d.setCel(masked, 0, Cel{Image(32, 24, Rgba8{200, 220, 90, 255}), 0, 0}), "masked cel");
    d.layer(masked)->hasMask = true;
    {
        Image mask(16, 12, Rgba8{0, 0, 0, 255});
        for (int y = 0; y < 12; ++y)
            for (int x = 0; x < 16; ++x) {
                const auto v = static_cast<std::uint8_t>(x * 16);
                mask.setPixel(x, y, Rgba8{v, v, v, 255});
            }
        must(d.setMaskCel(masked, 0, Cel{mask, 8, 6}), "mask cel");
    }
    const LayerId clip = must(d.addLayer("Clipped"), "clipped");
    d.layer(clip)->clipped = true;
    must(d.setCel(clip, 0, Cel{pattern(32, 24, 9, 255), 0, 0}), "clip cel");

    const LayerId hue = must(d.addLayer("Hue", LayerKind::Adjustment), "hue");
    d.layer(hue)->adjustment = Adjustment{AdjustmentType::HueSaturation, 40, -30, 10};
    d.layer(hue)->opacity = 180;
    const LayerId poster = must(d.addLayer("Posterize", LayerKind::Adjustment, group), "poster");
    d.layer(poster)->adjustment = Adjustment{AdjustmentType::Posterize, 6, 0, 0};
    d.setTileMode(TileMode::Horizontal);
#endif

#if ERA >= 3
    const LayerId levels = must(d.addLayer("Levels", LayerKind::Adjustment), "levels");
    d.layer(levels)->adjustment = Adjustment{AdjustmentType::Levels, 10, 240, 130};
    const LayerId curves = must(d.addLayer("Curves", LayerKind::Adjustment), "curves");
    d.layer(curves)->adjustment = Adjustment(AdjustmentType::Curves, {{0, 10}, {90, 140}, {255, 230}});
    d.layer(curves)->opacity = 160;
    const LayerId linked = must(d.addLayer("Linked"), "linked");
    must(d.setCel(linked, 0, Cel{pattern(10, 10, 33, 230), 20, 2}), "linked cel");
    must(d.linkCels(linked, {0, 1, 2}), "link");
    must(d.palette().add(Rgba8{12, 34, 56, 255}), "palette");
    must(d.palette().add(Rgba8{200, 100, 50, 255}), "palette");
#endif

    must(d.validate(), "validate");
    SaveOptions opts;
    opts.createdUtc = "2026-10-29T00:00:00Z";
    must(saveProject(d, out / (base + ".acestudio"), opts), "save");
    for (int f = 0; f < d.frameCount(); ++f)
        must(writePng(d.composite(f), out / (base + "_frame" + std::to_string(f) + ".png")), "render");
    std::printf("%s: %zu layers, %d frames\n", base.c_str(), d.layers().size(), d.frameCount());
    return 0;
}
