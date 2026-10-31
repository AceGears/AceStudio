"""NVIDIA driver detection without importing torch, and free system memory."""

import os
import re
import shutil
import subprocess
import sys

# Oldest NVIDIA driver branch that supports CUDA 13.0 (CUDA 13.0 release
# notes: R580). Older drivers get the CUDA 12.6 PyTorch build.
CUDA13_MIN_DRIVER = 580


def nvidia_driver():
    """The NVIDIA driver version as a string ("591.44"), or None when there
    is no NVIDIA GPU or nvidia-smi is missing."""
    exe = shutil.which("nvidia-smi")
    if exe is None and sys.platform == "win32":
        candidate = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "nvidia-smi.exe")
        exe = candidate if os.path.exists(candidate) else None
    if exe is None:
        return None
    try:
        out = subprocess.run([exe, "--query-gpu=driver_version", "--format=csv,noheader"],
                             stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=15,
                             creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    except (OSError, subprocess.TimeoutExpired):
        return None
    m = re.search(r"\d+(\.\d+)*", out.stdout or "")
    return m.group(0) if out.returncode == 0 and m else None


def driver_major(version):
    try:
        return int(str(version).split(".")[0])
    except (TypeError, ValueError):
        return 0


def supports_cuda13(version):
    return driver_major(version) >= CUDA13_MIN_DRIVER


def available_memory():
    """Bytes of RAM this process can still use, or None if unknown. Takes the
    smaller of the system's available memory and any cgroup limit (containers)."""
    avail = None
    if sys.platform == "win32":
        import ctypes

        class MemoryStatus(ctypes.Structure):
            _fields_ = [("dwLength", ctypes.c_ulong), ("dwMemoryLoad", ctypes.c_ulong),
                        ("ullTotalPhys", ctypes.c_ulonglong), ("ullAvailPhys", ctypes.c_ulonglong),
                        ("ullTotalPageFile", ctypes.c_ulonglong), ("ullAvailPageFile", ctypes.c_ulonglong),
                        ("ullTotalVirtual", ctypes.c_ulonglong), ("ullAvailVirtual", ctypes.c_ulonglong),
                        ("ullAvailExtendedVirtual", ctypes.c_ulonglong)]
        st = MemoryStatus()
        st.dwLength = ctypes.sizeof(MemoryStatus)
        if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(st)):
            avail = int(st.ullAvailPhys)
        return avail
    try:
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith("MemAvailable:"):
                    avail = int(line.split()[1]) * 1024
                    break
    except OSError:
        pass
    # cgroup limits (containers), v2 or v1: the tightest one on the way to the root.
    def walk(root, rel, limit_name, usage_name):
        nonlocal avail
        path = os.path.join(root, rel.lstrip("/"))
        while path.startswith(root):
            try:
                with open(os.path.join(path, limit_name)) as f:
                    limit = f.read().strip()
                with open(os.path.join(path, usage_name)) as f:
                    used = int(f.read().strip())
                if limit != "max" and int(limit) < 2**60:   # v1 reports "unlimited" as a huge number
                    room = int(limit) - used
                    avail = room if avail is None else min(avail, room)
            except (OSError, ValueError):
                pass
            if path == root:
                break
            path = os.path.dirname(path)
    try:
        with open("/proc/self/cgroup") as f:
            for line in f:
                parts = line.strip().split(":", 2)
                if len(parts) != 3:
                    continue
                if parts[0] == "0" and parts[1] == "":
                    walk("/sys/fs/cgroup", parts[2], "memory.max", "memory.current")
                elif "memory" in parts[1].split(","):
                    walk("/sys/fs/cgroup/memory", parts[2], "memory.limit_in_bytes", "memory.usage_in_bytes")
    except OSError:
        pass
    return avail
