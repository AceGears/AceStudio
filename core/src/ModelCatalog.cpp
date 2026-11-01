#include "acestudio/core/ModelCatalog.h"

#include "acestudio/core/FileUtil.h"

#include <algorithm>

namespace fs = std::filesystem;
using nlohmann::json;

namespace acestudio::ai {

namespace {
constexpr const char* kManifest = "acestudio-model.json";
constexpr const char* kApprovals = "licence-approvals.json";

bool safeRelative(const std::string& p) {
    if (p.empty() || p.front() == '/' || p.front() == '\\' || p.find(':') != std::string::npos) return false;
    std::string norm = p;
    std::replace(norm.begin(), norm.end(), '\\', '/');
    std::size_t start = 0;
    while (start <= norm.size()) {
        const std::size_t end = std::min(norm.find('/', start), norm.size());
        if (norm.substr(start, end - start) == "..") return false;
        start = end + 1;
    }
    return true;
}

Result<json> readJson(const fs::path& p) {
    auto bytes = readFileBytes(p, 16u << 20);
    if (!bytes) return bytes.error();
    json j = json::parse(bytes.value().begin(), bytes.value().end(), nullptr, false);
    if (j.is_discarded()) return Error{ErrorCode::CorruptData, p.string() + " is not valid JSON"};
    return j;
}

Status writeJson(const fs::path& p, const json& j) {
    const std::string s = j.dump(2);
    return writeFileAtomic(p, std::span(reinterpret_cast<const std::uint8_t*>(s.data()), s.size()));
}

fs::path fromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
} // namespace

std::uint64_t ModelEntry::knownSize() const {
    std::uint64_t n = 0;
    for (const auto& f : files) n += f.size;
    return n;
}

Result<ModelCatalog> ModelCatalog::load(const fs::path& file) {
    auto j = readJson(file);
    if (!j) return j.error();
    return parse(j.value());
}

Result<ModelCatalog> ModelCatalog::parse(const json& j) {
    if (!j.is_object() || j.value("schemaVersion", 0) != 1 || !j.contains("models") || !j["models"].is_array())
        return Error{ErrorCode::CorruptData, "Unsupported model catalogue"};
    const json licenses = j.value("licenses", json::object());
    ModelCatalog cat;
    try {
        for (const auto& m : j["models"]) {
            ModelEntry e;
            e.id = m.at("id").get<std::string>();
            e.name = m.at("name").get<std::string>();
            e.task = m.at("task").get<std::string>();
            e.kind = m.value("kind", std::string("model"));
            e.requiresModel = m.value("requiresModel", std::string());
            e.loraScale = m.value("loraScale", 1.0);
            if (!(e.loraScale > 0.0 && e.loraScale <= 2.0))
                return Error{ErrorCode::CorruptData, "model " + e.id + ": loraScale must be in (0, 2]"};
            e.description = m.value("description", std::string());
            e.copyright = m.value("copyright", std::string());
            e.source = m.value("source", std::string());
            e.entry = m.value("entry", std::string("."));
            e.runtime = m.value("runtime", std::string());
            e.vramMb = m.value("vramMb", 0);
            e.cpuMemoryMb = m.value("cpuMemoryMb", 0);
            e.fileListVerified = m.value("fileListVerified", true);
            const std::string lic = m.at("license").get<std::string>();
            if (!licenses.contains(lic)) return Error{ErrorCode::CorruptData, "Model " + e.id + " names unknown licence " + lic};
            const json& L = licenses[lic];
            e.license = {lic, L.value("name", lic), L.value("url", std::string()), L.value("commercialUse", false),
                         L.value("summary", std::string()), L.value("obligations", std::string())};
            if (e.id.empty() || e.id.front() == '.' || e.id.find_first_of("/\\:") != std::string::npos)
                return Error{ErrorCode::CorruptData, "Invalid model id '" + e.id + "'"};
            if (e.entry != "." && !safeRelative(e.entry)) return Error{ErrorCode::CorruptData, "Unsafe entry in " + e.id};
            bool pinned = true;
            for (const auto& f : m.at("files")) {
                ModelFile mf{f.at("path").get<std::string>(), f.at("url").get<std::string>(), f.value("size", std::uint64_t{0}),
                             f.value("sha256", std::string())};
                if (!safeRelative(mf.path)) return Error{ErrorCode::CorruptData, "Unsafe file path in " + e.id + ": " + mf.path};
                if (mf.url.rfind("https://", 0) != 0) return Error{ErrorCode::CorruptData, "Model files must use https: " + mf.url};
                if (mf.size == 0 || mf.sha256.size() != 64) pinned = false;
                e.files.push_back(std::move(mf));
            }
            if (e.files.empty()) return Error{ErrorCode::CorruptData, "Model " + e.id + " has no files"};
            e.pinned = pinned;
            if (cat.find(e.id)) return Error{ErrorCode::CorruptData, "Duplicate model id " + e.id};
            cat.m_models.push_back(std::move(e));
        }
    } catch (const json::exception& ex) {
        return Error{ErrorCode::CorruptData, std::string("Model catalogue: ") + ex.what()};
    }
    return cat;
}

const ModelEntry* ModelCatalog::find(const std::string& id) const {
    for (const auto& m : m_models)
        if (m.id == id) return &m;
    return nullptr;
}

std::vector<const ModelEntry*> ModelCatalog::forTask(const std::string& task) const {
    std::vector<const ModelEntry*> out;
    for (const auto& m : m_models)
        if (m.task == task) out.push_back(&m);
    return out;
}

std::optional<std::string> runtimeProblem(const ModelEntry& m, const json& caps) {
    auto has = [&](const char* k) { return caps.is_object() && caps.value(k, false); };
    if (m.runtime == "onnx" && !has("onnxruntime"))
        return m.name + " needs ONNX Runtime. Install the \"onnx-windows\" (or \"onnx-cpu\") runtime profile.";
    if (m.runtime == "torch" && !has("torch"))
        return m.name + " needs PyTorch. Install the \"diffusion-cuda\" (NVIDIA) or \"diffusion-cpu\" runtime profile.";
    if (m.runtime == "diffusion" && !(has("torch") && has("diffusers")))
        return m.name + " needs PyTorch and Diffusers. Install the \"diffusion-cuda\" (NVIDIA) or \"diffusion-cpu\" runtime profile.";
    return std::nullopt;
}

const char* toString(InstallState s) noexcept {
    switch (s) {
    case InstallState::NotInstalled: return "not installed";
    case InstallState::Partial: return "partially downloaded";
    case InstallState::Installed: return "installed";
    }
    return "?";
}

fs::path ModelStore::entryPath(const ModelEntry& m) const {
    return m.entry == "." ? dir(m) : dir(m) / fromUtf8(m.entry);
}

json ModelStore::manifest(const ModelEntry& m) const {
    auto j = readJson(dir(m) / kManifest);
    return j && j.value().is_object() ? j.value() : json::object();
}

InstallState ModelStore::state(const ModelEntry& m) const {
    std::error_code ec;
    const json man = manifest(m);
    const json hashes = man.value("files", json::object());
    int present = 0;
    bool complete = true;
    for (const auto& f : m.files) {
        const fs::path p = dir(m) / fromUtf8(f.path);
        const bool partial = fs::exists(fs::path(p).concat(".part"), ec);
        if (fs::exists(p, ec)) {
            ++present;
            if (f.size && fs::file_size(p, ec) != f.size) complete = false;
            // Every file must have been hash-checked by a download.
            if (!hashes.contains(f.path)) complete = false;
        } else {
            complete = false;
            if (partial) ++present;
        }
    }
    if (present == 0) return InstallState::NotInstalled;
    return complete ? InstallState::Installed : InstallState::Partial;
}

std::uint64_t ModelStore::bytesOnDisk(const ModelEntry& m) const {
    std::uint64_t n = 0;
    std::error_code ec;
    for (const auto& f : m.files) {
        const fs::path p = dir(m) / fromUtf8(f.path);
        if (fs::exists(p, ec)) n += fs::file_size(p, ec);
        else if (fs::exists(fs::path(p).concat(".part"), ec)) n += fs::file_size(fs::path(p).concat(".part"), ec);
    }
    return n;
}

json ModelStore::loadApprovals() const {
    auto j = readJson(m_root / kApprovals);
    return j && j.value().is_object() ? j.value() : json::object();
}

Status ModelStore::saveApprovals(const json& j) const {
    std::error_code ec;
    fs::create_directories(m_root, ec);
    if (ec) return Error{ErrorCode::IoError, "Cannot create " + m_root.string() + ": " + ec.message()};
    return writeJson(m_root / kApprovals, j);
}

Status ModelStore::approve(const ModelEntry& m) {
    json a = loadApprovals();
    a[m.id] = {{"license", m.license.id}, {"licenseName", m.license.name}, {"licenseUrl", m.license.url},
               {"approvedUtc", nowUtcIso8601()}};
    return saveApprovals(a);
}

bool ModelStore::isApproved(const ModelEntry& m) const {
    const json a = loadApprovals();
    // A licence change in a newer catalogue requires approving again.
    return a.contains(m.id) && a[m.id].value("license", std::string()) == m.license.id;
}

void ModelStore::revoke(const ModelEntry& m) {
    json a = loadApprovals();
    a.erase(m.id);
    (void)saveApprovals(a);
}

Result<json> ModelStore::downloadParams(const ModelEntry& m) const {
    if (!isApproved(m))
        return Error{ErrorCode::PermissionDenied, "The licence for " + m.name + " has not been accepted."};
    const json recorded = manifest(m).value("files", json::object());
    json files = json::array();
    for (const auto& f : m.files) {
        std::string sha = f.sha256;
        if (sha.empty() && recorded.contains(f.path)) sha = recorded[f.path].value("sha256", std::string());
        files.push_back({{"path", f.path}, {"url", f.url}, {"size", f.size}, {"sha256", sha}});
    }
    const auto d = dir(m).u8string();
    return json{{"dest_dir", std::string(d.begin(), d.end())}, {"files", files}, {"license_accepted", m.license.id},
                {"model_id", m.id}};
}

Status ModelStore::recordDownload(const ModelEntry& m, const json& sha256ByPath) {
    json man = manifest(m);
    man["id"] = m.id;
    man["license"] = m.license.id;
    man["downloadedUtc"] = nowUtcIso8601();
    json files = man.value("files", json::object());
    std::error_code ec;
    for (const auto& f : m.files) {
        if (!sha256ByPath.contains(f.path) || !sha256ByPath[f.path].is_string()) continue;
        const std::string sha = sha256ByPath[f.path].get<std::string>();
        if (!f.sha256.empty() && sha != f.sha256)
            return Error{ErrorCode::CorruptData, f.path + " does not match the catalogue hash"};
        files[f.path] = {{"sha256", sha}, {"size", fs::file_size(dir(m) / fromUtf8(f.path), ec)}};
    }
    man["files"] = files;
    return writeJson(dir(m) / kManifest, man);
}

Status ModelStore::remove(const ModelEntry& m) {
    std::error_code ec;
    fs::remove_all(dir(m), ec);
    if (ec) return Error{ErrorCode::IoError, "Cannot delete " + dir(m).string() + ": " + ec.message()};
    return {};
}

} // namespace acestudio::ai
