"""Process-level resource limits, set by the app from the resource profile
(Eco / Balanced / Performance) through the environment at launch:

  ACESTUDIO_LOW_PRIORITY=1   below-normal CPU priority, so Windows, drawing and
                             other programs stay responsive during generation
  ACESTUDIO_CPU_THREADS=N    threads for PyTorch, ONNX Runtime and numpy (0 = all)

Applied once at start-up, before PyTorch or numpy is imported (their thread
pools read OMP_NUM_THREADS / MKL_NUM_THREADS when first loaded).
"""

import os


def cpu_threads():
    try:
        n = int(os.environ.get("ACESTUDIO_CPU_THREADS", "0"))
    except ValueError:
        return 0
    return max(0, n)


def apply_process_limits():
    applied = {}
    n = cpu_threads()
    if n > 0:
        for var in ("OMP_NUM_THREADS", "MKL_NUM_THREADS", "OPENBLAS_NUM_THREADS", "NUMEXPR_NUM_THREADS"):
            os.environ[var] = str(n)
        applied["threads"] = n
    if os.environ.get("ACESTUDIO_LOW_PRIORITY") == "1":
        try:
            if os.name == "nt":
                import ctypes
                from ctypes import wintypes
                BELOW_NORMAL_PRIORITY_CLASS = 0x00004000
                k32 = ctypes.WinDLL("kernel32", use_last_error=True)
                # Explicit types: with the default int the process pseudo-handle
                # is truncated on 64-bit Windows and the call does nothing.
                k32.GetCurrentProcess.restype = wintypes.HANDLE
                k32.GetCurrentProcess.argtypes = []
                k32.SetPriorityClass.restype = wintypes.BOOL
                k32.SetPriorityClass.argtypes = [wintypes.HANDLE, wintypes.DWORD]
                k32.GetPriorityClass.restype = wintypes.DWORD
                k32.GetPriorityClass.argtypes = [wintypes.HANDLE]
                if not k32.SetPriorityClass(k32.GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS):
                    raise OSError(ctypes.get_last_error(), "SetPriorityClass failed")
                if k32.GetPriorityClass(k32.GetCurrentProcess()) != BELOW_NORMAL_PRIORITY_CLASS:
                    raise OSError("the priority did not change")
            else:
                os.nice(5)
            applied["priority"] = "below-normal"
        except Exception as e:   # noqa: BLE001 - a limit that cannot be set is not fatal
            applied["priority"] = "normal (could not lower: %s)" % e
    return applied


def limit_torch(torch):
    """Caps PyTorch's intra-op threads (call right after importing torch)."""
    n = cpu_threads()
    if n > 0:
        try:
            torch.set_num_threads(n)
        except Exception:   # noqa: BLE001
            pass
