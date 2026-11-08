#pragma once

// Path confinement for AI worker jobs (SEC-005).
//
// The worker reads and writes whatever paths a job names, so the host checks
// every path parameter before a job is queued. WorkerSupervisor::submit()
// calls checkJobPaths() for every job, so the app, acestudio-cli and
// acestudio-mcp (which runs the CLI) all go through it.
//
// Path parameters (see ai_worker/acestudio_worker/tasks/*.py):
//   inputs   input, input<N>, init_image, mask, reference<N>, references[],
//            controlnet.image, ip_adapter.images[]
//   models   model, model_dir, vae_dir, lora.path, lora_preload.path,
//            controlnet.dir, ip_adapter.dir, preview.dir, cutout.model
//   outputs  dest_dir
//   names    ip_adapter.weight, ip_adapter.encoder, files[].path: relative,
//            no "..", inside the folder they belong to
// Every path must be absolute and resolve (after "..", "." and existing
// symbolic links) inside one of `roots` or the job's own folder. An input may
// also be one of `userFiles`: a file the user chose explicitly.

#include "acestudio/core/Result.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <vector>

namespace acestudio::ai {

struct JobPathPolicy {
    std::vector<std::filesystem::path> roots;       // AI data folders, model folder, jobs folder
    std::filesystem::path jobDir;                   // this job's folder
    std::vector<std::filesystem::path> userFiles;   // inputs the user picked (inputs only)
};

// True when `path` is `root` or inside it, after resolving both (absolute
// paths only; case-insensitive on Windows).
bool pathIsInside(const std::filesystem::path& path, const std::filesystem::path& root);

// Checks every path parameter of `params`; the error names the first bad one.
Status checkJobPaths(const nlohmann::json& params, const JobPathPolicy& policy);

} // namespace acestudio::ai
