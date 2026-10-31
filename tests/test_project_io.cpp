#include <catch2/catch_test_macros.hpp>

#include "TestSupport.h"
#include "acestudio/core/FileUtil.h"
#include "acestudio/core/ProjectIO.h"

#include <miniz.h>

#include <algorithm>
#include <filesystem>

using namespace acestudio;
namespace fs = std::filesystem;

namespace {
Document sampleDocument() {
    auto d = Document::create(32, 16, ColorMode::Indexed).value();
    REQUIRE(d.setPixelAspect(2.0, 1.0));
    d.setColorProfile("Display P3");
    d.palette().setName("Test");
    const auto g = d.addLayer("Armor", LayerKind::Group).value();
    const auto body = d.addLayer("Body").value();
    const auto plate = d.addLayer("Plate", LayerKind::Raster, g).value();
    d.layer(plate)->blend = BlendMode::Multiply;
    d.layer(plate)->opacity = 200;
    d.layer(plate)->alphaLocked = true;
    d.layer(body)->locked = true;
    d.layer(g)->visible = false;

    REQUIRE(d.insertFrame(1, 80));
    REQUIRE(d.setFrameDuration(0, 120));
    Image a(3, 2);
    a.setPixel(1, 1, Rgba8{10, 20, 30, 40});
    REQUIRE(d.setCel(body, 0, Cel{a, 4, 5}));
    REQUIRE(d.setCel(plate, 1, Cel{Image(32, 16, Rgba8::opaque(1, 2, 3)), 0, 0}));
    REQUIRE(d.addTag({"idle", 0, 1, PlaybackDirection::PingPong, 3, "#ABCDEF"}));
    d.extensions()["ai"] = {{"model", "sdxl-base-1.0"}, {"seed", 1234}};
    return d;
}

void checkEqual(const Document& a, const Document& b) {
    CHECK(documentToJson(a) == documentToJson(b));
    REQUIRE(a.cels().size() == b.cels().size());
    for (const auto& [k, c] : a.cels()) {
        const Cel* other = b.cel(k.layer, k.frame);
        REQUIRE(other);
        CHECK(other->image == c.image);
        CHECK(other->x == c.x);
        CHECK(other->y == c.y);
    }
}
} // namespace

TEST_CASE("Project serializes and deserializes losslessly", "[project]") {
    const Document doc = sampleDocument();
    auto bytes = serializeProject(doc, {false, "2026-01-01T00:00:00.000Z"});
    REQUIRE(bytes);

    ProjectInfo info;
    auto back = deserializeProject(bytes.value(), &info);
    REQUIRE(back);
    checkEqual(doc, back.value());
    CHECK(info.schemaVersion == 1);   // nothing schema 2 adds is used
    CHECK(info.createdUtc == "2026-01-01T00:00:00.000Z");
    CHECK_FALSE(info.modifiedUtc.empty());
    CHECK(back.value().colorMode() == ColorMode::Indexed);
    CHECK(back.value().pixelAspectX() == 2.0);
    CHECK(back.value().extensions()["ai"]["seed"] == 1234);
    // New layers after load must not collide with restored IDs.
    auto copy = std::move(back).value();
    CHECK(copy.addLayer("new").value() == 4);
}

TEST_CASE("Save and load through the filesystem with backup", "[project]") {
    TempDir dir;
    const fs::path path = dir / "hero.acestudio";
    Document doc = sampleDocument();
    REQUIRE(saveProject(doc, path));
    CHECK_FALSE(fs::exists(path.string() + ".bak"));                  // nothing to back up yet

    REQUIRE(doc.addLayer("Second save"));
    REQUIRE(saveProject(doc, path));
    REQUIRE(fs::exists(path.string() + ".bak"));

    auto previous = loadProject(path.string() + ".bak");
    REQUIRE(previous);
    CHECK(previous.value().layers().size() == 3);
    auto current = loadProject(path);
    REQUIRE(current);
    CHECK(current.value().layers().size() == 4);

    // No temporary files are left behind.
    int entries = 0;
    for (const auto& e : fs::directory_iterator(dir.path())) { (void)e; ++entries; }
    CHECK(entries == 2);
}

TEST_CASE("Loading rejects corrupt and foreign files", "[project]") {
    TempDir dir;
    const fs::path path = dir / "x.acestudio";
    auto bytes = serializeProject(sampleDocument()).value();

    SECTION("truncated archive") {
        bytes.resize(bytes.size() / 2);
        auto r = deserializeProject(bytes);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == ErrorCode::CorruptData);
    }
    SECTION("damaged cel pixel data fails the ZIP CRC check") {
        // Cel PNGs are stored uncompressed, so their bytes appear verbatim.
        const std::string idat = "IDAT";
        auto it = std::search(bytes.begin(), bytes.end(), idat.begin(), idat.end());
        REQUIRE(it != bytes.end());
        *(it + 6) ^= 0xFF;
        auto r = deserializeProject(bytes);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == ErrorCode::CorruptData);
    }
    SECTION("not a zip") {
        std::vector<std::uint8_t> text = {'h', 'e', 'l', 'l', 'o'};
        CHECK(deserializeProject(text).error().code == ErrorCode::CorruptData);
    }
    SECTION("missing file") {
        CHECK(loadProject(dir / "nope.acestudio").error().code == ErrorCode::NotFound);
    }
}

TEST_CASE("Projects from a newer schema are refused / not misread", "[project]") {
    // Re-pack a valid project with a manifest that claims a future schema.
    auto bytes = serializeProject(Document::create(4, 4).value()).value();
    mz_zip_archive in{};
    REQUIRE(mz_zip_reader_init_mem(&in, bytes.data(), bytes.size(), 0));
    mz_zip_archive out{};
    REQUIRE(mz_zip_writer_init_heap(&out, 0, 0));
    const std::string manifest =
        R"({"format":"acestudio.project","schemaVersion":99,"writerVersion":"9.0.0"})";
    REQUIRE(mz_zip_writer_add_mem(&out, "manifest.json", manifest.data(), manifest.size(), static_cast<mz_uint>(MZ_DEFAULT_COMPRESSION)));
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&in); ++i) {
        char name[256];
        mz_zip_reader_get_filename(&in, i, name, sizeof name);
        if (std::string(name) == "manifest.json") continue;
        REQUIRE(mz_zip_writer_add_from_zip_reader(&out, &in, i));
    }
    void* buf = nullptr;
    std::size_t size = 0;
    REQUIRE(mz_zip_writer_finalize_heap_archive(&out, &buf, &size));
    std::vector<std::uint8_t> patched(static_cast<std::uint8_t*>(buf), static_cast<std::uint8_t*>(buf) + size);
    mz_free(buf);
    mz_zip_writer_end(&out);
    mz_zip_reader_end(&in);

    auto r = deserializeProject(patched);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == ErrorCode::UnsupportedVersion);
    CHECK(r.error().message.find("newer AceStudio") != std::string::npos);
}

TEST_CASE("Save refuses to write an invalid document", "[project]") {
    auto doc = Document::create(4, 4).value();
    Layer a; a.id = 5; a.kind = LayerKind::Group; a.parent = 6;
    Layer b; b.id = 6; b.kind = LayerKind::Group; b.parent = 5;
    REQUIRE(doc.restoreLayer(a));
    REQUIRE(doc.restoreLayer(b));
    TempDir dir;
    CHECK_FALSE(saveProject(doc, dir / "bad.acestudio"));
    CHECK_FALSE(fs::exists(dir / "bad.acestudio"));
}
