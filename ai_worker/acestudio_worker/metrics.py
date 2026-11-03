"""Per-job measurements: wall and CPU time, named stages, peak process RAM and
peak device VRAM. Every job reports them in info["metrics"], so benchmark
numbers come from the run itself rather than estimates.

Peak VRAM is the device-wide used memory (torch.cuda.mem_get_info), sampled:
it includes ONNX Runtime's CUDA allocations (background removal), which
torch.cuda.max_memory_allocated does not see. Sampling starts only when the
job has already imported torch, so tasks without PyTorch stay light.
"""

import contextlib
import os
import sys
import threading
import time


def process_rss_bytes():
    """Current resident memory of this process (0 when unknown)."""
    try:
        if os.name == "nt":
            import ctypes
            from ctypes import wintypes

            class PMC(ctypes.Structure):
                _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD),
                            ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                            ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
                            ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t), ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                            ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t)]
            pmc = PMC()
            pmc.cb = ctypes.sizeof(PMC)
            proc = ctypes.windll.kernel32.GetCurrentProcess()
            if ctypes.windll.psapi.GetProcessMemoryInfo(proc, ctypes.byref(pmc), pmc.cb):
                return int(pmc.WorkingSetSize)
            return 0
        with open("/proc/self/statm") as f:
            return int(f.read().split()[1]) * os.sysconf("SC_PAGE_SIZE")
    except Exception:   # noqa: BLE001 - measurements never fail a job
        return 0


def device_used_bytes():
    """Device-wide used VRAM on GPU 0, or None without an initialised CUDA."""
    torch = sys.modules.get("torch")
    if torch is None:
        return None
    try:
        if not torch.cuda.is_available() or not torch.cuda.is_initialized():
            return None
        free, total = torch.cuda.mem_get_info()
        return int(total - free)
    except Exception:   # noqa: BLE001
        return None


class Sampler:
    """Samples RSS and VRAM every `interval` seconds on a daemon thread."""

    def __init__(self, interval=0.25):
        self.interval = interval
        self.peak_rss = process_rss_bytes()
        self.peak_vram = None
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._loop, name="metrics", daemon=True)

    def _sample(self):
        self.peak_rss = max(self.peak_rss, process_rss_bytes())
        v = device_used_bytes()
        if v is not None:
            self.peak_vram = v if self.peak_vram is None else max(self.peak_vram, v)

    def _loop(self):
        while not self._stop.wait(self.interval):
            self._sample()

    def start(self):
        self._thread.start()
        return self

    def stop(self):
        self._stop.set()
        self._thread.join(timeout=1.0)
        self._sample()


class Stages:
    """Named durations, in the order first seen. Re-entering a name adds to it."""

    def __init__(self):
        self.seconds = {}

    @contextlib.contextmanager
    def stage(self, name):
        t0 = time.perf_counter()
        try:
            yield
        finally:
            self.seconds[name] = self.seconds.get(name, 0.0) + (time.perf_counter() - t0)

    def add(self, name, seconds):
        self.seconds[name] = self.seconds.get(name, 0.0) + float(seconds)


def summary(wall_s, cpu_s, sampler, stages):
    m = {"wallSeconds": round(wall_s, 3), "cpuSeconds": round(cpu_s, 3),
         "cpuCoresAverage": round(cpu_s / wall_s, 2) if wall_s > 0 else 0.0,
         "peakRssGb": round(sampler.peak_rss / 2**30, 3),
         "stages": {k: round(v, 3) for k, v in stages.seconds.items()}}
    if sampler.peak_vram is not None:
        m["peakDeviceVramGb"] = round(sampler.peak_vram / 2**30, 3)
    return m
