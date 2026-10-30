#include "acestudio/core/Subprocess.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace fs = std::filesystem;

namespace acestudio {

namespace {

// Lines arriving from one pipe.
struct LineBuffer {
    std::string partial;
    std::deque<std::string> lines;
    bool eof = false;

    void feed(const char* data, std::size_t n) {
        partial.append(data, n);
        std::size_t start = 0;
        for (;;) {
            const std::size_t nl = partial.find('\n', start);
            if (nl == std::string::npos) break;
            std::string line = partial.substr(start, nl - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(std::move(line));
            start = nl + 1;
        }
        partial.erase(0, start);
        // Guard against a child that never sends a newline.
        if (partial.size() > (64u << 20)) {
            lines.push_back(std::move(partial));
            partial.clear();
        }
    }
    void finish() {
        if (!partial.empty()) {
            if (partial.back() == '\r') partial.pop_back();
            lines.push_back(std::move(partial));
            partial.clear();
        }
        eof = true;
    }
};

} // namespace

std::string quoteWindowsArg(std::string_view arg) {
    if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string_view::npos) return std::string(arg);
    std::string out = "\"";
    for (std::size_t i = 0;; ++i) {
        std::size_t backslashes = 0;
        while (i < arg.size() && arg[i] == '\\') { ++i; ++backslashes; }
        if (i == arg.size()) {
            out.append(backslashes * 2, '\\');   // escape before the closing quote
            break;
        }
        if (arg[i] == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out.push_back('"');
        } else {
            out.append(backslashes, '\\');
            out.push_back(arg[i]);
        }
    }
    out.push_back('"');
    return out;
}

std::optional<fs::path> findExecutable(std::string_view name) {
    if (name.empty()) return std::nullopt;
    std::error_code ec;
    const fs::path direct(name);
    if (direct.has_parent_path()) return fs::exists(direct, ec) ? std::optional<fs::path>(direct) : std::nullopt;
#ifdef _WIN32
    wchar_t buf[32768];
    const DWORD n = GetEnvironmentVariableW(L"PATH", buf, 32768);
    if (n == 0 || n >= 32768) return std::nullopt;
    std::wstring path(buf, n);
    const wchar_t sep = L';';
    std::vector<std::string> exts = {"", ".exe", ".bat", ".cmd"};
#else
    const char* p = std::getenv("PATH");
    if (!p) return std::nullopt;
    std::string path(p);
    const char sep = ':';
    std::vector<std::string> exts = {""};
#endif
    std::size_t start = 0;
    while (start <= path.size()) {
        std::size_t end = path.find(sep, start);
        if (end == decltype(path)::npos) end = path.size();
        const fs::path dir(path.substr(start, end - start));
        if (!dir.empty())
            for (const auto& ext : exts) {
                fs::path cand = dir / (std::string(name) + ext);
                if (fs::is_regular_file(cand, ec)) {
#ifndef _WIN32
                    if (::access(cand.c_str(), X_OK) != 0) continue;
#endif
                    return cand;
                }
            }
        start = end + 1;
    }
    return std::nullopt;
}

// ----------------------------------------------------------------- shared

struct Subprocess::Impl {
    std::mutex mu;
    std::condition_variable cv;
    LineBuffer out, err;
    std::thread outThread, errThread;
    std::mutex writeMu;
    std::optional<int> exit;
    std::optional<std::chrono::steady_clock::time_point> exitSeen;
    bool killed = false;
    long long pid = -1;

#ifdef _WIN32
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    HANDLE stdinW = nullptr;
    HANDLE stdoutR = nullptr;
    HANDLE stderrR = nullptr;
    HANDLE readerThreads[2] = {nullptr, nullptr};   // for CancelSynchronousIo
#else
    int stdinW = -1;
    int stdoutR = -1;
    int stderrR = -1;
#endif

    void reader(bool isErr) {
#ifdef _WIN32
        {
            HANDLE self = nullptr;
            DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self, 0, FALSE, DUPLICATE_SAME_ACCESS);
            std::lock_guard lock(mu);
            readerThreads[isErr ? 1 : 0] = self;
        }
#endif
        char buf[16384];
        for (;;) {
#ifdef _WIN32
            DWORD got = 0;
            const BOOL ok = ReadFile(isErr ? stderrR : stdoutR, buf, sizeof buf, &got, nullptr);
            if (!ok || got == 0) break;
            const std::size_t n = got;
#else
            const ssize_t r = ::read(isErr ? stderrR : stdoutR, buf, sizeof buf);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) break;
            const std::size_t n = static_cast<std::size_t>(r);
#endif
            std::lock_guard lock(mu);
            (isErr ? err : out).feed(buf, n);
            cv.notify_all();
        }
        std::lock_guard lock(mu);
        (isErr ? err : out).finish();
        cv.notify_all();
    }

    // Platform: non-blocking check of the raw process exit. Caller holds mu.
    std::optional<int> pollRaw();
    void killRaw();
    void closeStdinRaw();
    void closeHandles();
};

#ifdef _WIN32

namespace {
std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::string lastErrorText(const char* what) {
    return std::string(what) + " failed (Windows error " + std::to_string(GetLastError()) + ")";
}
struct CaseLess {
    bool operator()(const std::wstring& a, const std::wstring& b) const { return _wcsicmp(a.c_str(), b.c_str()) < 0; }
};
} // namespace

std::optional<int> Subprocess::Impl::pollRaw() {
    if (!process) return exit;
    if (WaitForSingleObject(process, 0) != WAIT_OBJECT_0) return std::nullopt;
    DWORD code = 0;
    GetExitCodeProcess(process, &code);
    return killed ? -9 : static_cast<int>(code);
}
void Subprocess::Impl::killRaw() {
    if (process && WaitForSingleObject(process, 0) != WAIT_OBJECT_0) {
        killed = true;
        TerminateProcess(process, 1);
    }
}
void Subprocess::Impl::closeStdinRaw() {
    if (stdinW) { CloseHandle(stdinW); stdinW = nullptr; }
}
void Subprocess::Impl::closeHandles() {
    closeStdinRaw();
    if (stdoutR) { CloseHandle(stdoutR); stdoutR = nullptr; }
    if (stderrR) { CloseHandle(stderrR); stderrR = nullptr; }
    if (process) { CloseHandle(process); process = nullptr; }
    if (job) { CloseHandle(job); job = nullptr; }
    for (HANDLE& t : readerThreads)
        if (t) { CloseHandle(t); t = nullptr; }
}

Result<std::unique_ptr<Subprocess>> Subprocess::start(const ProcessSpec& spec) {
    auto impl = std::make_unique<Impl>();
    fs::path program = spec.program;
    if (!program.has_parent_path()) {
        auto found = findExecutable(program.string());
        if (!found) return Error{ErrorCode::NotFound, "Program not found on PATH: " + program.string()};
        program = *found;
    }
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE inR = nullptr, outW = nullptr, errW = nullptr;
    if (!CreatePipe(&inR, &impl->stdinW, &sa, 0) || !CreatePipe(&impl->stdoutR, &outW, &sa, 0) ||
        !CreatePipe(&impl->stderrR, &errW, &sa, 0)) {
        for (HANDLE h : {inR, outW, errW}) if (h) CloseHandle(h);
        impl->closeHandles();
        return Error{ErrorCode::IoError, lastErrorText("CreatePipe")};
    }
    for (HANDLE h : {impl->stdinW, impl->stdoutR, impl->stderrR}) SetHandleInformation(h, HANDLE_FLAG_INHERIT, 0);

    std::wstring cmd = widen(quoteWindowsArg(narrow(program.wstring())));
    for (const auto& a : spec.args) cmd += L" " + widen(quoteWindowsArg(a));

    // Environment block: parent's variables with overrides, sorted.
    std::map<std::wstring, std::wstring, CaseLess> vars;
    if (wchar_t* block = GetEnvironmentStringsW()) {
        for (const wchar_t* p = block; *p; p += wcslen(p) + 1) {
            std::wstring entry(p);
            const std::size_t eq = entry.find(L'=', 1);   // skip leading '=' of drive vars
            if (eq == std::wstring::npos) continue;
            vars[entry.substr(0, eq)] = entry.substr(eq + 1);
        }
        FreeEnvironmentStringsW(block);
    }
    for (const auto& [k, v] : spec.env) {
        if (v.empty()) vars.erase(widen(k)); else vars[widen(k)] = widen(v);
    }
    std::wstring envBlock;
    for (const auto& [k, v] : vars) { envBlock += k; envBlock += L'='; envBlock += v; envBlock += L'\0'; }
    envBlock += L'\0';

    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inR;
    si.hStdOutput = outW;
    si.hStdError = errW;
    PROCESS_INFORMATION pi{};
    const std::wstring cwd = spec.workingDir.empty() ? std::wstring() : spec.workingDir.wstring();
    const BOOL ok = CreateProcessW(program.wstring().c_str(), cmd.data(), nullptr, nullptr, TRUE,
                                   CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | CREATE_SUSPENDED,
                                   envBlock.data(), cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    const std::string spawnError = ok ? std::string() : lastErrorText("CreateProcess");
    for (HANDLE h : {inR, outW, errW}) CloseHandle(h);
    if (!ok) {
        impl->closeHandles();
        return Error{ErrorCode::IoError, spawnError + ": " + program.string()};
    }
    // Kill the child automatically if AceStudio exits or crashes.
    impl->job = CreateJobObjectW(nullptr, nullptr);
    if (impl->job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(impl->job, JobObjectExtendedLimitInformation, &info, sizeof info);
        if (!AssignProcessToJobObject(impl->job, pi.hProcess)) { CloseHandle(impl->job); impl->job = nullptr; }
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    impl->process = pi.hProcess;
    impl->pid = static_cast<long long>(pi.dwProcessId);
    Impl* raw = impl.get();
    impl->outThread = std::thread([raw] { raw->reader(false); });
    impl->errThread = std::thread([raw] { raw->reader(true); });
    return std::unique_ptr<Subprocess>(new Subprocess(std::move(impl)));
}

Status Subprocess::writeLine(std::string_view line) {
    std::lock_guard lock(m->writeMu);
    if (!m->stdinW) return Error{ErrorCode::IoError, "Process stdin is closed"};
    std::string data(line);
    data.push_back('\n');
    const char* p = data.data();
    std::size_t left = data.size();
    while (left > 0) {
        DWORD wrote = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(left, 1u << 20));
        if (!WriteFile(m->stdinW, p, chunk, &wrote, nullptr)) return Error{ErrorCode::IoError, lastErrorText("WriteFile")};
        p += wrote;
        left -= wrote;
    }
    return {};
}

#else // POSIX

std::optional<int> Subprocess::Impl::pollRaw() {
    if (pid <= 0) return exit;
    int status = 0;
    const pid_t r = ::waitpid(static_cast<pid_t>(pid), &status, WNOHANG);
    if (r == 0) return std::nullopt;
    if (r < 0) return exit.value_or(-1);   // already reaped
    pid = -pid;                            // reaped; keep sign as a marker
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return -WTERMSIG(status);
    return -1;
}
void Subprocess::Impl::killRaw() {
    if (pid > 0 && !exit) {
        killed = true;
        ::kill(static_cast<pid_t>(pid), SIGKILL);
    }
}
void Subprocess::Impl::closeStdinRaw() {
    if (stdinW >= 0) { ::close(stdinW); stdinW = -1; }
}
void Subprocess::Impl::closeHandles() {
    closeStdinRaw();
    if (stdoutR >= 0) { ::close(stdoutR); stdoutR = -1; }
    if (stderrR >= 0) { ::close(stderrR); stderrR = -1; }
}

Result<std::unique_ptr<Subprocess>> Subprocess::start(const ProcessSpec& spec) {
    // A dead child must surface as a write error, not kill AceStudio.
    static const bool sigpipeIgnored = [] { std::signal(SIGPIPE, SIG_IGN); return true; }();
    (void)sigpipeIgnored;

    auto impl = std::make_unique<Impl>();
    int in[2], out[2], err[2];
    if (::pipe2(in, O_CLOEXEC) != 0) return Error{ErrorCode::IoError, std::string("pipe: ") + std::strerror(errno)};
    if (::pipe2(out, O_CLOEXEC) != 0) { ::close(in[0]); ::close(in[1]); return Error{ErrorCode::IoError, "pipe failed"}; }
    if (::pipe2(err, O_CLOEXEC) != 0) {
        for (int fd : {in[0], in[1], out[0], out[1]}) ::close(fd);
        return Error{ErrorCode::IoError, "pipe failed"};
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_adddup2(&fa, err[1], 2);
    if (!spec.workingDir.empty()) {
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 29))
        posix_spawn_file_actions_addchdir_np(&fa, spec.workingDir.c_str());
#else
        posix_spawn_file_actions_destroy(&fa);
        for (int fd : {in[0], in[1], out[0], out[1], err[0], err[1]}) ::close(fd);
        return Error{ErrorCode::InvalidArgument, "Setting a working directory is not supported on this platform"};
#endif
    }

    std::map<std::string, std::string> vars;
    for (char** e = environ; e && *e; ++e) {
        const std::string entry(*e);
        const std::size_t eq = entry.find('=');
        if (eq != std::string::npos) vars[entry.substr(0, eq)] = entry.substr(eq + 1);
    }
    for (const auto& [k, v] : spec.env) {
        if (v.empty()) vars.erase(k); else vars[k] = v;
    }
    std::vector<std::string> envStrings;
    for (const auto& [k, v] : vars) envStrings.push_back(k + "=" + v);
    std::vector<char*> envp;
    for (auto& s : envStrings) envp.push_back(s.data());
    envp.push_back(nullptr);

    std::string prog = spec.program.string();
    std::vector<std::string> argStrings{prog};
    argStrings.insert(argStrings.end(), spec.args.begin(), spec.args.end());
    std::vector<char*> argv;
    for (auto& s : argStrings) argv.push_back(s.data());
    argv.push_back(nullptr);

    pid_t pid = 0;
    const int rc = ::posix_spawnp(&pid, prog.c_str(), &fa, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&fa);
    ::close(in[0]);
    ::close(out[1]);
    ::close(err[1]);
    if (rc != 0) {
        for (int fd : {in[1], out[0], err[0]}) ::close(fd);
        return Error{rc == ENOENT ? ErrorCode::NotFound : ErrorCode::IoError,
                     "Cannot start " + prog + ": " + std::strerror(rc)};
    }
    impl->pid = pid;
    impl->stdinW = in[1];
    impl->stdoutR = out[0];
    impl->stderrR = err[0];
    Impl* raw = impl.get();
    impl->outThread = std::thread([raw] { raw->reader(false); });
    impl->errThread = std::thread([raw] { raw->reader(true); });
    return std::unique_ptr<Subprocess>(new Subprocess(std::move(impl)));
}

Status Subprocess::writeLine(std::string_view line) {
    std::lock_guard lock(m->writeMu);
    if (m->stdinW < 0) return Error{ErrorCode::IoError, "Process stdin is closed"};
    std::string data(line);
    data.push_back('\n');
    const char* p = data.data();
    std::size_t left = data.size();
    while (left > 0) {
        const ssize_t w = ::write(m->stdinW, p, left);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return Error{ErrorCode::IoError, std::string("Write to process failed: ") + std::strerror(errno)};
        p += w;
        left -= static_cast<std::size_t>(w);
    }
    return {};
}

#endif

// ----------------------------------------------------------------- common

Subprocess::Subprocess(std::unique_ptr<Impl> impl) : m(std::move(impl)) {}

Subprocess::~Subprocess() {
    if (!m) return;
    closeStdin();
    if (!wait(std::chrono::milliseconds(0))) {
        kill();
        (void)wait(std::chrono::seconds(5));
    }
    auto eofs = [this] {
        std::lock_guard lock(m->mu);
        return m->out.eof && m->err.eof;
    };
#ifdef _WIN32
    // A grandchild can keep the pipes open after the child died; cancel
    // the blocking reads so the threads can finish.
    if (!eofs()) {
        std::lock_guard lock(m->mu);
        for (HANDLE t : m->readerThreads)
            if (t) CancelSynchronousIo(t);
    }
#endif
    for (int i = 0; i < 300 && !eofs(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!eofs()) {
        // A reader is still blocked (POSIX: a grandchild inherited the
        // pipe). Detach and leak the state rather than risk a crash.
        m->outThread.detach();
        m->errThread.detach();
        (void)m.release();
        return;
    }
    m->outThread.join();
    m->errThread.join();
    m->closeHandles();
}

std::vector<std::string> Subprocess::takeLines(Stream s) {
    std::lock_guard lock(m->mu);
    auto& buf = s == Stream::Stdout ? m->out : m->err;
    std::vector<std::string> out(std::make_move_iterator(buf.lines.begin()), std::make_move_iterator(buf.lines.end()));
    buf.lines.clear();
    return out;
}

bool Subprocess::waitForLine(std::chrono::milliseconds timeout) {
    std::unique_lock lock(m->mu);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (m->out.lines.empty()) {
        if (m->out.eof) return false;
        if (m->cv.wait_until(lock, deadline) == std::cv_status::timeout) return !m->out.lines.empty();
    }
    return true;
}

std::optional<int> Subprocess::exitCode() {
    std::lock_guard lock(m->mu);
    if (!m->exit) {
        auto raw = m->pollRaw();
        if (!raw) return std::nullopt;
        m->exit = raw;
        m->exitSeen = std::chrono::steady_clock::now();
    }
    // Report exit only once output is drained, so callers never miss the
    // last lines; give up waiting for EOF after a grace period (a
    // grandchild may have inherited the pipes).
    const bool drained = m->out.eof && m->err.eof;
    if (!drained && std::chrono::steady_clock::now() - *m->exitSeen < std::chrono::milliseconds(1500)) return std::nullopt;
    return m->exit;
}

std::optional<int> Subprocess::wait(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (auto c = exitCode()) return c;
        if (std::chrono::steady_clock::now() >= deadline) return std::nullopt;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

void Subprocess::kill() {
    std::lock_guard lock(m->mu);
    m->killRaw();
}

void Subprocess::closeStdin() {
    std::lock_guard lock(m->writeMu);
    m->closeStdinRaw();
}

long long Subprocess::pid() const noexcept { return m->pid < 0 ? -m->pid : m->pid; }

} // namespace acestudio
