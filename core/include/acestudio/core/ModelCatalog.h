#pragma once

// Catalogue of downloadable AI models with licence information, and the
// on-disk store of installed models. Nothing is downloaded without an
// explicit licence approval recorded here first.

#include "acestudio/core/Result.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace acestudio::ai {

struct ModelLicense {
    std::string id;
    std::string name;
    std::string url;
    bool commercialUse = false;
    std::string summary;
    std::string obligations;
};

struct ModelFile {
    std::string path;          // relative to the model folder
    std::string url;
    std::uint64_t size = 0;    // 0 = unknown
    std::string sha256;        // empty = recorded on first download
};

struct ModelEntry {
    std::string id;
    std::string name;
    std::string task;          // worker task it serves
    std::string kind = "model";  // model | lora
    std::string requiresModel;   // a LoRA's base model
    // A LoRA's default strength (adapter weight). 1.0 for most; an rsLoRA
    // trained with a large alpha needs far less (it overdrives into noise).
    double loraScale = 1.0;
    std::string description;
    ModelLicense license;
    std::string copyright;
    std::string source;
    std::string entry;         // file or folder (".") handed to the task
    std::string runtime;       // onnx | torch | diffusion
    int vramMb = 0;
    int cpuMemoryMb = 0;   // free RAM needed when the model runs on the CPU (0 = not stated)
    bool pinned = false;       // every file has a known size and hash
    bool fileListVerified = true;
    std::vector<ModelFile> files;

    std::uint64_t knownSize() const;
};

class ModelCatalog {
public:
    static Result<ModelCatalog> load(const std::filesystem::path& file);
    static Result<ModelCatalog> parse(const nlohmann::json& j);
    const std::vector<ModelEntry>& models() const { return m_models; }
    const ModelEntry* find(const std::string& id) const;
    std::vector<const ModelEntry*> forTask(const std::string& task) const;

private:
    std::vector<ModelEntry> m_models;
};

// Whether the worker's installed packages can run this model; returns a
// user-facing explanation (naming the runtime profile to install) if not.
std::optional<std::string> runtimeProblem(const ModelEntry& m, const nlohmann::json& capabilities);

enum class InstallState { NotInstalled, Partial, Installed };
const char* toString(InstallState s) noexcept;

// Installed models live in <root>/<id>/ with a manifest recording the
// accepted licence and the SHA-256 of every file.
class ModelStore {
public:
    explicit ModelStore(std::filesystem::path root) : m_root(std::move(root)) {}
    const std::filesystem::path& root() const { return m_root; }
    std::filesystem::path dir(const ModelEntry& m) const { return m_root / m.id; }
    // Path handed to the task (the entry file, or the folder).
    std::filesystem::path entryPath(const ModelEntry& m) const;

    InstallState state(const ModelEntry& m) const;
    std::uint64_t bytesOnDisk(const ModelEntry& m) const;

    // Licence approval: must exist (for the model's current licence id)
    // before downloadParams() will produce a job.
    Status approve(const ModelEntry& m);
    bool isApproved(const ModelEntry& m) const;
    void revoke(const ModelEntry& m);

    // Parameters for the worker's model.download task. Hashes recorded at
    // a previous download pin files the catalogue could not pin.
    Result<nlohmann::json> downloadParams(const ModelEntry& m) const;
    // Records the SHA-256 values reported by a finished download.
    Status recordDownload(const ModelEntry& m, const nlohmann::json& sha256ByPath);
    // Deletes the model's files (and manifest). Approval is kept.
    Status remove(const ModelEntry& m);

    nlohmann::json manifest(const ModelEntry& m) const;   // {} if none

private:
    nlohmann::json loadApprovals() const;
    Status saveApprovals(const nlohmann::json& j) const;
    std::filesystem::path m_root;
};

} // namespace acestudio::ai
