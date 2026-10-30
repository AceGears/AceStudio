// acestudio-cli ai ... — the AI worker from the command line. Uses exactly
// the same supervisor, catalogue and model store as the desktop app.

#include "CliCommon.h"

#include "acestudio/core/AiWorker.h"
#include "acestudio/core/FileUtil.h"
#include "acestudio/core/ModelCatalog.h"
#include "acestudio/core/PngCodec.h"
#include "acestudio/core/Settings.h"

#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;

namespace acestudio::cli {

namespace {

using namespace acestudio::ai;
using namespace std::chrono_literals;

std::string utf8(const fs::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

struct AiContext {
    AiEnvironment env;
    std::string pythonPath;   // setting ai.pythonPath
};

Result<AiContext> context() {
    AiContext c;
    auto data = userDataDir();
    if (!data) return data.error();
    Settings settings;
    (void)settings.load(data.value() / "settings.json");
    c.pythonPath = settings.get<std::string>("ai.pythonPath");
    auto local = userLocalDataDir();
    if (!local) return local.error();
    auto env = resolveAiEnvironment(local.value(), settings.get<std::string>("ai.modelDirectory"), executableDir());
    if (!env) return env.error();
    c.env = env.value();
    return c;
}

std::string mb(std::uint64_t bytes) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f MB", static_cast<double>(bytes) / 1e6);
    return buf;
}

// Runs one job to completion, printing progress to stderr in human mode.
Result<const Job*> runJob(WorkerSupervisor& sup, const std::string& task, json params, bool quiet,
                          const std::vector<std::pair<std::string, const Image*>>& images = {},
                          const ModelEntry* model = nullptr,
                          std::chrono::minutes timeout = std::chrono::minutes(120)) {
    sup.start();
    if (!sup.runUntil([&] { return sup.state() == WorkerState::Ready || sup.state() == WorkerState::Failed; }, 120s))
        return Error{ErrorCode::IoError, "The AI worker did not start in time"};
    if (sup.state() == WorkerState::Failed) return Error{ErrorCode::IoError, sup.lastError()};
    std::string why;
    if (!sup.taskAvailable(task, &why)) return Error{ErrorCode::UnsupportedVersion, task + " is not available: " + why};
    if (model)
        if (auto problem = runtimeProblem(*model, sup.hello()["capabilities"]))
            return Error{ErrorCode::UnsupportedVersion, *problem + " (acestudio-cli ai setup --profile ... --yes)"};
    auto id = sup.submit(task, std::move(params), task, images);
    if (!id) return id.error();
    std::string last;
    const bool done = sup.runUntil([&] {
        const Job* j = sup.job(id.value());
        if (!quiet) {
            std::string line = j->message;
            if (j->progress >= 0) line = std::to_string(static_cast<int>(j->progress * 100)) + "%  " + line;
            if (!line.empty() && line != last) {
                std::fprintf(stderr, "\r%-78.78s", line.c_str());
                std::fflush(stderr);
                last = line;
            }
        }
        return isFinished(j->state);
    }, timeout);
    if (!quiet && !last.empty()) std::fprintf(stderr, "\n");
    const Job* j = sup.job(id.value());
    if (!done) {
        (void)sup.cancel(j->id);
        return Error{ErrorCode::Cancelled, "Timed out"};
    }
    if (j->state == JobState::Failed) {
        const ErrorCode code = j->failure == FailureKind::InvalidInput ? ErrorCode::InvalidArgument
                             : j->failure == FailureKind::MissingModel || j->failure == FailureKind::MissingDependency
                                 ? ErrorCode::NotFound
                                 : ErrorCode::Internal;
        return Error{code, j->error};
    }
    if (j->state == JobState::Cancelled) return Error{ErrorCode::Cancelled, "Cancelled"};
    return j;
}

Result<std::unique_ptr<WorkerSupervisor>> supervisor(const AiContext& c) {
    fs::path python;
    const std::string configured = c.pythonPath;
    if (!configured.empty()) python = fs::path(std::u8string(configured.begin(), configured.end()));
    auto opt = workerOptionsFor(c.env, python);
    if (!opt) return Error{opt.error().code, opt.error().message + " Run: acestudio-cli ai setup --profile <profile> --yes"};
    return std::make_unique<WorkerSupervisor>(std::move(opt).value());
}

// Picks a model for `task`: the --model id if given, else the first one installed.
Result<const ModelEntry*> pickModel(const ModelCatalog& cat, const ModelStore& store, const std::string& task,
                                    const std::optional<std::string>& wanted) {
    if (wanted) {
        const ModelEntry* m = cat.find(*wanted);
        if (!m || m->task != task) return Error{ErrorCode::NotFound, "No model '" + *wanted + "' for " + task};
        if (store.state(*m) != InstallState::Installed)
            return Error{ErrorCode::NotFound, m->name + " is not installed. Run: acestudio-cli ai download " + m->id};
        return m;
    }
    for (const ModelEntry* m : cat.forTask(task))
        if (m->kind == "model" && store.state(*m) == InstallState::Installed) return m;
    return Error{ErrorCode::NotFound, "No model for " + task + " is installed. See: acestudio-cli ai models"};
}

int saveOutput(Output& out, const Args& a, const Job* j, const std::string& name, const fs::path& target) {
    std::error_code ec;
    auto it = j->outputs.find(name);
    if (it == j->outputs.end()) return out.fail(kInternal, "The worker returned no '" + name + "' output");
    if (fs::exists(target, ec) && !a.force) return out.fail(kIo, target.string() + " already exists (use --force)");
    fs::copy_file(it->second, target, fs::copy_options::overwrite_existing, ec);
    if (ec) return out.fail(kIo, "Cannot write " + target.string() + ": " + ec.message());
    return kOk;
}

int cmdStatus(const Args&, Output& out, const AiContext& c) {
    json r = {{"workerDir", utf8(c.env.workerDir)}, {"venv", utf8(c.env.venvDir)}, {"models", utf8(c.env.modelsDir)}};
    std::string human = "Worker files: " + c.env.workerDir.string() + "\nAI data:      " + c.env.root.string() + "\n";
    std::error_code ec;
    if (!fs::exists(venvPython(c.env.venvDir), ec)) {
        auto py = findPython(c.pythonPath);
        r["runtime"] = "not set up";
        r["python"] = py ? py.value().version : std::string();
        human += "Runtime:      not set up" + std::string(py ? " (Python " + py.value().version + " found)" : " (no Python 3.10+ found)") +
                 "\n  Set up with: acestudio-cli ai setup --profile onnx-windows|onnx-cpu|diffusion-cuda|diffusion-cpu --yes\n";
        return out.success(r, human);
    }
    auto sup = supervisor(c);
    if (!sup) return out.fail(sup.error());
    auto& s = *sup.value();
    s.start();
    s.runUntil([&] { return s.state() == WorkerState::Ready || s.state() == WorkerState::Failed; }, 120s);
    if (s.state() != WorkerState::Ready) return out.fail(kIo, "AI worker failed to start: " + s.lastError());
    // Device details arrive a little after hello.
    s.runUntil([&] { return s.hello()["capabilities"].value("probed", false); }, 120s);
    const json& h = s.hello();
    r["runtime"] = "ready";
    r["python"] = h.value("python", std::string());
    r["capabilities"] = h["capabilities"];
    r["tasks"] = h["tasks"];
    human += "Runtime:      ready (worker " + h.value("version", std::string("?")) + ", Python " + h.value("python", std::string("?")) + ")\n";
    const json& caps = h["capabilities"];
    for (const auto& d : caps.value("devices", json::array()))
        human += "GPU:          " + d.value("name", std::string("?")) + " (" + std::to_string(d.value("vramMb", 0)) + " MB)\n";
    if (caps.value("devices", json::array()).empty()) human += "GPU:          none visible to PyTorch\n";
    if (!caps.value("onnxProviders", json::array()).empty()) human += "ONNX:         " + caps["onnxProviders"].dump() + "\n";
    for (auto& [name, t] : h["tasks"].items())
        human += "  " + name + ": " + (t.value("available", false) ? "available" : "unavailable — " + t.value("reason", std::string())) + "\n";
    s.stop();
    return out.success(r, human);
}

int cmdSetup(const Args& a, Output& out, const AiContext& c) {
    const auto profile = a.opt("profile");
    if (!profile) return out.fail(kUsage, "ai setup: --profile onnx-windows|onnx-cpu|diffusion-cuda|diffusion-cpu is required");
    if (!a.flags.count("yes"))
        return out.fail(kUsage, "ai setup downloads Python packages (diffusion profiles: several GB). Re-run with --yes to agree.");
    auto py = findPython(c.pythonPath);
    if (!py) return out.fail(py.error());
    if (!a.json) std::fprintf(stderr, "Creating the AI Python environment with Python %s...\n", py.value().version.c_str());
    auto venv = ensureVenv(py.value(), c.env.venvDir);
    if (!venv) return out.fail(venv.error());
    auto sup = supervisor(c);
    if (!sup) return out.fail(sup.error());
    auto j = runJob(*sup.value(), "runtime.install", {{"profile", *profile}}, a.json);
    if (!j) return out.fail(j.error());
    return out.success({{"profile", *profile}, {"venv", utf8(c.env.venvDir)}}, "Installed AI runtime profile " + *profile + "\n");
}

int cmdModels(const Args&, Output& out, const AiContext& c, const ModelCatalog& cat) {
    ModelStore store(c.env.modelsDir);
    json list = json::array();
    std::string human;
    for (const auto& m : cat.models()) {
        const InstallState st = store.state(m);
        list.push_back({{"id", m.id}, {"name", m.name}, {"task", m.task}, {"license", m.license.id},
                        {"commercialUse", m.license.commercialUse}, {"sizeBytes", m.knownSize()},
                        {"state", toString(st)}, {"approved", store.isApproved(m)}, {"pinned", m.pinned}});
        human += m.id + "\n    " + m.name + "\n    licence " + m.license.name + " · " +
                 (m.knownSize() ? mb(m.knownSize()) : std::string("size unknown")) + " · " + toString(st) + "\n";
    }
    return out.success({{"models", list}, {"modelsDir", utf8(c.env.modelsDir)}}, human);
}

int cmdDownload(const Args& a, Output& out, const AiContext& c, const ModelCatalog& cat) {
    if (a.positional.size() != 3) return out.fail(kUsage, "ai download <model-id> --accept-license <licence-id>");
    const ModelEntry* m = cat.find(a.positional[2]);
    if (!m) return out.fail(kIo, "Unknown model " + a.positional[2]);
    ModelStore store(c.env.modelsDir);
    if (!store.isApproved(*m)) {
        if (a.opt("accept-license") != m->license.id)
            return out.fail(kUsage, m->name + " is licensed under " + m->license.name + " (" + m->license.url + ").\n" +
                                        m->license.summary + "\n" + m->license.obligations +
                                        "\nTo accept, re-run with --accept-license " + m->license.id);
        if (auto st = store.approve(*m); !st) return out.fail(st.error());
    }
    auto params = store.downloadParams(*m);
    if (!params) return out.fail(params.error());
    auto sup = supervisor(c);
    if (!sup) return out.fail(sup.error());
    auto j = runJob(*sup.value(), "model.download", params.value(), a.json);
    if (!j) return out.fail(j.error());
    if (auto st = store.recordDownload(*m, j.value()->info.value("sha256", json::object())); !st) return out.fail(st.error());
    return out.success({{"id", m->id}, {"path", utf8(store.entryPath(*m))}, {"state", toString(store.state(*m))}},
                       "Installed " + m->name + " in " + store.dir(*m).string() + "\n");
}

int cmdImageTask(const Args& a, Output& out, const AiContext& c, const ModelCatalog& cat, const std::string& verb) {
    const std::string task = verb == "remove-bg" ? "background.remove" : verb == "upscale" ? "upscale.esrgan" : "image.generate";
    const auto outPath = a.opt("out");
    if (!outPath) return out.fail(kUsage, "ai " + verb + ": --out is required");
    std::error_code ec;
    if (fs::exists(*outPath, ec) && !a.force) return out.fail(kIo, *outPath + " already exists (use --force)");
    ModelStore store(c.env.modelsDir);
    auto model = pickModel(cat, store, task, a.opt("model"));
    if (!model) return out.fail(model.error());
    json params = {{"model", utf8(store.entryPath(*model.value()))}};
    if (a.flags.count("cpu")) params["device"] = "cpu";
    std::optional<Image> input;
    if (task != "image.generate") {
        if (a.positional.size() != 3) return out.fail(kUsage, "ai " + verb + " <input.png> --out <output.png>");
        auto img = readPng(a.positional[2]);
        if (!img) return out.fail(img.error());
        input = std::move(img).value();
        if (task == "background.remove" && a.opt("threshold")) params["threshold"] = std::stod(*a.opt("threshold"));
        if (task == "upscale.esrgan" && optInt(a, "tile")) params["tile"] = *optInt(a, "tile");
    } else {
        const auto prompt = a.opt("prompt");
        if (!prompt) return out.fail(kUsage, "ai generate: --prompt is required");
        params = {{"model_dir", utf8(store.entryPath(*model.value()))}, {"prompt", *prompt},
                  {"negative_prompt", a.opt("negative").value_or("")},
                  {"width", optInt(a, "width").value_or(1024)}, {"height", optInt(a, "height").value_or(1024)},
                  {"steps", optInt(a, "steps").value_or(30)}, {"seed", optInt(a, "seed").value_or(-1)}};
        if (a.flags.count("cpu")) params["device"] = "cpu";
        if (a.opt("guidance")) params["guidance"] = std::stod(*a.opt("guidance"));
        if (auto lora = a.opt("lora")) {
            const ModelEntry* l = cat.find(*lora);
            if (!l || l->kind != "lora" || store.state(*l) != InstallState::Installed)
                return out.fail(kIo, "LoRA " + *lora + " is not installed");
            params["lora"] = {{"path", utf8(store.entryPath(*l))}, {"scale", 1.0}};
        }
        if (auto px = a.opt("pixel")) {
            const std::size_t x = px->find('x');
            const auto w = x == std::string::npos ? std::nullopt : parseInt(px->substr(0, x));
            const auto h = x == std::string::npos ? std::nullopt : parseInt(px->substr(x + 1));
            if (!w || !h || *w < 1 || *h < 1)
                return out.fail(kUsage, "--pixel expects WIDTHxHEIGHT, e.g. 64x64");
            params["pixel"] = {{"width", *w}, {"height", *h}};
            if (optInt(a, "colors")) params["pixel"]["colors"] = *optInt(a, "colors");
        }
        if (auto init = a.opt("init")) {
            auto img = readPng(*init);
            if (!img) return out.fail(img.error());
            input = std::move(img).value();
            if (a.opt("strength")) params["strength"] = std::stod(*a.opt("strength"));
        }
    }
    auto sup = supervisor(c);
    if (!sup) return out.fail(sup.error());
    std::vector<std::pair<std::string, const Image*>> images;
    if (input) images.push_back({task == "image.generate" ? "init_image" : "input", &*input});
    auto j = runJob(*sup.value(), task, params, a.json, images, model.value());
    if (!j) return out.fail(j.error());
    const std::string outputName = task == "image.generate" ? "image0" : "image";
    if (int code = saveOutput(out, a, j.value(), outputName, *outPath); code != kOk) return code;
    return out.success({{"path", *outPath}, {"info", j.value()->info}}, "Wrote " + *outPath + "\n");
}

} // namespace

int cmdAi(const Args& a, Output& out) {
    if (a.positional.size() < 2) return out.fail(kUsage, "ai: expected status|setup|models|download|remove-bg|upscale|generate");
    auto c = context();
    if (!c) return out.fail(c.error());
    const std::string& verb = a.positional[1];
    if (verb == "status") return cmdStatus(a, out, c.value());
    if (verb == "setup") return cmdSetup(a, out, c.value());
    auto cat = ModelCatalog::load(c.value().env.catalogFile);
    if (!cat) return out.fail(cat.error());
    if (verb == "models") return cmdModels(a, out, c.value(), cat.value());
    if (verb == "download") return cmdDownload(a, out, c.value(), cat.value());
    if (verb == "remove-bg" || verb == "upscale" || verb == "generate") return cmdImageTask(a, out, c.value(), cat.value(), verb);
    return out.fail(kUsage, "ai: unknown subcommand '" + verb + "'");
}

} // namespace acestudio::cli
