"""NVIDIA driver detection without importing torch."""

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
