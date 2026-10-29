#include "acestudio/core/ProjectIO.h"
#include "acestudio/core/FileUtil.h"
#include "acestudio/core/Log.h"
#include "acestudio/core/PngCodec.h"
#include "acestudio/core/Version.h"

#include <miniz.h>

#include <memory>

namespace fs = std::filesystem;
using nlohmann::json;

namespace acestudio {

namespace {

constexpr std::uint64_t kMaxEntryBytes = 512ull << 20;     // per ZIP entry, uncompressed
constexpr std::uint64_t kMaxTotalBytes = 4ull << 30;       // whole project, uncompressed
constexpr const char* kManifestName = "manifest.json";
constexpr const char* kDocumentName = "document.json";

std::string celFileName(LayerId layer, int frame) {
    return "cels/L" + std::to_string(layer) + "_F" + std::to_string(frame) + ".png";
}

// ------------------------------------------------------------ ZIP helpers

class ZipWriter {
public:
    ZipWriter() { m_ok = mz_zip_writer_init_heap(&m_zip, 0, 0) == MZ_TRUE; }
    ~ZipWriter() { mz_zip_writer_end(&m_zip); }
    ZipWriter(const ZipWriter&) = delete;
    ZipWriter& operator=(const ZipWriter&) = delete;

    Status add(const std::string& name, std::span<const std::uint8_t> data, bool compress) {
        if (!m_ok) return Error{ErrorCode::Internal, "ZIP writer not initialised"};
        const mz_uint level = compress ? static_cast<mz_uint>(MZ_DEFAULT_COMPRESSION) : static_cast<mz_uint>(MZ_NO_COMPRESSION);
        if (!mz_zip_writer_add_mem(&m_zip, name.c_str(), data.data(), data.size(), level))
            return Error{ErrorCode::Internal, "Failed to add " + name + " to project archive"};
        return {};
    }

    Result<std::vector<std::uint8_t>> finish() {
        void* buf = nullptr;
        std::size_t size = 0;
        if (!m_ok || !mz_zip_writer_finalize_heap_archive(&m_zip, &buf, &size))
            return Error{ErrorCode::Internal, "Failed to finalise project archive"};
        // Ownership of the finalised heap buffer passes to the caller.
        std::unique_ptr<void, decltype(&mz_free)> guard(buf, &mz_free);
        const auto* p = static_cast<const std::uint8_t*>(buf);
        return std::vector<std::uint8_t>(p, p + size);
    }

private:
    mz_zip_archive m_zip{};
    bool m_ok = false;
};

class ZipReader {
public:
    explicit ZipReader(std::span<const std::uint8_t> bytes) {
        m_ok = mz_zip_reader_init_mem(&m_zip, bytes.data(), bytes.size(), 0) == MZ_TRUE;
    }
    ~ZipReader() { mz_zip_reader_end(&m_zip); }
    ZipReader(const ZipReader&) = delete;
    ZipReader& operator=(const ZipReader&) = delete;

    bool ok() const { return m_ok; }

    // Extracts and CRC-verifies one entry, enforcing size limits.
    Result<std::vector<std::uint8_t>> read(const std::string& name) {
        const int idx = mz_zip_reader_locate_file(&m_zip, name.c_str(), nullptr, 0);
        if (idx < 0) return Error{ErrorCode::CorruptData, "Project is missing " + name};
        mz_zip_archive_file_stat st{};
        if (!mz_zip_reader_file_stat(&m_zip, static_cast<mz_uint>(idx), &st))
            return Error{ErrorCode::CorruptData, "Cannot read entry header for " + name};
        if (st.m_uncomp_size > kMaxEntryBytes) return Error{ErrorCode::CorruptData, name + " is too large"};
        m_total += st.m_uncomp_size;
        if (m_total > kMaxTotalBytes) return Error{ErrorCode::CorruptData, "Project exceeds size limit"};

        std::size_t size = 0;
        void* p = mz_zip_reader_extract_to_heap(&m_zip, static_cast<mz_uint>(idx), &size, 0);
        if (!p) {
            return Error{ErrorCode::CorruptData, "Entry " + name + " is damaged (" +
                                                     mz_zip_get_error_string(mz_zip_get_last_error(&m_zip)) + ")"};
        }
        std::unique_ptr<void, decltype(&mz_free)> guard(p, &mz_free);
        const auto* b = static_cast<const std::uint8_t*>(p);
        return std::vector<std::uint8_t>(b, b + size);
    }

private:
    mz_zip_archive m_zip{};
    bool m_ok = false;
    std::uint64_t m_total = 0;
};

std::span<const std::uint8_t> asBytes(const std::string& s) {
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

// ------------------------------------------------------------ JSON parsing

Error corrupt(std::string m) { return Error{ErrorCode::CorruptData, std::move(m)}; }

Result<Document> documentFromJson(const json& j, ZipReader& zip) {
    const json& canvas = j.at("canvas");
    const auto mode = colorModeFromString(canvas.at("colorMode").get<std::string>());
    if (!mode) return corrupt("Unknown color mode");
    auto created = Document::create(canvas.at("width").get<int>(), canvas.at("height").get<int>(), *mode);
    if (!created) return corrupt(created.error().message);
    Document doc = std::move(created).value();

    if (canvas.contains("pixelAspect")) {
        const auto& pa = canvas["pixelAspect"];
        if (auto st = doc.setPixelAspect(pa.at(0).get<double>(), pa.at(1).get<double>()); !st)
            return corrupt(st.error().message);
    }
    doc.setColorProfile(canvas.value("colorProfile", std::string("sRGB")));

    // Palette
    doc.palette().clear();
    if (j.contains("palette")) {
        doc.palette().setName(j["palette"].value("name", std::string("Untitled")));
        for (const auto& c : j["palette"].at("colors")) {
            auto col = Rgba8::fromHex(c.get<std::string>());
            if (!col) return corrupt("Invalid palette color");
            if (auto st = doc.palette().add(*col); !st) return corrupt(st.error().message);
        }
    }

    // Layers
    for (const auto& jl : j.at("layers")) {
        Layer l;
        l.id = jl.at("id").get<LayerId>();
        l.parent = jl.value("parent", kNoLayer);
        const std::string kind = jl.value("kind", std::string("raster"));
        if (kind == "raster") l.kind = LayerKind::Raster;
        else if (kind == "group") l.kind = LayerKind::Group;
        else return corrupt("Unknown layer kind: " + kind);
        l.name = jl.value("name", std::string());
        l.visible = jl.value("visible", true);
        l.locked = jl.value("locked", false);
        l.alphaLocked = jl.value("alphaLocked", false);
        const int op = jl.value("opacity", 255);
        if (op < 0 || op > 255) return corrupt("Layer opacity out of range");
        l.opacity = static_cast<std::uint8_t>(op);
        const auto blend = blendModeFromString(jl.value("blend", std::string("normal")));
        if (!blend) return corrupt("Unknown blend mode");
        l.blend = *blend;
        if (auto st = doc.restoreLayer(l); !st) return st.error();
    }

    // Frames
    std::vector<Frame> frames;
    for (const auto& jf : j.at("frames")) frames.push_back(Frame{jf.at("durationMs").get<int>()});
    if (frames.empty() || frames.size() > static_cast<std::size_t>(Document::kMaxFrames))
        return corrupt("Invalid frame count");
    doc.restoreFrames(std::move(frames));

    // Cels
    for (const auto& jc : j.at("cels")) {
        const auto layerId = jc.at("layer").get<LayerId>();
        const int frame = jc.at("frame").get<int>();
        const std::string file = jc.at("file").get<std::string>();
        if (file != celFileName(layerId, frame)) return corrupt("Unexpected cel file name: " + file);
        auto png = zip.read(file);
        if (!png) return png.error();
        auto img = decodePng(png.value());
        if (!img) return corrupt(file + ": " + img.error().message);
        Cel cel{std::move(img).value(), jc.value("x", 0), jc.value("y", 0)};
        if (auto st = doc.setCel(layerId, frame, std::move(cel)); !st)
            return corrupt("Invalid cel " + file + ": " + st.error().message);
    }

    // Tags
    if (j.contains("tags")) {
        for (const auto& jt : j["tags"]) {
            AnimationTag t;
            t.name = jt.at("name").get<std::string>();
            t.fromFrame = jt.at("from").get<int>();
            t.toFrame = jt.at("to").get<int>();
            const auto dir = directionFromString(jt.value("direction", std::string("forward")));
            if (!dir) return corrupt("Unknown tag direction");
            t.direction = *dir;
            t.repeat = jt.value("repeat", 0);
            t.color = jt.value("color", std::string("#4F8CFF"));
            if (auto st = doc.addTag(std::move(t)); !st) return corrupt(st.error().message);
        }
    }

    if (j.contains("extensions") && j["extensions"].is_object()) doc.extensions() = j["extensions"];

    if (auto st = doc.validate(); !st) return st.error();
    return doc;
}

} // namespace

json documentToJson(const Document& doc) {
    json j;
    j["canvas"] = {{"width", doc.width()},
                   {"height", doc.height()},
                   {"colorMode", toString(doc.colorMode())},
                   {"pixelAspect", {doc.pixelAspectX(), doc.pixelAspectY()}},
                   {"colorProfile", doc.colorProfile()}};

    json colors = json::array();
    for (const auto& c : doc.palette().colors()) colors.push_back(c.toHex(true));
    j["palette"] = {{"name", doc.palette().name()}, {"colors", colors}};

    json layers = json::array();
    for (const auto& l : doc.layers()) {
        layers.push_back({{"id", l.id},
                          {"parent", l.parent},
                          {"kind", l.kind == LayerKind::Group ? "group" : "raster"},
                          {"name", l.name},
                          {"visible", l.visible},
                          {"locked", l.locked},
                          {"alphaLocked", l.alphaLocked},
                          {"opacity", l.opacity},
                          {"blend", toString(l.blend)}});
    }
    j["layers"] = layers;

    json frames = json::array();
    for (const auto& f : doc.frames()) frames.push_back({{"durationMs", f.durationMs}});
    j["frames"] = frames;

    json cels = json::array();
    for (const auto& [k, c] : doc.cels())
        cels.push_back({{"layer", k.layer}, {"frame", k.frame}, {"x", c.x}, {"y", c.y},
                        {"file", celFileName(k.layer, k.frame)}});
    j["cels"] = cels;

    json tags = json::array();
    for (const auto& t : doc.tags())
        tags.push_back({{"name", t.name}, {"from", t.fromFrame}, {"to", t.toFrame},
                        {"direction", toString(t.direction)}, {"repeat", t.repeat}, {"color", t.color}});
    j["tags"] = tags;
    j["extensions"] = doc.extensions();
    return j;
}

Result<std::vector<std::uint8_t>> serializeProject(const Document& doc, const SaveOptions& opts) {
    if (auto st = doc.validate(); !st)
        return Error{ErrorCode::Internal, "Refusing to save an invalid document: " + st.error().message};

    const std::string now = nowUtcIso8601();
    const json manifest = {{"format", kProjectFormatId},
                           {"schemaVersion", kProjectSchemaVersion},
                           {"writerVersion", kVersionString},
                           {"createdUtc", opts.createdUtc.empty() ? now : opts.createdUtc},
                           {"modifiedUtc", now}};

    ZipWriter zip;
    // Manifest first so it can be sniffed cheaply.
    const std::string manifestText = manifest.dump(2);
    if (auto st = zip.add(kManifestName, asBytes(manifestText), true); !st) return st.error();
    const std::string docText = documentToJson(doc).dump(2);
    if (auto st = zip.add(kDocumentName, asBytes(docText), true); !st) return st.error();

    for (const auto& [k, c] : doc.cels()) {
        auto png = encodePng(c.image);
        if (!png) return png.error();
        if (auto st = zip.add(celFileName(k.layer, k.frame), png.value(), false); !st) return st.error();
    }
    return zip.finish();
}

Result<Document> deserializeProject(std::span<const std::uint8_t> bytes, ProjectInfo* infoOut) {
    ZipReader zip(bytes);
    if (!zip.ok()) return corrupt("Not an AceStudio project (invalid archive)");

    try {
        auto manifestBytes = zip.read(kManifestName);
        if (!manifestBytes) return manifestBytes.error();
        const json manifest = json::parse(manifestBytes.value());
        if (manifest.value("format", std::string()) != kProjectFormatId)
            return corrupt("Not an AceStudio project (unknown format id)");

        ProjectInfo info;
        info.schemaVersion = manifest.at("schemaVersion").get<int>();
        info.writerVersion = manifest.value("writerVersion", std::string());
        info.createdUtc = manifest.value("createdUtc", std::string());
        info.modifiedUtc = manifest.value("modifiedUtc", std::string());
        if (info.schemaVersion > kProjectSchemaVersion)
            return Error{ErrorCode::UnsupportedVersion,
                         "This project was saved by a newer AceStudio (" + info.writerVersion +
                             ", schema " + std::to_string(info.schemaVersion) + "). Please update AceStudio."};
        if (info.schemaVersion < 1) return corrupt("Invalid schema version");
        // Future: migrations from older schema versions run here.

        auto docBytes = zip.read(kDocumentName);
        if (!docBytes) return docBytes.error();
        const json docJson = json::parse(docBytes.value());
        auto doc = documentFromJson(docJson, zip);
        if (doc && infoOut) *infoOut = std::move(info);
        return doc;
    } catch (const json::exception& e) {
        return corrupt(std::string("Malformed project data: ") + e.what());
    }
}

Status saveProject(const Document& doc, const fs::path& path, const SaveOptions& opts) {
    auto bytes = serializeProject(doc, opts);
    if (!bytes) return bytes.error();

    // Verify what we are about to write can be read back before touching
    // the user's existing file.
    if (auto check = deserializeProject(bytes.value()); !check)
        return Error{ErrorCode::Internal, "Save aborted: verification failed: " + check.error().message};

    const fs::path backup = opts.keepBackup ? fs::path(path.string() + ".bak") : fs::path();
    auto st = writeFileAtomic(path, bytes.value(), backup);
    if (st) ACE_LOG_INFO("project", "Saved " + path.string() + " (" + std::to_string(bytes.value().size()) + " bytes)");
    else ACE_LOG_ERROR("project", "Save failed for " + path.string() + ": " + st.error().message);
    return st;
}

Result<Document> loadProject(const fs::path& path, ProjectInfo* infoOut) {
    auto bytes = readFileBytes(path, kMaxTotalBytes);
    if (!bytes) return bytes.error();
    auto doc = deserializeProject(bytes.value(), infoOut);
    if (doc) ACE_LOG_INFO("project", "Opened " + path.string());
    else ACE_LOG_ERROR("project", "Open failed for " + path.string() + ": " + doc.error().message);
    return doc;
}

} // namespace acestudio
