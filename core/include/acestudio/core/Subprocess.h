#pragma once

// Child process with line-oriented stdin/stdout/stderr pipes. Used to run
// the AI worker (and setup tools) without Qt, so the CLI and future MCP
// server share the same supervision code as the desktop app.
//
// Output is read on background threads and buffered as complete lines;
// all public methods are thread-safe. On Windows the child is placed in a
// job object that kills it if AceStudio itself dies; on POSIX the worker
// is expected to exit when its stdin closes.

#include "acestudio/core/Result.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace acestudio {

struct ProcessSpec {
    std::filesystem::path program;          // absolute path, or a name searched on PATH
    std::vector<std::string> args;          // UTF-8
    std::filesystem::path workingDir;       // empty = inherit
    // Added to (or overriding) the parent's environment. Empty value removes.
    std::vector<std::pair<std::string, std::string>> env;
};

class Subprocess {
public:
    enum class Stream { Stdout, Stderr };

    static Result<std::unique_ptr<Subprocess>> start(const ProcessSpec& spec);
    ~Subprocess();   // kills the child if still running

    Subprocess(const Subprocess&) = delete;
    Subprocess& operator=(const Subprocess&) = delete;

    // Writes `line` plus '\n' to the child's stdin.
    Status writeLine(std::string_view line);
    void closeStdin();

    // Removes and returns the complete lines received so far (without '\n';
    // a trailing '\r' is stripped too). A final unterminated line is
    // delivered once the stream reaches EOF.
    std::vector<std::string> takeLines(Stream s);
    // Blocks until a stdout line is available, the child exits or the
    // timeout passes. Returns true if a line is available.
    bool waitForLine(std::chrono::milliseconds timeout);

    // Non-blocking: exit code once the child has exited (and both output
    // streams are drained to EOF), otherwise nullopt. Killed children
    // report a negative value (POSIX: -signal) or the TerminateProcess code.
    std::optional<int> exitCode();
    bool running() { return !exitCode().has_value(); }
    // Waits for exit; nullopt on timeout.
    std::optional<int> wait(std::chrono::milliseconds timeout);
    void kill();
    long long pid() const noexcept;

    struct Impl;

private:
    explicit Subprocess(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m;
};

// Windows command-line quoting (CommandLineToArgvW / MSVCRT rules).
// Exposed for testing; used internally on Windows.
std::string quoteWindowsArg(std::string_view arg);

// Finds an executable on PATH (adds .exe on Windows). nullopt if absent.
std::optional<std::filesystem::path> findExecutable(std::string_view name);

} // namespace acestudio
