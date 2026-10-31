#include <catch2/catch_test_macros.hpp>

#include "TestSupport.h"
#include "acestudio/core/AiWorker.h"
#include "acestudio/core/PngCodec.h"

#include <fstream>
#include <iostream>

#ifndef _WIN32
#include <csignal>
#endif

using namespace acestudio;
using namespace acestudio::ai;
using namespace std::chrono_literals;

namespace {

// The tests drive the real Python worker with its diagnostic tasks. They
// are skipped when no Python 3.10+ is installed (e.g. under Wine).
// Returns a reference: callers take begin()/end() of prefixArgs, which must
// come from the same object (a by-value copy here once crashed on Windows).
const std::optional<PythonInfo>& python() {
    static const std::optional<PythonInfo> cached = [] () -> std::optional<PythonInfo> {
        auto p = findPython();
        if (!p) return std::nullopt;
        return p.value();
    }();
    return cached;
}

struct Fixture {
    TempDir dir;
    std::unique_ptr<WorkerSupervisor> sup;
    explicit Fixture(std::function<void(WorkerOptions&)> tweak = {}) {
        WorkerOptions o;
        o.process = workerProcess(python()->executable, ACESTUDIO_SOURCE_DIR "/ai_worker", true);
        o.process.args.insert(o.process.args.begin(), python()->prefixArgs.begin(), python()->prefixArgs.end());
        o.jobsRoot = dir.path() / "jobs";
        o.restartDelay = 50ms;
        if (tweak) tweak(o);
        sup = std::make_unique<WorkerSupervisor>(std::move(o));
    }
    bool ready() {
        sup->start();
        const bool ok = sup->runUntil([&] { return sup->state() == WorkerState::Ready; }, 60s);
        // Printed (and picked up by CI annotations) when the worker does not come up.
        if (!ok) std::cerr << "AIDIAG state=" << toString(sup->state()) << " error=" << sup->lastError() << "\n";
        return ok;
    }
    bool finished(const JobId& id, std::chrono::milliseconds t = 60s) {
        return sup->runUntil([&] { return isFinished(sup->job(id)->state); }, t);
    }
};

#define REQUIRE_PYTHON() \
    if (!python()) SKIP("Python 3.10+ not found")

} // namespace

TEST_CASE("probePython reports version and pointer size", "[ai]") {
    REQUIRE_PYTHON();
    auto p = probePython(python()->executable, python()->prefixArgs);
    REQUIRE(p);
    CHECK(p.value().version == python()->version);
    CHECK(p.value().version.find(' ') == std::string::npos);
    CHECK((p.value().bits == 32 || p.value().bits == 64));
}

TEST_CASE("AI worker starts, reports tasks and runs a job with progress", "[ai]") {
    REQUIRE_PYTHON();
    Fixture f;
    REQUIRE(f.ready());
    CHECK(f.sup->hello().value("protocol", 0) == 1);
    CHECK(f.sup->taskAvailable("diag.sleep"));
    std::string why;
    CHECK_FALSE(f.sup->taskAvailable("no.such.task", &why));

    auto id = f.sup->submit("diag.sleep", {{"seconds", 0.4}, {"steps", 4}}, "Nap");
    REQUIRE(id);
    std::vector<double> seen;
    std::vector<Event> ev;
    REQUIRE(f.sup->runUntil([&] {
        const Job* j = f.sup->job(id.value());
        if (j->progress >= 0 && (seen.empty() || seen.back() != j->progress)) seen.push_back(j->progress);
        return isFinished(j->state);
    }, 60s, &ev));
    const Job* j = f.sup->job(id.value());
    CHECK(j->state == JobState::Succeeded);
    CHECK(j->label == "Nap");
    REQUIRE(j->outputs.count("result"));
    CHECK(std::filesystem::exists(j->outputs.at("result")));
    CHECK(seen.size() >= 2);
    CHECK(seen.back() == 1.0);
    CHECK(std::any_of(ev.begin(), ev.end(), [](const Event& e) { return e.kind == Event::Kind::Job; }));
}

TEST_CASE("AI jobs run in order; queued jobs cancel immediately", "[ai]") {
    REQUIRE_PYTHON();
    Fixture f;
    REQUIRE(f.ready());
    auto a = f.sup->submit("diag.sleep", {{"seconds", 0.3}, {"steps", 3}}).value();
    auto b = f.sup->submit("diag.echo", {{"n", 2}}).value();
    auto c = f.sup->submit("diag.echo", {{"n", 3}}).value();
    REQUIRE(f.sup->cancel(b));
    CHECK(f.sup->job(b)->state == JobState::Cancelled);
    REQUIRE(f.finished(c));
    CHECK(f.sup->job(a)->state == JobState::Succeeded);
    CHECK(f.sup->job(c)->state == JobState::Succeeded);
    CHECK(f.sup->job(c)->info["params"]["n"] == 3);
    CHECK(f.sup->job(a)->finished <= f.sup->job(c)->started);
    CHECK(f.sup->job(b)->attempts == 0);   // never reached the worker
    CHECK(f.sup->idle());
    f.sup->clearFinished(true);
    CHECK(f.sup->jobs().empty());
}

TEST_CASE("Cancelling a running AI job is cooperative, or forced after a grace period", "[ai]") {
    REQUIRE_PYTHON();
    Fixture f([](WorkerOptions& o) { o.cancelGrace = 800ms; });
    REQUIRE(f.ready());
    const auto pid = f.sup->workerPid();

    auto soft = f.sup->submit("diag.sleep", {{"seconds", 30}, {"steps", 300}}).value();
    REQUIRE(f.sup->runUntil([&] { return f.sup->job(soft)->progress > 0; }, 30s));
    REQUIRE(f.sup->cancel(soft));
    REQUIRE(f.finished(soft, 10s));
    CHECK(f.sup->job(soft)->state == JobState::Cancelled);
    CHECK(f.sup->workerPid() == pid);   // same worker, no restart needed

    auto stubborn = f.sup->submit("diag.stubborn", {{"seconds", 60}}).value();
    REQUIRE(f.sup->runUntil([&] { return f.sup->job(stubborn)->state == JobState::Running; }, 30s));
    const auto t0 = Clock::now();
    REQUIRE(f.sup->cancel(stubborn));
    REQUIRE(f.finished(stubborn, 20s));
    CHECK(f.sup->job(stubborn)->state == JobState::Cancelled);
    CHECK(Clock::now() - t0 < 15s);
    REQUIRE(f.ready());                 // restarted after the forced kill
    CHECK(f.sup->workerPid() != pid);
    auto after = f.sup->submit("diag.echo", {}).value();
    REQUIRE(f.finished(after));
    CHECK(f.sup->job(after)->state == JobState::Succeeded);
}

TEST_CASE("A worker crash retries the job once, then fails it with the worker's output", "[ai]") {
    REQUIRE_PYTHON();
    Fixture f;
    REQUIRE(f.ready());
    auto crash = f.sup->submit("diag.crash", {{"after_steps", 1}, {"code", 17}}).value();
    REQUIRE(f.finished(crash));
    const Job* j = f.sup->job(crash);
    CHECK(j->state == JobState::Failed);
    CHECK(j->failure == FailureKind::WorkerCrashed);
    CHECK(j->attempts == 2);
    CHECK(j->error.find("exit code 17") != std::string::npos);
    CHECK(j->error.find("simulated fatal error") != std::string::npos);
    // The editor-side service survives and keeps working.
    REQUIRE(f.ready());
    auto ok = f.sup->submit("diag.echo", {}).value();
    REQUIRE(f.finished(ok));
    CHECK(f.sup->job(ok)->state == JobState::Succeeded);
    CHECK(f.sup->restarts() >= 2);
}

TEST_CASE("Repeated crashes stop automatic restarts until start() is called", "[ai]") {
    REQUIRE_PYTHON();
    Fixture f([](WorkerOptions& o) { o.maxCrashes = 2; o.maxAttempts = 1; });
    REQUIRE(f.ready());
    auto c1 = f.sup->submit("diag.crash", {{"after_steps", 0}}).value();
    REQUIRE(f.finished(c1));
    REQUIRE(f.ready());
    auto c2 = f.sup->submit("diag.crash", {{"after_steps", 0}}).value();
    auto queued = f.sup->submit("diag.echo", {}).value();
    REQUIRE(f.sup->runUntil([&] { return f.sup->state() == WorkerState::Failed; }, 30s));
    CHECK(f.sup->job(c2)->state == JobState::Failed);
    CHECK(f.sup->job(queued)->state == JobState::Failed);   // not left hanging
    CHECK(f.sup->lastError().find("keeps crashing") != std::string::npos);
    REQUIRE(f.ready());   // explicit start() recovers
}

#ifndef _WIN32
TEST_CASE("Killing the worker from outside mid-job: job retried on a fresh worker", "[ai]") {
    REQUIRE_PYTHON();
    Fixture f;
    REQUIRE(f.ready());
    auto id = f.sup->submit("diag.sleep", {{"seconds", 1.5}, {"steps", 15}}).value();
    REQUIRE(f.sup->runUntil([&] { return f.sup->job(id)->progress > 0.1; }, 30s));
    const auto pid = f.sup->workerPid();
    REQUIRE(::kill(static_cast<pid_t>(pid), SIGKILL) == 0);   // like the OS killing an OOM process
    REQUIRE(f.finished(id));
    const Job* j = f.sup->job(id);
    CHECK(j->state == JobState::Succeeded);
    CHECK(j->attempts == 2);
    CHECK(f.sup->workerPid() != pid);
}
#endif

TEST_CASE("AI job inputs, failures and stop", "[ai]") {
    REQUIRE_PYTHON();
    Fixture f;
    REQUIRE(f.ready());
    Image img(3, 2, Rgba8::opaque(1, 2, 3));
    auto e = f.sup->submit("diag.echo", {{"keep", true}}, "", {{"input", &img}}).value();
    REQUIRE(f.finished(e));
    const std::string path = f.sup->job(e)->info["params"]["input"].get<std::string>();
    CHECK(readPng(std::filesystem::path(path)).value() == img);

    auto fail = f.sup->submit("diag.fail", {{"message", "no VRAM"}, {"kind", "oom"}}).value();
    auto unknown = f.sup->submit("no.such.task", {}).value();
    REQUIRE(f.finished(unknown));
    CHECK(f.sup->job(fail)->failure == FailureKind::OutOfMemory);
    CHECK(f.sup->job(fail)->error == "no VRAM");
    CHECK(f.sup->job(unknown)->state == JobState::Failed);

    auto running = f.sup->submit("diag.sleep", {{"seconds", 30}, {"steps", 300}}).value();
    REQUIRE(f.sup->runUntil([&] { return f.sup->job(running)->state == JobState::Running; }, 30s));
    f.sup->stop();
    CHECK(f.sup->state() == WorkerState::Stopped);
    CHECK(f.sup->job(running)->state == JobState::Cancelled);
}

TEST_CASE("AI worker that cannot start is reported, not retried forever", "[ai]") {
    TempDir dir;
    WorkerOptions o;
    o.process.program = (dir.path() / "no-python-here").string();
    o.jobsRoot = dir.path();
    WorkerSupervisor sup(o);
    sup.start();
    CHECK(sup.state() == WorkerState::Failed);
    CHECK(sup.lastError().find("Could not start") != std::string::npos);
}
