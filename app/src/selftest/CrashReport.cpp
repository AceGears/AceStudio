#include "CrashReport.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// dbghelp.h needs windows.h first.
#include <dbghelp.h>

#include <cstdio>
#include <cstring>

namespace acestudio::ui {

namespace {

DWORD g_mainThread = 0;

// "module+0xoffset" for an address, plus the symbol when one is known.
void describe(HANDLE process, DWORD64 addr) {
    char module[MAX_PATH] = "?";
    DWORD64 base = 0;
    HMODULE mod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(addr), &mod) &&
        mod) {
        char path[MAX_PATH] = {};
        if (GetModuleFileNameA(mod, path, MAX_PATH)) {
            const char* slash = std::strrchr(path, '\\');
            strncpy_s(module, slash ? slash + 1 : path, _TRUNCATE);
        }
        base = reinterpret_cast<DWORD64>(mod);
    }
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256] = {};
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    DWORD64 disp = 0;
    const bool named = SymFromAddr(process, addr, &disp, sym) != FALSE;
    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof(line);
    DWORD lineDisp = 0;
    const bool lined = SymGetLineFromAddr64(process, addr, &lineDisp, &line) != FALSE;
    std::fprintf(stderr, "ui-audit: crash   at %s+0x%llx", module, static_cast<unsigned long long>(addr - base));
    if (named) std::fprintf(stderr, " %s+0x%llx", sym->Name, static_cast<unsigned long long>(disp));
    if (lined && line.FileName) {
        const char* f = std::strrchr(line.FileName, '\\');
        std::fprintf(stderr, " (%s:%lu)", f ? f + 1 : line.FileName, line.LineNumber);
    }
    std::fprintf(stderr, "\n");
}

LONG WINAPI onCrash(EXCEPTION_POINTERS* ep) {
    const DWORD thread = GetCurrentThreadId();
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    std::fprintf(stderr, "ui-audit: crash: exception 0x%08lx on %s thread %lu", er->ExceptionCode,
                 thread == g_mainThread ? "the main" : "another", thread);
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        std::fprintf(stderr, " (%s 0x%llx)", er->ExceptionInformation[0] == 0 ? "reading" : er->ExceptionInformation[0] == 1 ? "writing" : "executing",
                     static_cast<unsigned long long>(er->ExceptionInformation[1]));
    std::fprintf(stderr, "\n");

    HANDLE process = GetCurrentProcess();
    CONTEXT ctx = *ep->ContextRecord;
    STACKFRAME64 frame{};
    DWORD machine = 0;
#if defined(_M_X64)
    machine = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset = ctx.Rip;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrStack.Offset = ctx.Rsp;
#elif defined(_M_ARM64)
    machine = IMAGE_FILE_MACHINE_ARM64;
    frame.AddrPC.Offset = ctx.Pc;
    frame.AddrFrame.Offset = ctx.Fp;
    frame.AddrStack.Offset = ctx.Sp;
#endif
    frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;
    if (machine == 0) {
        describe(process, reinterpret_cast<DWORD64>(er->ExceptionAddress));
    } else {
        for (int i = 0; i < 24; ++i) {
            if (!StackWalk64(machine, process, GetCurrentThread(), &frame, &ctx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
                break;
            if (frame.AddrPC.Offset == 0) break;
            describe(process, frame.AddrPC.Offset);
        }
    }
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;   // the process still ends as a crash
}

} // namespace

void installAuditCrashReport() {
    g_mainThread = GetCurrentThreadId();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    SetUnhandledExceptionFilter(onCrash);
}

} // namespace acestudio::ui

#else

namespace acestudio::ui {
void installAuditCrashReport() {}
} // namespace acestudio::ui

#endif
