#include "acestudio/core/AiWorker.h"
#include "acestudio/core/FileUtil.h"

#include "acestudio/core/PngCodec.h"

#include <miniz.h>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <sstream>
#include <thread>

namespace fs = std::filesystem;
using nlohmann::json;

namespace acestudio::ai {

namespace {
constexpr int kProtocol = 1;
constexpr std::size_t kStderrTail = 40;

FailureKind kindFromWorker(const std::string& k) {
    if (k == "oom") return FailureKind::OutOfMemory;
    if (k == "missing_dependency") return FailureKind::MissingDependency;
    if (k == "missing_model") return FailureKind::MissingModel;
    if (k == "invalid_input") return FailureKind::InvalidInput;
    return FailureKind::Error;
}

std::string utf8(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}
fs::path fromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string randomSuffix() {
    static std::mt19937_64 rng{std::random_device{}()};
    static const char* hex = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < 6; ++i) s.push_back(hex[rng() & 15]);
    return s;
}
} // namespace

const char* toString(JobState s) noexcept {
    switch (s) {
    case JobState::Queued: return "queued";
    case JobState::Running: return "running";
    case JobState::Succeeded: return "succeeded";
    case JobState::Failed: return "failed";
    case JobState::Cancelled: return "cancelled";
    }
    return "?";
}
const char* toString(WorkerState s) noexcept {
    switch (s) {
    case WorkerState::Stopped: return "stopped";
    case WorkerState::Starting: return "starting";
    case WorkerState::Ready: return "ready";
    case WorkerState::Restarting: return "restarting";
    case WorkerState::Failed: return "failed";
    }
    return "?";
}
const char* toString(FailureKind k) noexcept {
    switch (k) {
    case FailureKind::None: return "none";
    case FailureKind::Error: return "error";
    case FailureKind::OutOfMemory: return "out_of_memory";
    case FailureKind::MissingDependency: return "missing_dependency";
    case FailureKind::MissingModel: return "missing_model";
    case FailureKind::InvalidInput: return "invalid_input";
    case FailureKind::WorkerCrashed: return "worker_crashed";
    case FailureKind::Unavailable: return "unavailable";
    }
    return "?";
}
bool isFinished(JobState s) noexcept {
    return s == JobState::Succeeded || s == JobState::Failed || s == JobState::Cancelled;
}

WorkerSupervisor::WorkerSupervisor(WorkerOptions options) : m_opt(std::move(options)) {}

WorkerSupervisor::~WorkerSupervisor() { stop(); }

void WorkerSupervisor::setState(WorkerState s, std::vector<Event>& ev) {
    if (s == m_state) return;
    m_state = s;
    ev.push_back({Event::Kind::WorkerState, {}, toString(s), LogLevel::Info});
}

void WorkerSupervisor::start() {
    if (m_proc) return;
    if (m_state == WorkerState::Failed) m_crashes.clear();
    std::vector<Event> ev;
    launch(ev);
}

void WorkerSupervisor::launch(std::vector<Event>& ev) {
    if (m_state == WorkerState::Restarting) ++m_restarts;
    m_restartAt.reset();
    m_pending.clear();
    m_stderrTail.clear();
    m_hello = json::object();
    m_hardCancel = false;
    auto p = Subprocess::start(m_opt.process);
    if (!p) {
        m_lastError = "Could not start the AI worker: " + p.error().message;
        ACE_LOG_ERROR("ai", m_lastError);
        setState(WorkerState::Failed, ev);
        return;
    }
    m_proc = std::move(p).value();
    ACE_LOG_INFO("ai", "Started AI worker (pid " + std::to_string(m_proc->pid()) + ")");
    setState(WorkerState::Starting, ev);
    m_helloDeadline = Clock::now() + m_opt.helloTimeout;
    if (auto st = send("hello", {{"protocol", kProtocol}}, Pending::Hello); !st)
        ACE_LOG_WARN("ai", "hello failed: " + st.error().message);
}

void WorkerSupervisor::killWorker() {
    if (!m_proc) return;
    m_proc->kill();
    (void)m_proc->wait(std::chrono::seconds(5));
}

void WorkerSupervisor::stop() {
    m_restartAt.reset();
    if (!m_proc) {
        if (m_state != WorkerState::Failed) m_state = WorkerState::Stopped;
        return;
    }
    m_stopping = true;
    (void)send("shutdown", json::object(), Pending::Shutdown);
    m_proc->closeStdin();
    if (!m_proc->wait(std::chrono::seconds(3))) killWorker();
    if (Job* j = runningJob()) {
        j->error = "Stopped";
        finishJob(*j, JobState::Cancelled, m_deferred);
    }
    m_proc.reset();
    m_pending.clear();
    m_stopping = false;
    m_state = WorkerState::Stopped;
}

void WorkerSupervisor::restart() {
    stop();
    m_crashes.clear();
    start();
}

Status WorkerSupervisor::send(const std::string& method, json params, Pending kind, const JobId& job) {
    if (!m_proc) return Error{ErrorCode::IoError, "AI worker is not running"};
    const long long id = m_nextRequest++;
    json msg = {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}};
    m_pending[id] = {kind, job};
    return m_proc->writeLine(msg.dump());
}

Result<JobId> WorkerSupervisor::submit(const std::string& task, json params, std::string label,
                                       const std::vector<std::pair<std::string, const Image*>>& images) {
    if (task.empty()) return Error{ErrorCode::InvalidArgument, "No task given"};
    if (!params.is_object()) params = json::object();
    Job job;
    job.id = "j" + std::to_string(++m_jobCounter) + "-" + randomSuffix();
    job.task = task;
    job.label = label.empty() ? task : std::move(label);
    job.dir = m_opt.jobsRoot / job.id;
    std::error_code ec;
    fs::create_directories(job.dir, ec);
    if (ec) return Error{ErrorCode::IoError, "Cannot create job folder " + job.dir.string() + ": " + ec.message()};
    for (const auto& [name, img] : images) {
        if (!img) continue;
        const fs::path p = job.dir / ("in_" + name + ".png");
        if (auto st = writePng(*img, p); !st) return st.error();
        params[name] = utf8(p);
    }
    job.params = std::move(params);
    job.created = Clock::now();
    const JobId id = job.id;
    m_order.push_back(id);
    m_jobs.emplace(id, std::move(job));
    return id;
}

Status WorkerSupervisor::cancel(const JobId& id) {
    Job* j = findJob(id);
    if (!j) return Error{ErrorCode::NotFound, "No such job"};
    if (isFinished(j->state)) return {};
    j->cancelRequested = true;
    if (j->state == JobState::Queued) {
        finishJob(*j, JobState::Cancelled, m_deferred);   // reported by the next poll()
        return {};
    }
    if (!m_cancelDeadline) {
        m_cancelStarted = Clock::now();
        m_cancelDeadline = m_cancelStarted + m_opt.cancelGrace;
        j->message = j->uninterruptible ? "Cancelling after the model has loaded…" : "Cancelling…";
        (void)send("job.cancel", {{"job", id}}, Pending::Cancel, id);
    }
    return {};
}

std::vector<Event> WorkerSupervisor::poll() {
    std::vector<Event> ev = std::exchange(m_deferred, {});
    if (m_proc) {
        for (auto& line : m_proc->takeLines(Subprocess::Stream::Stderr)) {
            m_stderrTail.push_back(line);
            if (m_stderrTail.size() > kStderrTail) m_stderrTail.pop_front();
            ACE_LOG_DEBUG("ai.worker", line);
            ev.push_back({Event::Kind::Log, {}, line, LogLevel::Debug});
        }
        for (auto& line : m_proc->takeLines(Subprocess::Stream::Stdout)) handleLine(line, ev);
        if (auto code = m_proc->exitCode()) {
            // Pick up anything that arrived between the reads above and exit.
            for (auto& line : m_proc->takeLines(Subprocess::Stream::Stdout)) handleLine(line, ev);
            for (auto& line : m_proc->takeLines(Subprocess::Stream::Stderr)) {
                m_stderrTail.push_back(line);
                if (m_stderrTail.size() > kStderrTail) m_stderrTail.pop_front();
            }
            onWorkerExit(*code, ev);
        }
    }
    const auto now = Clock::now();
    if (m_proc && m_state == WorkerState::Starting && m_helloDeadline && now > *m_helloDeadline) {
        ACE_LOG_WARN("ai", "AI worker did not answer hello in time; restarting it");
        m_lastError = "The AI worker did not start in time.";
        killWorker();
        onWorkerExit(-1, ev);
    }
    if (m_proc && m_cancelDeadline && now > *m_cancelDeadline) {
        Job* r = runningJob();
        if (r && r->uninterruptible && now < m_cancelStarted + m_opt.uninterruptibleGrace) {
            // Still loading: wait for it (the cancel is honoured right after).
            r->message = "Cancelling after the model has loaded…";
            m_cancelDeadline = now + std::chrono::seconds(1);
        } else {
        // The job ignored cancellation (stuck in native code): kill the
        // worker. The job ends Cancelled and the worker restarts.
        ACE_LOG_WARN("ai", "Job did not stop after cancel; restarting the AI worker");
        m_hardCancel = true;
        killWorker();
        if (auto code = m_proc->exitCode()) onWorkerExit(*code, ev);
        }
    }
    if (!m_proc && m_state == WorkerState::Restarting && m_restartAt && now >= *m_restartAt) {
        launch(ev);
    }
    dispatch(ev);
    return ev;
}

void WorkerSupervisor::handleLine(const std::string& line, std::vector<Event>& ev) {
    json msg = json::parse(line, nullptr, false);
    if (msg.is_discarded() || !msg.is_object()) {
        ACE_LOG_WARN("ai", "Ignoring malformed worker output: " + line.substr(0, 200));
        return;
    }
    if (msg.contains("id") && !msg["id"].is_null() && (msg.contains("result") || msg.contains("error"))) {
        handleResponse(msg, ev);
    } else if (msg.contains("method") && msg["method"].is_string()) {
        handleNotification(msg["method"].get<std::string>(), msg.value("params", json::object()), ev);
    }
}

void WorkerSupervisor::handleResponse(const json& msg, std::vector<Event>& ev) {
    if (!msg["id"].is_number_integer()) return;
    auto it = m_pending.find(msg["id"].get<long long>());
    if (it == m_pending.end()) return;
    const Request req = it->second;
    m_pending.erase(it);
    const bool isError = msg.contains("error");
    const std::string errText = isError ? msg["error"].value("message", std::string("error")) : std::string();
    switch (req.kind) {
    case Pending::Hello:
        if (isError || !msg["result"].is_object() || msg["result"].value("protocol", 0) != kProtocol) {
            m_lastError = "The AI worker speaks an incompatible protocol" + (isError ? ": " + errText : std::string());
            ACE_LOG_ERROR("ai", m_lastError);
            m_crashes.assign(static_cast<std::size_t>(m_opt.maxCrashes), Clock::now());   // do not retry
            killWorker();
            return;
        }
        m_hello = msg["result"];
        m_helloDeadline.reset();
        ACE_LOG_INFO("ai", "AI worker ready: " + m_hello.value("version", std::string("?")) + " on Python " +
                               m_hello.value("python", std::string("?")));
        setState(WorkerState::Ready, ev);
        break;
    case Pending::Submit:
        if (isError) {
            if (Job* j = findJob(req.job)) {
                const int code = msg["error"].value("code", 0);
                j->error = errText;
                j->failure = code == -32003 ? FailureKind::Unavailable : FailureKind::Error;
                finishJob(*j, JobState::Failed, ev);
            }
        }
        break;
    case Pending::Cancel:
    case Pending::Shutdown:
        break;
    }
}

void WorkerSupervisor::handleNotification(const std::string& method, const json& p, std::vector<Event>& ev) {
    if (method == "capabilities") {
        // Device details arrive after hello (probing torch can be slow).
        if (p.is_object()) m_hello["capabilities"] = p;
        ev.push_back({Event::Kind::WorkerState, {}, "capabilities", LogLevel::Info});
        return;
    }
    if (method == "log") {
        const std::string level = p.value("level", std::string("info"));
        const std::string text = p.value("message", std::string());
        const LogLevel lv = level == "error" ? LogLevel::Error : level == "warning" ? LogLevel::Warning : LogLevel::Info;
        ACE_LOG(lv, "ai.worker", text);
        ev.push_back({Event::Kind::Log, {}, text, lv});
        return;
    }
    Job* j = findJob(p.value("job", std::string()));
    if (!j || j->state != JobState::Running) return;
    if (method == "job.progress") {
        if (p.contains("progress") && p["progress"].is_number()) j->progress = p["progress"].get<double>();
        if (!j->cancelRequested) j->message = p.value("message", j->message);
        const bool wasUninterruptible = j->uninterruptible;
        j->uninterruptible = p.value("uninterruptible", false);
        if (j->cancelRequested && m_cancelDeadline && wasUninterruptible && !j->uninterruptible && j->id == m_running)
            m_cancelDeadline = Clock::now() + m_opt.cancelGrace;   // the stage ended: the usual grace from here
        j->step = p.value("step", j->step);
        j->steps = p.value("steps", j->steps);
        if (p.contains("preview") && p["preview"].is_string()) {
            // Only files inside the job's own folder are accepted.
            std::error_code ec;
            const fs::path pv = fs::weakly_canonical(fromUtf8(p["preview"].get<std::string>()), ec);
            const fs::path dir = fs::weakly_canonical(j->dir, ec);
            const auto rel = pv.lexically_relative(dir);
            if (!ec && !rel.empty() && *rel.begin() != "..") {
                j->preview = pv;
                ++j->previewSerial;
            }
        }
        ev.push_back({Event::Kind::Job, j->id, {}, LogLevel::Info});
    } else if (method == "job.done") {
        j->outputs.clear();
        if (p.contains("outputs") && p["outputs"].is_object())
            for (auto& [k, v] : p["outputs"].items())
                if (v.is_string()) j->outputs[k] = fromUtf8(v.get<std::string>());
        j->info = p.value("info", json::object());
        j->progress = 1.0;
        j->message.clear();
        finishJob(*j, JobState::Succeeded, ev);
    } else if (method == "job.failed") {
        j->error = p.value("error", std::string("failed"));
        j->failure = kindFromWorker(p.value("kind", std::string("error")));
        finishJob(*j, JobState::Failed, ev);
    } else if (method == "job.cancelled") {
        finishJob(*j, JobState::Cancelled, ev);
    }
}

void WorkerSupervisor::finishJob(Job& job, JobState state, std::vector<Event>& ev) {
    job.state = state;
    job.finished = Clock::now();
    if (state == JobState::Cancelled) job.message = "Cancelled";
    if (job.id == m_running) {
        m_running.clear();
        m_cancelDeadline.reset();
    }
    std::string timing;
    if (job.started != Clock::time_point{}) {
        // Wall time from dispatch to the worker's answer; used for benchmarks.
        const double s = std::chrono::duration<double>(job.finished - job.started).count();
        char buf[32];
        std::snprintf(buf, sizeof buf, " in %.1f s", s);
        timing = buf;
    }
    const std::string summary = job.label + ": " + toString(state) + timing + (job.error.empty() ? "" : " — " + job.error);
    if (state == JobState::Failed) ACE_LOG_WARN("ai", summary); else ACE_LOG_INFO("ai", summary);
    ev.push_back({Event::Kind::Job, job.id, {}, LogLevel::Info});
}

void WorkerSupervisor::onWorkerExit(int code, std::vector<Event>& ev) {
    m_proc.reset();
    m_pending.clear();
    m_helloDeadline.reset();
    std::ostringstream tail;
    for (const auto& l : m_stderrTail) tail << "\n" << l;
    const bool intentional = m_hardCancel || m_stopping;
    if (Job* j = runningJob()) {
        if (m_hardCancel || j->cancelRequested) {
            finishJob(*j, JobState::Cancelled, ev);
        } else if (j->attempts < m_opt.maxAttempts) {
            j->state = JobState::Queued;            // retry once on a fresh worker
            j->message = "AI worker crashed; retrying";
            j->progress = -1;
            m_running.clear();
            m_cancelDeadline.reset();
            ev.push_back({Event::Kind::Job, j->id, {}, LogLevel::Info});
        } else {
            const bool killed = code == -9 || code == 137;   // SIGKILL: usually the OS out-of-memory killer
            j->error = "The AI worker crashed (exit code " + std::to_string(code) + ") while running this job." +
                       (killed ? std::string(" The system may have run out of memory.") : std::string()) +
                       (m_stderrTail.empty() ? std::string() : "\nLast output:" + tail.str());
            j->failure = FailureKind::WorkerCrashed;
            finishJob(*j, JobState::Failed, ev);
        }
    }
    m_hardCancel = false;
    if (intentional) {
        setState(WorkerState::Restarting, ev);
        m_restartAt = Clock::now();
        return;
    }
    ACE_LOG_WARN("ai", "AI worker exited unexpectedly (code " + std::to_string(code) + ")" + tail.str());
    const auto now = Clock::now();
    m_crashes.push_back(now);
    m_crashes.erase(std::remove_if(m_crashes.begin(), m_crashes.end(),
                                   [&](Clock::time_point t) { return now - t > m_opt.crashWindow; }),
                    m_crashes.end());
    if (static_cast<int>(m_crashes.size()) >= m_opt.maxCrashes) {
        if (m_lastError.empty() || m_state != WorkerState::Starting)
            m_lastError = "The AI worker keeps crashing (exit code " + std::to_string(code) + ")." +
                          (m_stderrTail.empty() ? std::string() : "\nLast output:" + tail.str());
        // Nothing can run: fail queued jobs rather than leave them hanging.
        for (auto& id : m_order) {
            Job& q = m_jobs.at(id);
            if (q.state == JobState::Queued) {
                q.error = "The AI worker is not available.";
                q.failure = FailureKind::WorkerCrashed;
                finishJob(q, JobState::Failed, ev);
            }
        }
        setState(WorkerState::Failed, ev);
        return;
    }
    setState(WorkerState::Restarting, ev);
    m_restartAt = now + m_opt.restartDelay * static_cast<long>(m_crashes.size());
}

void WorkerSupervisor::dispatch(std::vector<Event>& ev) {
    if (!m_proc || m_state != WorkerState::Ready || !m_running.empty()) return;
    for (const auto& id : m_order) {
        Job& j = m_jobs.at(id);
        if (j.state != JobState::Queued) continue;
        j.state = JobState::Running;
        j.attempts++;
        j.started = Clock::now();
        j.progress = -1;
        j.message = "Starting…";
        m_running = id;
        const std::string dir = utf8(j.dir);
        if (auto st = send("job.submit", {{"job", id}, {"task", j.task}, {"params", j.params}, {"dir", dir}},
                           Pending::Submit, id);
            !st) {
            // The pipe broke: the exit handler will requeue it.
            ACE_LOG_WARN("ai", "Could not send job: " + st.error().message);
        }
        ev.push_back({Event::Kind::Job, id, {}, LogLevel::Info});
        return;
    }
}

bool WorkerSupervisor::taskAvailable(const std::string& task, std::string* reason) const {
    const json* t = nullptr;
    if (m_hello.contains("tasks") && m_hello["tasks"].contains(task)) t = &m_hello["tasks"][task];
    if (!t) {
        if (reason) *reason = m_state == WorkerState::Ready ? "Not offered by this AI worker" : "The AI worker is not running";
        return false;
    }
    if (reason) *reason = t->value("reason", std::string());
    return t->value("available", false);
}

Job* WorkerSupervisor::findJob(const JobId& id) {
    auto it = m_jobs.find(id);
    return it == m_jobs.end() ? nullptr : &it->second;
}
const Job* WorkerSupervisor::job(const JobId& id) const {
    auto it = m_jobs.find(id);
    return it == m_jobs.end() ? nullptr : &it->second;
}
Job* WorkerSupervisor::runningJob() { return m_running.empty() ? nullptr : findJob(m_running); }

std::vector<const Job*> WorkerSupervisor::jobs() const {
    std::vector<const Job*> out;
    for (const auto& id : m_order) out.push_back(&m_jobs.at(id));
    return out;
}

bool WorkerSupervisor::idle() const {
    return std::none_of(m_order.begin(), m_order.end(), [&](const JobId& id) { return !isFinished(m_jobs.at(id).state); });
}

void WorkerSupervisor::clearFinished(bool removeFiles) {
    std::vector<JobId> keep;
    for (const auto& id : m_order) {
        const Job& j = m_jobs.at(id);
        if (!isFinished(j.state)) { keep.push_back(id); continue; }
        if (removeFiles) {
            std::error_code ec;
            fs::remove_all(j.dir, ec);
        }
        m_jobs.erase(id);
    }
    m_order = std::move(keep);
}

// ---------------------------------------------------------------- runtime

Result<PythonInfo> probePython(const fs::path& exe, const std::vector<std::string>& prefixArgs) {
    ProcessSpec spec;
    spec.program = exe;
    spec.args = prefixArgs;
    spec.args.insert(spec.args.end(), {"-c", "import struct, sys; print('%d.%d.%d %d' % (sys.version_info[:3] + (struct.calcsize('P') * 8,)))"});
    spec.env = {{"PYTHONIOENCODING", "utf-8"}};
    auto p = Subprocess::start(spec);
    if (!p) return p.error();
    auto code = p.value()->wait(std::chrono::seconds(30));
    if (!code) return Error{ErrorCode::IoError, exe.string() + " did not respond"};
    auto lines = p.value()->takeLines(Subprocess::Stream::Stdout);
    if (*code != 0 || lines.empty()) return Error{ErrorCode::IoError, exe.string() + " is not a working Python"};
    // "3.12.4 64"
    std::string line = lines.back();
    int bits = 64;
    if (auto sp = line.find(' '); sp != std::string::npos) {
        int b = 0;
        if (std::from_chars(line.data() + sp + 1, line.data() + line.size(), b).ec == std::errc() && (b == 32 || b == 64))
            bits = b;
        line.resize(sp);
    }
    PythonInfo info{exe, prefixArgs, line, 0, bits};
    std::error_code pec;
    for (fs::directory_iterator it(exe.parent_path(), pec), end; !pec && it != end; it.increment(pec)) {
        const std::string n = it->path().filename().string();
        if (n.rfind("python", 0) == 0 && it->path().extension() == "._pth") info.embeddable = true;
    }
    // "3.12.4" -> major 3, minor 12
    int major = 0, minor = 0;
    const char* b = info.version.data();
    const char* e = b + info.version.size();
    auto r1 = std::from_chars(b, e, major);
    bool parsed = r1.ec == std::errc() && r1.ptr < e && *r1.ptr == '.';
    if (parsed) parsed = std::from_chars(r1.ptr + 1, e, minor).ec == std::errc();
    if (!parsed || major != 3 || minor < 10)
        return Error{ErrorCode::UnsupportedVersion, "Python " + info.version + " found; AceStudio needs Python 3.10 or newer"};
    info.minor = minor;
    return info;
}

fs::path bundledPython() {
#ifdef _WIN32
    std::error_code ec;
    const fs::path p = executableDir() / "python" / "python.exe";
    if (fs::exists(p, ec)) return p;
#endif
    return {};
}

Result<PythonInfo> findPython(const fs::path& configured) {
    std::string tried;
    auto attempt = [&](const fs::path& exe, std::vector<std::string> pre) -> std::optional<PythonInfo> {
        auto r = probePython(exe, pre);
        if (r) return r.value();
        tried += "\n  " + exe.string() + ": " + r.error().message;
        return std::nullopt;
    };
    if (!configured.empty())
        if (auto r = attempt(configured, {})) return *r;
    // The Python bundled with the installer: a known version that the AI
    // packages support.
    if (const fs::path b = bundledPython(); !b.empty())
        if (auto r = attempt(b, {}); r && !r->is32Bit()) return *r;
    // First choice: a version the AI packages support, newest first.
    for (int minor = kNewestAiPythonMinor; minor >= 10; --minor) {
#ifdef _WIN32
        if (auto py = findExecutable("py")) {
            const std::string flag = "-3." + std::to_string(minor);
            auto r = probePython(*py, {flag});   // silently skip versions that are not installed
            if (r && !r.value().is32Bit()) return r.value();
        }
#else
        if (auto exe = findExecutable("python3." + std::to_string(minor)))
            if (auto r = probePython(*exe, {}); r && !r.value().is32Bit()) return r.value();
#endif
    }
    // Otherwise any Python 3.10+, possibly newer than the AI packages support.
#ifdef _WIN32
    if (auto py = findExecutable("py"))
        if (auto r = attempt(*py, {"-3"})) return *r;
    for (const char* name : {"python", "python3"})
#else
    for (const char* name : {"python3", "python"})
#endif
        if (auto exe = findExecutable(name))
            if (auto r = attempt(*exe, {})) return *r;
    return Error{ErrorCode::NotFound, "No suitable Python (3.10+) was found." + tried};
}

bool PythonInfo::newerThanAiPackages() const noexcept { return minor > kNewestAiPythonMinor; }

fs::path venvPython(const fs::path& venvDir) {
#ifdef _WIN32
    // A copy of the bundled embeddable Python keeps python.exe at the top.
    std::error_code ec;
    if (!fs::exists(venvDir / "Scripts" / "python.exe", ec) && fs::exists(venvDir / "python.exe", ec)) return venvDir / "python.exe";
    return venvDir / "Scripts" / "python.exe";
#else
    return venvDir / "bin" / "python";
#endif
}

namespace {

Result<int> runQuiet(const ProcessSpec& spec, std::chrono::seconds timeout, std::string* err) {
    auto p = Subprocess::start(spec);
    if (!p) return p.error();
    auto code = p.value()->wait(timeout);
    if (err) {
        for (auto& l : p.value()->takeLines(Subprocess::Stream::Stdout)) *err += "\n" + l;
        for (auto& l : p.value()->takeLines(Subprocess::Stream::Stderr)) *err += "\n" + l;
    }
    if (!code) return Error{ErrorCode::IoError, spec.program.string() + " did not finish"};
    return *code;
}

// Unpacks a wheel (a ZIP) into `dir`, refusing paths that leave it.
Status unpackWheel(const fs::path& wheel, const fs::path& dir) {
    auto bytes = readFileBytes(wheel, 64u << 20);
    if (!bytes) return bytes.error();
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, bytes.value().data(), bytes.value().size(), 0))
        return Error{ErrorCode::CorruptData, wheel.string() + " is not a readable wheel"};
    std::unique_ptr<mz_zip_archive, void (*)(mz_zip_archive*)> guard(&zip, [](mz_zip_archive* z) { mz_zip_reader_end(z); });
    const mz_uint n = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < n; ++i) {
        mz_zip_archive_file_stat st{};
        if (!mz_zip_reader_file_stat(&zip, i, &st)) return Error{ErrorCode::CorruptData, "Damaged wheel " + wheel.string()};
        const std::string name = st.m_filename;
        if (name.empty() || name.back() == '/') continue;
        if (name.front() == '/' || name.find(':') != std::string::npos || name.find("..") != std::string::npos || name.find('\\') != std::string::npos)
            return Error{ErrorCode::CorruptData, "Unsafe path in wheel: " + name};
        const fs::path target = dir / fs::path(std::u8string(name.begin(), name.end()));
        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);
        if (!mz_zip_reader_extract_to_file(&zip, i, target.string().c_str(), 0))
            return Error{ErrorCode::IoError, "Cannot unpack " + name + " from " + wheel.string()};
    }
    return {};
}

// The embeddable Python as a private environment: a copy in the user's data
// folder (the install folder may not be writable), without its ._pth file so
// it searches paths like a regular install (site-packages, PYTHONPATH), and
// with pip unpacked from the wheel bundled next to it.
Result<fs::path> copyEmbeddable(const PythonInfo& base, const fs::path& venvDir) {
    std::error_code ec;
    const fs::path from = base.executable.parent_path();
    fs::create_directories(venvDir, ec);
    fs::copy(from, venvDir, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) return Error{ErrorCode::IoError, "Cannot copy the bundled Python to " + venvDir.string() + ": " + ec.message()};
    fs::path wheel;
    for (fs::directory_iterator it(venvDir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string n = it->path().filename().string();
        if (it->path().extension() == "._pth") fs::remove(it->path(), ec);
        if (n.rfind("pip-", 0) == 0 && it->path().extension() == ".whl") wheel = it->path();
        ec.clear();
    }
    const fs::path py = venvDir / "python.exe";
    if (wheel.empty()) return Error{ErrorCode::NotFound, "The bundled Python has no pip wheel; reinstall AceStudio"};
    // pip is pure Python: unpacking its wheel into site-packages installs it.
    if (auto st = unpackWheel(wheel, venvDir / "Lib" / "site-packages"); !st) return st.error();
    ProcessSpec spec;
    spec.program = py;
    spec.args = {"-m", "pip", "--version"};
    spec.env = {{"PYTHONIOENCODING", "utf-8"}, {"PYTHONNOUSERSITE", "1"}};
    std::string err;
    auto code = runQuiet(spec, std::chrono::minutes(2), &err);
    if (!code) return code.error();
    if (code.value() != 0) return Error{ErrorCode::IoError, "pip does not run in the bundled Python." + err};
    auto ok = probePython(py);
    if (!ok) return ok.error();
    return py;
}

} // namespace

Result<fs::path> ensureVenv(const PythonInfo& base, const fs::path& venvDir) {
    const fs::path py = venvPython(venvDir);
    std::error_code ec;
    if (fs::exists(py, ec)) {
        auto existing = probePython(py);
        // Reuse it only if it was made from the same Python minor version;
        // otherwise (e.g. it was built on a Python too new for the AI
        // packages) rebuild it. It is AceStudio's own folder: no user data.
        if (existing && existing.value().minor == base.minor && existing.value().bits == base.bits) return py;
        ACE_LOG_INFO("ai", "Rebuilding the AI Python environment for Python " + base.version);
        fs::remove_all(venvDir, ec);
        if (ec) return Error{ErrorCode::IoError, "Cannot replace " + venvDir.string() + ": " + ec.message()};
    }
    if (base.embeddable) return copyEmbeddable(base, venvDir);
    ProcessSpec spec;
    spec.program = base.executable;
    spec.args = base.prefixArgs;
    spec.args.insert(spec.args.end(), {"-m", "venv", utf8(venvDir)});
    auto p = Subprocess::start(spec);
    if (!p) return p.error();
    auto code = p.value()->wait(std::chrono::minutes(5));
    if (!code || *code != 0) {
        std::string out;
        for (auto& l : p.value()->takeLines(Subprocess::Stream::Stderr)) out += "\n" + l;
        return Error{ErrorCode::IoError, "Creating the Python environment failed." + out};
    }
    auto ok = probePython(py);
    if (!ok) return ok.error();
    return py;
}

ProcessSpec workerProcess(const fs::path& python, const fs::path& workerDir, bool diagnostics) {
    ProcessSpec spec;
    spec.program = python;
    spec.args = {"-u", "-m", "acestudio_worker"};
    spec.workingDir = workerDir;
    spec.env = {{"PYTHONPATH", utf8(workerDir)},
                {"PYTHONIOENCODING", "utf-8"},
                {"PYTHONUNBUFFERED", "1"},
                {"PYTHONNOUSERSITE", "1"},   // ignore the user's site-packages
                {"ACESTUDIO_WORKER_DIAG", diagnostics ? "1" : ""}};
    return spec;
}

} // namespace acestudio::ai

namespace acestudio::ai {

Result<AiEnvironment> resolveAiEnvironment(const fs::path& dataDir, const std::string& modelDirSetting, const fs::path& exeDir) {
    AiEnvironment env;
    env.root = dataDir / "ai";
    env.venvDir = env.root / "python";
    env.modelsDir = modelDirSetting.empty() ? env.root / "models" : fs::path(std::u8string(modelDirSetting.begin(), modelDirSetting.end()));
    env.jobsDir = env.root / "jobs";
    std::vector<fs::path> candidates;
#ifdef _WIN32
    wchar_t buf[4096];
    const DWORD n = GetEnvironmentVariableW(L"ACESTUDIO_WORKER_DIR", buf, 4096);
    if (n > 0 && n < 4096) candidates.emplace_back(std::wstring(buf, n));
#else
    if (const char* e = std::getenv("ACESTUDIO_WORKER_DIR")) candidates.emplace_back(e);
#endif
    if (!exeDir.empty()) {
        candidates.push_back(exeDir / "ai_worker");
        candidates.push_back(exeDir / ".." / "share" / "acestudio" / "ai_worker");
    }
#ifdef ACESTUDIO_DEV_SOURCE_DIR
    candidates.emplace_back(ACESTUDIO_DEV_SOURCE_DIR "/ai_worker");
#endif
    std::error_code ec;
    for (const auto& c : candidates)
        if (fs::exists(c / "acestudio_worker" / "__init__.py", ec)) {
            env.workerDir = fs::weakly_canonical(c, ec);
            if (ec) env.workerDir = c;
            break;
        }
    if (env.workerDir.empty()) return Error{ErrorCode::NotFound, "The AI worker files (ai_worker folder) were not found next to AceStudio."};
    env.catalogFile = env.workerDir / "catalog" / "models.json";
    return env;
}

int pruneJobFolders(const fs::path& jobsRoot, std::chrono::hours maxAge) {
    std::error_code ec;
    if (!fs::is_directory(jobsRoot, ec)) return 0;
    const auto cutoff = fs::file_time_type::clock::now() - maxAge;
    auto isJobName = [](const std::string& n) {
        if (n.size() < 4 || n[0] != 'j') return false;
        const auto dash = n.find('-');
        if (dash == std::string::npos || dash < 2) return false;
        for (std::size_t i = 1; i < dash; ++i)
            if (n[i] < '0' || n[i] > '9') return false;
        return dash + 1 < n.size();
    };
    int removed = 0;
    for (fs::directory_iterator it(jobsRoot, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec) || !isJobName(it->path().filename().string())) continue;
        auto newest = it->last_write_time(ec);
        for (fs::recursive_directory_iterator f(it->path(), ec), fe; !ec && f != fe; f.increment(ec)) {
            const auto t = f->last_write_time(ec);
            if (!ec && t > newest) newest = t;
        }
        ec.clear();
        if (newest >= cutoff) continue;
        std::error_code rm;
        fs::remove_all(it->path(), rm);
        if (!rm) ++removed;
    }
    return removed;
}

Result<WorkerOptions> workerOptionsFor(const AiEnvironment& env, const fs::path& python) {
    fs::path py = python;
    std::error_code ec;
    if (py.empty()) {
        py = venvPython(env.venvDir);
        if (!fs::exists(py, ec)) return Error{ErrorCode::NotFound, "The AI runtime is not set up yet."};
    }
    WorkerOptions o;
    o.process = workerProcess(py, env.workerDir);
    o.jobsRoot = env.jobsDir;
    return o;
}

} // namespace acestudio::ai
