#include "acestudio/core/ProjectIO.h"
#include "acestudio/core/FileUtil.h"
#include "acestudio/core/Log.h"
#include "acestudio/core/PngCodec.h"
#include "acestudio/core/Version.h"

#include "ZipArchive.h"

#include <memory>

namespace fs = std::filesystem;
using nlohmann::json;

namespace acestudio {

namespace {

constexpr const char* kManifestName = "manifest.json";
constexpr const char* kDocumentName = "document.json";

std::string celFileName(LayerId layer, int frame) {
    return "cels/L" + std::to_string(layer) + "_F" + std::to_string(frame) + ".png";
}

std::string maskFileName(LayerId layer, int frame) {
    return "masks/L" + std::to_string(layer) + "_F" + std::to_string(frame) + ".png";
}

using detail::kMaxTotalBytes;
using detail::ZipReader;
using detail::ZipWriter;

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
        else if (kind == "adjustment") l.kind = LayerKind::Adjustment;
        else return corrupt("Unknown layer kind: " + kind);
        l.clipped = jl.value("clipped", false);
        if (jl.contains("mask")) {
            l.hasMask = true;
            l.maskEnabled = jl["mask"].value("enabled", true);
        }
        if (l.kind == LayerKind::Adjustment) {
            const json& ja = jl.at("adjustment");
            const auto t = adjustmentTypeFromString(ja.at("type").get<std::string>());
            if (!t) return corrupt("Unknown adjustment type");
            l.adjustment = Adjustment{*t, ja.value("a", 0), ja.value("b", 0), ja.value("c", 0)};
            if (ja.contains("points")) {
                if (!ja["points"].is_array() || ja["points"].size() > 16) return corrupt("Invalid curve points");
                for (const auto& pt : ja["points"]) {
                    if (!pt.is_array() || pt.size() != 2 || !pt[0].is_number_integer() || !pt[1].is_number_integer())
                        return corrupt("Invalid curve point");
                    const int px = pt[0].get<int>(), py = pt[1].get<int>();
                    if (px < 0 || px > 255 || py < 0 || py > 255) return corrupt("Curve point out of range");
                    l.adjustment.points.push_back({static_cast<std::uint8_t>(px), static_cast<std::uint8_t>(py)});
                }
            }
        }
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

    // Cels. Linked cels (schema 3) carry a "link" id and may share one PNG.
    std::map<std::string, Image> decoded;
    std::map<CelKey, std::uint32_t> links;
    for (const auto& jc : j.at("cels")) {
        const auto layerId = jc.at("layer").get<LayerId>();
        const int frame = jc.at("frame").get<int>();
        const std::string file = jc.at("file").get<std::string>();
        std::uint32_t link = 0;
        if (jc.contains("link")) {
            if (!jc["link"].is_number_unsigned() || jc["link"].get<std::uint64_t>() == 0 || jc["link"].get<std::uint64_t>() > 0xFFFFFFFFull)
                return corrupt("Invalid cel link");
            link = jc["link"].get<std::uint32_t>();
        }
        bool nameOk = file == celFileName(layerId, frame);
        if (!nameOk && link)   // a linked cel may name another frame's file of the same layer
            for (int f = 0; f < doc.frameCount() && !nameOk; ++f) nameOk = file == celFileName(layerId, f);
        if (!nameOk) return corrupt("Unexpected cel file name: " + file);
        auto it = decoded.find(file);
        if (it == decoded.end()) {
            auto png = zip.read(file);
            if (!png) return png.error();
            auto img = decodePng(png.value());
            if (!img) return corrupt(file + ": " + img.error().message);
            it = decoded.emplace(file, std::move(img).value()).first;
        }
        Cel cel{it->second, jc.value("x", 0), jc.value("y", 0)};
        if (auto st = doc.setCel(layerId, frame, std::move(cel)); !st)
            return corrupt("Invalid cel " + file + ": " + st.error().message);
        if (link) links[CelKey{layerId, frame}] = link;
    }
    doc.restoreLinks(std::move(links));   // checked by validate()

    // Layer masks (schema 2)
    if (j.contains("masks")) {
        for (const auto& jc : j["masks"]) {
            const auto layerId = jc.at("layer").get<LayerId>();
            const int frame = jc.at("frame").get<int>();
            const std::string file = jc.at("file").get<std::string>();
            if (file != maskFileName(layerId, frame)) return corrupt("Unexpected mask file name: " + file);
            auto png = zip.read(file);
            if (!png) return png.error();
            auto img = decodePng(png.value());
            if (!img) return corrupt(file + ": " + img.error().message);
            if (auto st = doc.setMaskCel(layerId, frame, Cel{std::move(img).value(), jc.value("x", 0), jc.value("y", 0)}); !st)
                return corrupt("Invalid mask " + file + ": " + st.error().message);
        }
    }
    if (canvas.contains("tileMode")) {
        const auto tm = tileModeFromString(canvas["tileMode"].get<std::string>());
        if (!tm) return corrupt("Unknown tile mode");
        doc.setTileMode(*tm);
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

int requiredSchemaVersion(const Document& doc) {
    // The lowest schema that can hold this document, so files that use no new
    // feature stay readable by older builds.
    int v = 1;
    if (doc.tileMode() != TileMode::None || !doc.maskCels().empty()) v = 2;
    for (const auto& l : doc.layers()) {
        if (l.clipped || l.hasMask || l.kind == LayerKind::Adjustment) v = std::max(v, 2);
        if (l.kind == LayerKind::Adjustment &&
            (l.adjustment.type == AdjustmentType::Levels || l.adjustment.type == AdjustmentType::Curves))
            v = std::max(v, 3);   // schema 3: Levels and Curves
    }
    if (!doc.links().empty()) v = std::max(v, 3);   // schema 3: linked cels
    return v;
}

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
        json jl = {{"id", l.id},
                   {"parent", l.parent},
                   {"kind", l.kind == LayerKind::Group ? "group" : l.kind == LayerKind::Adjustment ? "adjustment" : "raster"},
                   {"name", l.name},
                   {"visible", l.visible},
                   {"locked", l.locked},
                   {"alphaLocked", l.alphaLocked},
                   {"opacity", l.opacity},
                   {"blend", toString(l.blend)}};
        if (l.clipped) jl["clipped"] = true;
        if (l.hasMask) jl["mask"] = {{"enabled", l.maskEnabled}};
        if (l.kind == LayerKind::Adjustment) {
            jl["adjustment"] = {{"type", toString(l.adjustment.type)}, {"a", l.adjustment.a}, {"b", l.adjustment.b}, {"c", l.adjustment.c}};
            if (!l.adjustment.points.empty()) {
                json pts = json::array();
                for (const auto& pt : l.adjustment.points) pts.push_back({pt.x, pt.y});
                jl["adjustment"]["points"] = pts;
            }
        }
        layers.push_back(std::move(jl));
    }
    j["layers"] = layers;

    json frames = json::array();
    for (const auto& f : doc.frames()) frames.push_back({{"durationMs", f.durationMs}});
    j["frames"] = frames;

    json cels = json::array();
    for (const auto& [k, c] : doc.cels()) {
        const std::uint32_t link = doc.celLink(k.layer, k.frame);
        // Linked cels share the PNG of their group's first frame.
        const int fileFrame = link ? doc.linkedFrames(k.layer, k.frame).front() : k.frame;
        json jc = {{"layer", k.layer}, {"frame", k.frame}, {"x", c.x}, {"y", c.y}, {"file", celFileName(k.layer, fileFrame)}};
        if (link) jc["link"] = link;
        cels.push_back(std::move(jc));
    }
    j["cels"] = cels;
    if (!doc.maskCels().empty()) {
        json masks = json::array();
        for (const auto& [k, c] : doc.maskCels())
            masks.push_back({{"layer", k.layer}, {"frame", k.frame}, {"x", c.x}, {"y", c.y}, {"file", maskFileName(k.layer, k.frame)}});
        j["masks"] = masks;
    }
    if (doc.tileMode() != TileMode::None) j["canvas"]["tileMode"] = toString(doc.tileMode());

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
                           {"schemaVersion", requiredSchemaVersion(doc)},
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
        // A linked group's pixels are stored once, under its first frame.
        if (doc.celLink(k.layer, k.frame) && doc.linkedFrames(k.layer, k.frame).front() != k.frame) continue;
        auto png = encodePng(c.image);
        if (!png) return png.error();
        if (auto st = zip.add(celFileName(k.layer, k.frame), png.value(), false); !st) return st.error();
    }
    for (const auto& [k, c] : doc.maskCels()) {
        auto png = encodePng(c.image);
        if (!png) return png.error();
        if (auto st = zip.add(maskFileName(k.layer, k.frame), png.value(), false); !st) return st.error();
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
