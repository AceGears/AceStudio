#include <catch2/catch_test_macros.hpp>

#include "TestSupport.h"
#include "acestudio/core/Subprocess.h"

#include <thread>

using namespace acestudio;
using namespace std::chrono_literals;

namespace {
ProcessSpec child(std::vector<std::string> args) {
    ProcessSpec s;
    s.program = ACESTUDIO_TEST_CHILD;
    s.args = std::move(args);
    return s;
}
std::vector<std::string> collect(Subprocess& p, Subprocess::Stream s = Subprocess::Stream::Stdout) {
    REQUIRE(p.wait(20s));
    return p.takeLines(s);
}
} // namespace

TEST_CASE("Subprocess echoes lines on stdout and stderr", "[subprocess]") {
    auto p = Subprocess::start(child({"echo"}));
    REQUIRE(p);
    auto& proc = *p.value();
    CHECK(proc.pid() > 0);
    REQUIRE(proc.writeLine("hello"));
    REQUIRE(proc.writeLine("w\xC3\xB6rld"));   // UTF-8 passes through untouched
    std::vector<std::string> got;
    for (int i = 0; i < 200 && got.size() < 2; ++i) {
        proc.waitForLine(100ms);
        for (auto& l : proc.takeLines(Subprocess::Stream::Stdout)) got.push_back(l);
    }
    REQUIRE(got.size() == 2);
    CHECK(got[0] == "echo:hello");
    CHECK(got[1] == "echo:w\xC3\xB6rld");
    CHECK(proc.running());
    proc.closeStdin();   // child sees EOF and exits by itself
    REQUIRE(proc.wait(20s) == 0);
    auto rest = proc.takeLines(Subprocess::Stream::Stdout);
    REQUIRE_FALSE(rest.empty());
    CHECK(rest.back() == "stdin-closed");
    CHECK(proc.takeLines(Subprocess::Stream::Stderr).size() == 2);
    CHECK_FALSE(proc.writeLine("late"));
}

TEST_CASE("Subprocess reports exit codes and delivers all output", "[subprocess]") {
    auto p = Subprocess::start(child({"exit", "7"}));
    REQUIRE(p);
    CHECK(p.value()->wait(20s) == 7);

    auto spam = Subprocess::start(child({"spam", "5000"}));
    REQUIRE(spam);
    auto lines = collect(*spam.value());
    REQUIRE(lines.size() == 5001);
    CHECK(lines[4999] == "line 4999");
    CHECK(lines.back() == "tail-without-newline");
    CHECK(spam.value()->exitCode() == 3);
}

TEST_CASE("Subprocess passes arguments, environment and working directory", "[subprocess]") {
    const std::vector<std::string> args{"plain", "with space", "quote\"inside", "trailing\\", "", "back\\\\slash \"q\""};
    std::vector<std::string> full{"args"};
    full.insert(full.end(), args.begin(), args.end());
    auto p = Subprocess::start(child(full));
    REQUIRE(p);
    auto lines = collect(*p.value());
    REQUIRE(lines.size() == args.size());
    for (std::size_t i = 0; i < args.size(); ++i) CHECK(lines[i] == "[" + args[i] + "]");

    auto spec = child({"env", "ACE_TEST"});
    spec.env = {{"ACE_TEST", "value 42"}};
    auto e = Subprocess::start(spec);
    REQUIRE(e);
    CHECK(collect(*e.value()) == std::vector<std::string>{"value 42"});

    TempDir dir;
    auto cspec = child({"cwd"});
    cspec.workingDir = dir.path();
    auto c = Subprocess::start(cspec);
    REQUIRE(c);
    auto cwd = collect(*c.value());
    REQUIRE(cwd.size() == 1);
    CHECK(std::filesystem::equivalent(cwd[0], dir.path()));

    // Paths built from generic (forward-slash) strings must work on Windows too.
    auto gspec = child({"cwd"});
    gspec.workingDir = std::filesystem::path(dir.path().generic_string());
    gspec.program = std::filesystem::path(std::filesystem::path(ACESTUDIO_TEST_CHILD).generic_string());
    auto g = Subprocess::start(gspec);
    REQUIRE(g);
    auto gcwd = collect(*g.value());
    REQUIRE(gcwd.size() == 1);
    CHECK(std::filesystem::equivalent(gcwd[0], dir.path()));
}

TEST_CASE("Subprocess can be killed and fails cleanly to start", "[subprocess]") {
    auto p = Subprocess::start(child({"sleep", "60"}));
    REQUIRE(p);
    auto& proc = *p.value();
    REQUIRE(proc.waitForLine(20s));
    const auto t0 = std::chrono::steady_clock::now();
    proc.kill();
    const auto code = proc.wait(20s);
    REQUIRE(code);
    CHECK(*code != 0);
    CHECK(std::chrono::steady_clock::now() - t0 < 10s);

    auto missing = Subprocess::start(ProcessSpec{"/definitely/not/here/acestudio-nope", {}, {}, {}});
    CHECK_FALSE(missing);
    auto notOnPath = Subprocess::start(ProcessSpec{"acestudio-no-such-program-xyz", {}, {}, {}});
    REQUIRE_FALSE(notOnPath);
    CHECK(notOnPath.error().code == ErrorCode::NotFound);
}

TEST_CASE("Destroying a running Subprocess kills it", "[subprocess]") {
    auto p = Subprocess::start(child({"sleep", "60"}));
    REQUIRE(p);
    REQUIRE(p.value()->waitForLine(20s));
    const auto t0 = std::chrono::steady_clock::now();
    p.value().reset();
    CHECK(std::chrono::steady_clock::now() - t0 < 10s);
}

TEST_CASE("Windows argument quoting follows CommandLineToArgvW rules", "[subprocess]") {
    CHECK(quoteWindowsArg("abc") == "abc");
    CHECK(quoteWindowsArg("") == "\"\"");
    CHECK(quoteWindowsArg("a b") == "\"a b\"");
    CHECK(quoteWindowsArg("a\"b") == "\"a\\\"b\"");
    CHECK(quoteWindowsArg("C:\\path with space\\") == "\"C:\\path with space\\\\\"");
    CHECK(quoteWindowsArg("a\\\\\"b") == "\"a\\\\\\\\\\\"b\"");
    CHECK(quoteWindowsArg("no\\quote") == "no\\quote");
}
