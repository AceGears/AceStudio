#include <catch2/catch_test_macros.hpp>

#include "TestSupport.h"
#include "acestudio/core/Export.h"
#include "acestudio/core/FileUtil.h"
#include "acestudio/core/PngCodec.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <random>

using namespace acestudio;
namespace fs = std::filesystem;

namespace {
// 8×6 sprite, 4 frames: a 2×2 block moving right; frame 3 duplicates frame 0.
Document sampleAnim() {
    auto d = Document::create(8, 6).value();
    const auto l = d.addLayer("body").value();
    for (int f = 1; f < 4; ++f) REQUIRE(d.insertFrame(f, 50 + f * 25));
    const int xs[4] = {1, 3, 5, 1};
    for (int f = 0; f < 4; ++f) {
        Image img(2, 2, Rgba8::opaque(200, 40, 40));
        img.setPixel(1, 1, Rgba8::opaque(20, 20, 220));
        REQUIRE(d.setCel(l, f, Cel{img, xs[f], 2}));
    }
    REQUIRE(d.addTag({"walk", 0, 2, PlaybackDirection::PingPong, 0, "#00FF00"}));
    REQUIRE(d.addTag({"last", 3, 3, PlaybackDirection::Forward, 2, "#0000FF"}));
    return d;
}

bool regionEquals(const Image& sheet, const IntRect& r, const Image& expected) {
    return sheet.copy(r) == expected;
}

std::vector<AnimFrame> framesOf(const Document& d) { return renderFrames(d, {}).value(); }
} // namespace

TEST_CASE("Frame ranges and playback order", "[export]") {
    const Document d = sampleAnim();
    CHECK(resolveFrames(d, {}).value().size() == 4);
    CHECK(resolveFrames(d, {"walk", 0, -1}).value() == std::vector<int>{0, 1, 2});
    CHECK(resolveFrames(d, {"", 1, 2}).value() == std::vector<int>{1, 2});
    CHECK(resolveFrames(d, {"nope", 0, -1}).error().code == ErrorCode::NotFound);
    CHECK_FALSE(resolveFrames(d, {"", 2, 9}));
    auto pp = renderFrames(d, {"walk", 0, -1}, 1, true);
    REQUIRE(pp);
    CHECK(pp.value().size() == 4);                       // 0,1,2,1
    CHECK(pp.value()[3].image == d.composite(1));
    auto scaled = renderFrames(d, {}, 3);
    REQUIRE(scaled);
    CHECK(scaled.value()[0].image.width() == 24);
    CHECK(scaled.value()[0].image.pixel(5, 8) == Rgba8::opaque(200, 40, 40));
}

TEST_CASE("Sprite sheet layouts place every frame exactly", "[export][sheet]") {
    const Document d = sampleAnim();
    SECTION("horizontal with spacing and border") {
        SheetOptions o;
        o.spacing = 2;
        o.borderPadding = 1;
        auto s = buildSpriteSheet(d, {}, o);
        REQUIRE(s);
        CHECK(s.value().image.width() == 1 + 4 * 8 + 3 * 2 + 1);
        CHECK(s.value().image.height() == 1 + 6 + 1);
        for (const auto& f : s.value().frames) CHECK(regionEquals(s.value().image, f.rect, d.composite(f.frame)));
        CHECK(s.value().frames[1].rect.x == 1 + 8 + 2);
    }
    SECTION("grid with automatic columns, vertical") {
        SheetOptions o;
        o.layout = SheetLayout::Grid;
        auto g = buildSpriteSheet(d, {}, o).value();
        CHECK(g.image.width() == 16);
        CHECK(g.image.height() == 12);
        o.layout = SheetLayout::Vertical;
        auto v = buildSpriteSheet(d, {}, o).value();
        CHECK(v.image.height() == 24);
    }
    SECTION("trim, merge duplicates, packed, power of two") {
        SheetOptions o;
        o.layout = SheetLayout::Packed;
        o.trim = true;
        o.mergeDuplicates = true;
        o.powerOfTwo = true;
        auto s = buildSpriteSheet(d, {}, o);
        REQUIRE(s);
        const auto& fr = s.value().frames;
        REQUIRE(fr.size() == 4);
        CHECK(fr[0].trimmed);
        CHECK(fr[0].rect.w == 2);
        CHECK(fr[0].source == IntRect{1, 2, 2, 2});
        CHECK(fr[3].rect == fr[0].rect);                 // duplicate shares the rect
        CHECK((s.value().image.width() & (s.value().image.width() - 1)) == 0);
        for (std::size_t i = 0; i < fr.size(); ++i)
            for (std::size_t j = i + 1; j < fr.size(); ++j)
                if (!(fr[j].rect == fr[i].rect)) CHECK(fr[i].rect.intersected(fr[j].rect).empty());
        for (const auto& f : fr) CHECK(regionEquals(s.value().image, f.rect, d.composite(f.frame).copy(f.source)));
    }
}

TEST_CASE("Aseprite JSON metadata", "[export][json]") {
    const Document d = sampleAnim();
    auto sheet = buildSpriteSheet(d, {}, {}).value();
    const auto hash = nlohmann::json::parse(spriteSheetJson(d, sheet, "hero.png", "hero", JsonStyle::Hash));
    CHECK(hash["frames"].size() == 4);
    CHECK(hash["frames"]["hero 1"]["frame"]["x"] == 8);
    CHECK(hash["frames"]["hero 1"]["duration"] == 75);
    CHECK(hash["frames"]["hero 0"]["sourceSize"]["w"] == 8);
    CHECK(hash["meta"]["image"] == "hero.png");
    CHECK(hash["meta"]["size"]["w"] == 32);
    REQUIRE(hash["meta"]["frameTags"].size() == 2);
    CHECK(hash["meta"]["frameTags"][0]["direction"] == "pingpong");
    CHECK(hash["meta"]["frameTags"][1]["repeat"] == "2");

    const auto arr = nlohmann::json::parse(spriteSheetJson(d, sheet, "hero.png", "hero", JsonStyle::Array));
    REQUIRE(arr["frames"].is_array());
    CHECK(arr["frames"][2]["filename"] == "hero 2");

    // Exporting only frames 1..2 remaps tags and drops ones outside the range.
    auto part = buildSpriteSheet(d, {"", 1, 2}, {}).value();
    const auto pj = nlohmann::json::parse(spriteSheetJson(d, part, "p.png", "p", JsonStyle::Hash));
    CHECK(pj["meta"]["frameTags"].empty());
    CHECK(pj["frames"].size() == 2);
}

TEST_CASE("GIF round trip is exact for <=255 colours with transparency", "[export][gif]") {
    const Document d = sampleAnim();
    auto bytes = encodeGif(framesOf(d), 0);
    REQUIRE(bytes);
    auto back = decodeGif(bytes.value());
    REQUIRE(back);
    REQUIRE(back.value().frames.size() == 4);
    for (int f = 0; f < 4; ++f) {
        CHECK(back.value().frames[static_cast<std::size_t>(f)].image == d.composite(f));
        CHECK(back.value().frames[static_cast<std::size_t>(f)].durationMs == gifEffectiveDurationMs(d.frames()[static_cast<std::size_t>(f)].durationMs));
    }
    CHECK(gifEffectiveDurationMs(1) == 20);
    CHECK(gifEffectiveDurationMs(104) == 100);
    CHECK(gifEffectiveDurationMs(105) == 110);
}

TEST_CASE("GIF LZW handles dictionary resets on noisy frames", "[export][gif]") {
    std::mt19937 rng(1234);
    std::vector<Rgba8> pal;
    for (int i = 0; i < 200; ++i) pal.push_back(Rgba8::opaque(static_cast<std::uint8_t>(rng()), static_cast<std::uint8_t>(rng()), static_cast<std::uint8_t>(rng())));
    std::vector<AnimFrame> frames;
    for (int f = 0; f < 3; ++f) {
        Image img(96, 96);
        for (int y = 0; y < 96; ++y)
            for (int x = 0; x < 96; ++x) img.setPixel(x, y, pal[rng() % pal.size()]);
        frames.push_back({img, 100});
    }
    auto back = decodeGif(encodeGif(frames).value());
    REQUIRE(back);
    for (int f = 0; f < 3; ++f) CHECK(back.value().frames[static_cast<std::size_t>(f)].image == frames[static_cast<std::size_t>(f)].image);
}

TEST_CASE("GIF reduces >255 colours approximately", "[export][gif]") {
    Image img(64, 64);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) img.setPixel(x, y, Rgba8::opaque(static_cast<std::uint8_t>(x * 4), static_cast<std::uint8_t>(y * 4), 128));
    auto back = decodeGif(encodeGif({{img, 100}}).value());
    REQUIRE(back);
    int worst = 0;
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            const Rgba8 a = img.pixel(x, y), b = back.value().frames[0].image.pixel(x, y);
            worst = std::max({worst, std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b)});
        }
    CHECK(worst <= 24);
}

TEST_CASE("APNG round trip is lossless including partial alpha", "[export][apng]") {
    Document d = sampleAnim();
    std::vector<AnimFrame> frames = framesOf(d);
    frames[1].image.setPixel(0, 0, Rgba8{10, 20, 30, 77});
    auto bytes = encodeApng(frames, 3);
    REQUIRE(bytes);
    auto first = decodePng(bytes.value());            // non-APNG readers see frame 0
    REQUIRE(first);
    CHECK(first.value() == frames[0].image);
    auto back = decodeApng(bytes.value());
    REQUIRE(back);
    CHECK(back.value().loopCount == 3);
    REQUIRE(back.value().frames.size() == 4);
    for (std::size_t i = 0; i < 4; ++i) {
        CHECK(back.value().frames[i].image == frames[i].image);
        CHECK(back.value().frames[i].durationMs == frames[i].durationMs);
    }
    auto damaged = bytes.value();
    damaged[damaged.size() / 2] ^= 0x55;
    CHECK_FALSE(decodeApng(damaged));
}

TEST_CASE("Animated WebP round trip is lossless and exact", "[export][webp]") {
    Document d = sampleAnim();
    std::vector<AnimFrame> frames = framesOf(d);
    frames[2].image.setPixel(7, 5, Rgba8{1, 2, 3, 200});
    auto bytes = encodeWebp(frames, 0);
    REQUIRE(bytes);
    auto back = decodeWebp(bytes.value());
    REQUIRE(back);
    REQUIRE(back.value().frames.size() == 4);
    for (std::size_t i = 0; i < 4; ++i) {
        CHECK(back.value().frames[i].image == frames[i].image);
        CHECK(back.value().frames[i].durationMs == frames[i].durationMs);
    }
    CHECK_FALSE(decodeWebp(std::vector<std::uint8_t>{1, 2, 3}));
}

TEST_CASE("Encoders reject bad input", "[export]") {
    CHECK_FALSE(encodeGif({}));
    CHECK_FALSE(encodeApng({{Image(2, 2), 100}, {Image(3, 2), 100}}));
    CHECK_FALSE(encodeWebp({{Image(2, 2), 0}}));
}

TEST_CASE("PNG sequence export", "[export]") {
    TempDir dir;
    const Document d = sampleAnim();
    auto paths = exportPngSequence(d, {"walk", 0, -1}, dir.path(), "hero", 2);
    REQUIRE(paths);
    REQUIRE(paths.value().size() == 3);
    CHECK(paths.value()[1].filename() == "hero_001.png");
    auto img = readPng(paths.value()[2]);
    REQUIRE(img);
    CHECK(img.value().width() == 16);
    CHECK_FALSE(exportPngSequence(d, {"walk", 0, -1}, dir.path(), "hero", 2));   // refuses to overwrite
    CHECK(exportPngSequence(d, {"walk", 0, -1}, dir.path(), "hero", 2, true));
}

TEST_CASE("Sheet and animation file writers refuse to overwrite", "[export]") {
    TempDir dir;
    const Document d = sampleAnim();
    auto files = writeSpriteSheet(d, {}, {}, dir.path() / "hero", JsonStyle::Array);
    REQUIRE(files);
    CHECK(files.value().image.filename() == "hero.png");
    CHECK(files.value().data.filename() == "hero.json");
    CHECK(files.value().frames == 4);
    CHECK(readPng(files.value().image).value().width() == files.value().width);
    CHECK_FALSE(writeSpriteSheet(d, {}, {}, dir.path() / "hero.png", JsonStyle::Hash));
    auto noJson = writeSpriteSheet(d, {}, {}, dir.path() / "bare.png", std::nullopt);
    REQUIRE(noJson);
    CHECK(noJson.value().data.empty());
    CHECK_FALSE(fs::exists(dir.path() / "bare.json"));

    auto n = writeAnimation(d, {"walk", 0, -1}, AnimFormat::Gif, dir.path() / "w.gif", 2);
    REQUIRE(n);
    CHECK(n.value() == 4);   // ping-pong 0,1,2,1
    auto back = decodeGif(readFileBytes(dir.path() / "w.gif").value());
    REQUIRE(back);
    CHECK(back.value().frames[0].image.width() == 16);
    CHECK_FALSE(writeAnimation(d, {}, AnimFormat::Webp, dir.path() / "w.gif"));
    CHECK(writeAnimation(d, {}, AnimFormat::Apng, dir.path() / "w.gif", 1, 0, true));
}

TEST_CASE("GIF decoder matches Pillow on interlaced, animated and multi-palette GIFs", "[export]") {
    // tests/data/gif: GIFs written by Pillow 12.3, and Pillow's own decoded frames.
    const std::filesystem::path dir = std::filesystem::path(ACESTUDIO_SOURCE_DIR) / "tests" / "data" / "gif";
    struct Case {
        const char* name;
        std::vector<int> durations;
        int loop;
    };
    for (const Case& c : {Case{"interlaced", {100}, 0}, Case{"anim", {50, 120, 70, 200, 30}, 3}, Case{"local_palettes", {100, 100, 100}, 0}}) {
        INFO(c.name);
        auto bytes = readFileBytes(dir / (std::string(c.name) + ".gif"), 1u << 20);
        REQUIRE(bytes);
        auto gif = decodeGif(bytes.value());
        REQUIRE(gif);
        REQUIRE(gif.value().frames.size() == c.durations.size());
        CHECK(gif.value().loopCount == c.loop);
        for (std::size_t i = 0; i < c.durations.size(); ++i) {
            INFO("frame " << i);
            CHECK(gif.value().frames[i].durationMs == c.durations[i]);
            auto want = readPng(dir / (std::string(c.name) + "_" + std::to_string(i) + ".png"));
            REQUIRE(want);
            const Image& got = gif.value().frames[i].image;
            REQUIRE(got.width() == want.value().width());
            REQUIRE(got.height() == want.value().height());
            int diff = 0;
            for (int y = 0; y < got.height(); ++y)
                for (int x = 0; x < got.width(); ++x) {
                    const Rgba8 a = got.pixel(x, y), b = want.value().pixel(x, y);
                    if (a.a == 0 && b.a == 0) continue;   // transparent: colour does not matter
                    if (!(a == b)) ++diff;
                }
            CHECK(diff == 0);
        }
    }
}
