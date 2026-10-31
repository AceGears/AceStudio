#pragma once

// Supervision of the Python AI worker (ADR-004).
//
// The supervisor owns the worker process, a FIFO job queue and the protocol
// (newline-delimited JSON-RPC 2.0 over stdin/stdout, see
// docs/ai-worker-protocol.md). It is single-threaded: call poll() regularly
// from the owning thread (the app uses a timer, the CLI a loop). The worker
// runs one job at a time; a crash, hang or out-of-memory kill restarts the
// worker without touching the editor.

#include "acestudio/core/Image.h"
#include "acestudio/core/Log.h"
#include "acestudio/core/Result.h"
#include "acestudio/core/Subprocess.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace acestudio::ai {

using JobId = std::string;
using Clock = std::chrono::steady_clock;

enum class JobState { Queued, Running, Succeeded, Failed, Cancelled };
enum class FailureKind { None, Error, OutOfMemory, MissingDependency, MissingModel, InvalidInput, WorkerCrashed, Unavailable };
enum class WorkerState { Stopped, Starting, Ready, Restarting, Failed };

const char* toString(JobState s) noexcept;
const char* toString(WorkerState s) noexcept;
const char* toString(FailureKind k) noexcept;
bool isFinished(JobState s) noexcept;

struct Job {
    JobId id;
    std::string task;
    std::string label;                         // shown in the queue UI
    nlohmann::json params;
    std::filesystem::path dir;                 // inputs and outputs live here
    JobState state = JobState::Queued;
    double progress = -1;                      // 0..1, or -1 when unknown
    std::string message;
    int step = 0;
    int steps = 0;
    std::map<std::string, std::filesystem::path> outputs;
    nlohmann::json info;
    std::string error;
    FailureKind failure = FailureKind::None;
    int attempts = 0;
    bool cancelRequested = false;
    Clock::time_point created{};
    Clock::time_point started{};
    Clock::time_point finished{};
};

struct WorkerOptions {
    ProcessSpec process;                       // how to launch the worker
    std::filesystem::path jobsRoot;            // job directories are created here
    std::chrono::milliseconds helloTimeout{std::chrono::seconds(90)};   // importing torch can be slow
    std::chrono::milliseconds cancelGrace{std::chrono::seconds(10)};    // then the worker is killed
    std::chrono::milliseconds restartDelay{std::chrono::milliseconds(500)};
    int maxCrashes = 3;                        // within crashWindow, then stop restarting
    std::chrono::milliseconds crashWindow{std::chrono::minutes(2)};
    int maxAttempts = 2;                       // a job that crashes the worker is retried once
};

struct Event {
    enum class Kind { WorkerState, Job, Log } kind = Kind::Log;
    JobId job;
    std::string text;
    LogLevel level = LogLevel::Info;
};

class WorkerSupervisor {
public:
    explicit WorkerSupervisor(WorkerOptions options);
    ~WorkerSupervisor();

    WorkerSupervisor(const WorkerSupervisor&) = delete;
    WorkerSupervisor& operator=(const WorkerSupervisor&) = delete;

    // Starts the worker (no-op if running). After Failed, start() resets
    // the crash counter and tries again.
    void start();
    // Asks the worker to shut down, then kills it after a short wait. A
    // running job becomes Cancelled; queued jobs stay queued.
    void stop();
    void restart();

    // Queues a job. Each `images` entry is written to the job directory as
    // a PNG and its path stored in params[name].
    Result<JobId> submit(const std::string& task, nlohmann::json params, std::string label = {},
                         const std::vector<std::pair<std::string, const Image*>>& images = {});
    Status cancel(const JobId& id);

    // Drives everything: reads worker output, handles exits and timeouts,
    // dispatches queued jobs. Non-blocking. Returns what changed.
    std::vector<Event> poll();
    // Polls until `pred` is true or the timeout passes (CLI and tests).
    template <typename Pred> bool runUntil(Pred pred, std::chrono::milliseconds timeout, std::vector<Event>* events = nullptr);

    WorkerState state() const noexcept { return m_state; }
    // Result of the worker's "hello": version, capabilities and tasks.
    const nlohmann::json& hello() const noexcept { return m_hello; }
    // Whether the connected worker offers `task`, and why not.
    bool taskAvailable(const std::string& task, std::string* reason = nullptr) const;
    // Why the worker is in the Failed state (last stderr lines included).
    const std::string& lastError() const noexcept { return m_lastError; }
    long long workerPid() const noexcept { return m_proc ? m_proc->pid() : -1; }
    int restarts() const noexcept { return m_restarts; }

    const Job* job(const JobId& id) const;
    std::vector<const Job*> jobs() const;        // submission order
    // Forgets finished jobs; with removeFiles, deletes their directories.
    void clearFinished(bool removeFiles);
    bool idle() const;                            // nothing queued or running

private:
    enum class Pending { Hello, Submit, Cancel, Shutdown };
    struct Request { Pending kind; JobId job; };

    void setState(WorkerState s, std::vector<Event>& ev);
    void launch(std::vector<Event>& ev);
    Status send(const std::string& method, nlohmann::json params, Pending kind, const JobId& job = {});
    void handleLine(const std::string& line, std::vector<Event>& ev);
    void handleResponse(const nlohmann::json& msg, std::vector<Event>& ev);
    void handleNotification(const std::string& method, const nlohmann::json& p, std::vector<Event>& ev);
    void onWorkerExit(int code, std::vector<Event>& ev);
    void finishJob(Job& job, JobState state, std::vector<Event>& ev);
    void dispatch(std::vector<Event>& ev);
    void killWorker();
    Job* findJob(const JobId& id);
    Job* runningJob();

    WorkerOptions m_opt;
    WorkerState m_state = WorkerState::Stopped;
    std::unique_ptr<Subprocess> m_proc;
    nlohmann::json m_hello = nlohmann::json::object();
    std::string m_lastError;
    std::deque<std::string> m_stderrTail;
    std::map<long long, Request> m_pending;
    long long m_nextRequest = 1;
    std::map<JobId, Job> m_jobs;
    std::vector<JobId> m_order;
    JobId m_running;
    std::optional<Clock::time_point> m_helloDeadline;
    std::optional<Clock::time_point> m_cancelDeadline;
    std::optional<Clock::time_point> m_restartAt;
    std::vector<Clock::time_point> m_crashes;
    bool m_hardCancel = false;      // we killed the worker on purpose
    bool m_stopping = false;
    int m_restarts = 0;
    unsigned m_jobCounter = 0;
};

template <typename Pred>
bool WorkerSupervisor::runUntil(Pred pred, std::chrono::milliseconds timeout, std::vector<Event>* events) {
    const auto deadline = Clock::now() + timeout;
    for (;;) {
        auto ev = poll();
        if (events) events->insert(events->end(), ev.begin(), ev.end());
        if (pred()) return true;
        if (Clock::now() >= deadline) return false;
        if (m_proc) (void)m_proc->waitForLine(std::chrono::milliseconds(20));
        else std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

// ---------------------------------------------------------------- runtime

struct PythonInfo {
    std::filesystem::path executable;
    std::vector<std::string> prefixArgs;   // e.g. {"-3.13"} for the Windows py launcher
    std::string version;                   // "3.12.4"
    int minor = 0;                         // 12 for 3.12
    int bits = 64;                         // pointer size: 32 for a 32-bit (x86) build
    // Newer than the AI packages support (see kNewestAiPythonMinor): the
    // worker runs, but some runtime profiles cannot be installed.
    bool newerThanAiPackages() const noexcept;
    // A 32-bit Python: ONNX Runtime and PyTorch publish 64-bit wheels only,
    // so no AI runtime profile can be installed.
    bool is32Bit() const noexcept { return bits == 32; }
};

// Newest Python minor version every pinned AI package publishes Windows
// wheels for (checked 2026-10-30: onnxruntime 1.31.1 and
// onnxruntime-directml 1.24.4 stop at 3.14; torch 2.15.0 has 3.15).
inline constexpr int kNewestAiPythonMinor = 14;

// Runs `python -c` to read the version and pointer size; requires >= 3.10.
Result<PythonInfo> probePython(const std::filesystem::path& exe, const std::vector<std::string>& prefixArgs = {});
// Configured path first. Otherwise prefers the newest 64-bit version the
// AI packages support (3.14 down to 3.10: "py -3.14" ... on Windows,
// python3.14 ... on POSIX), then any Python 3.10+ (py -3, python, python3).
Result<PythonInfo> findPython(const std::filesystem::path& configured = {});
// The interpreter inside a virtual environment folder.
std::filesystem::path venvPython(const std::filesystem::path& venvDir);
// Creates (or reuses) a virtual environment; one made from a different
// Python minor version is rebuilt. Blocking; up to a few minutes.
Result<std::filesystem::path> ensureVenv(const PythonInfo& base, const std::filesystem::path& venvDir);
// ProcessSpec that runs the worker package found in `workerDir`.
ProcessSpec workerProcess(const std::filesystem::path& python, const std::filesystem::path& workerDir, bool diagnostics = false);

} // namespace acestudio::ai

namespace acestudio::ai {

// Where AI files live. Everything is under the user data folder except the
// worker package, which ships next to the executable.
struct AiEnvironment {
    std::filesystem::path root;        // <data>/ai
    std::filesystem::path venvDir;     // <data>/ai/python
    std::filesystem::path modelsDir;   // setting ai.modelDirectory, else <data>/ai/models
    std::filesystem::path jobsDir;     // <data>/ai/jobs
    std::filesystem::path workerDir;   // folder containing acestudio_worker/
    std::filesystem::path catalogFile; // <workerDir>/catalog/models.json
};

// Worker search order: ACESTUDIO_WORKER_DIR, <exeDir>/ai_worker,
// <exeDir>/../share/acestudio/ai_worker, then the source tree of a
// development build.
Result<AiEnvironment> resolveAiEnvironment(const std::filesystem::path& dataDir, const std::string& modelDirSetting,
                                           const std::filesystem::path& exeDir);

// Worker options for this environment: the venv's Python if it exists.
// `python` overrides the interpreter (e.g. a configured path).
Result<WorkerOptions> workerOptionsFor(const AiEnvironment& env, const std::filesystem::path& python = {});

} // namespace acestudio::ai
