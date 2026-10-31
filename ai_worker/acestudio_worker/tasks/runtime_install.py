"""Installs AI packages into the worker's own Python environment with pip.

Only profiles shipped in requirements/ can be installed (no arbitrary
package names from the host). pip output is streamed as progress messages;
cancelling terminates pip. The host restarts the worker afterwards so newly
installed packages are picked up.
"""

import os
import platform
import re
import struct
import subprocess
import sys
import threading
import time

from ..jobs import Cancelled, JobError

NAME = "runtime.install"
REQ_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "requirements")
PROFILES = {
    "onnx-windows": ["onnx-windows.txt"],
    "onnx-cpu": ["onnx-cpu.txt"],
    "diffusion-cuda": ["torch-cuda.txt", "diffusion.txt"],
    "diffusion-cpu": ["torch-cpu.txt", "diffusion.txt"],
    "base": ["base.txt"],
}


def available():
    try:
        import pip  # noqa: F401
    except ImportError:
        return False, "pip is not available in this Python environment"
    return True, ""


def _pip(ctx, args, label):
    # --only-binary: never compile from source (no compiler on most user
    # PCs); a missing wheel fails fast with a clear message instead.
    cmd = [sys.executable, "-m", "pip", "install", "--disable-pip-version-check", "--no-input",
           "--progress-bar", "off", "--only-binary", ":all:"] + args
    ctx.log("Running: " + " ".join(cmd))
    env = dict(os.environ, PYTHONUNBUFFERED="1")
    # stdin=DEVNULL: the worker's stdin is the protocol pipe, and the main
    # thread is blocked reading it. On Windows a child that inherits that
    # handle stalls at startup (CPython queries its stdin, and a query on a
    # synchronous pipe waits for the pending read), so pip never ran.
    proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            env=env, text=True, encoding="utf-8", errors="replace")
    tail = []
    stop = threading.Event()
    started = time.monotonic()
    last = {"msg": "starting pip"}

    def watch():
        # Cancels pip on request, and reports elapsed time so a slow step
        # (a large download, or a package building from source) is visible.
        beat = 0.0
        while not stop.wait(0.2):
            if ctx.cancelled() and proc.poll() is None:
                proc.terminate()
            elapsed = time.monotonic() - started
            if elapsed - beat >= 5:
                beat = elapsed
                ctx.progress(None, "%s: %s (%d s)" % (label, last["msg"][:100], elapsed), force=True)

    watcher = threading.Thread(target=watch, daemon=True)
    watcher.start()
    try:
        for line in proc.stdout:
            line = line.rstrip()
            if not line:
                continue
            tail = (tail + [line])[-30:]
            ctx.log("pip: " + line[:500])   # everything pip says reaches the app's log
            m = re.match(r"(Collecting|Downloading|Building|Installing collected packages|Successfully installed|ERROR)\s*(.*)", line)
            if m:
                last["msg"] = "%s %s" % (m.group(1), m.group(2))
                ctx.progress(None, "%s: %s" % (label, last["msg"][:120]), force=True)
        code = proc.wait()
    finally:
        stop.set()
    if ctx.cancelled():
        raise Cancelled()
    if code != 0:
        raise JobError("pip failed (exit %d) installing %s:\n%s" % (code, label, "\n".join(tail[-12:])),
                       kind="missing_dependency")


def run(ctx):
    profile = ctx.param("profile", required=True, kind=str)
    files = PROFILES.get(profile)
    if files is None:
        raise JobError("Unknown runtime profile '%s'" % profile, kind="invalid_input")
    if profile != "base" and struct.calcsize("P") == 4:
        raise JobError("This is a 32-bit Python (%s). ONNX Runtime and PyTorch only publish 64-bit "
                       "packages; install the 64-bit Python from python.org." % platform.python_version(),
                       kind="missing_dependency")
    for i, name in enumerate(files):
        path = os.path.join(REQ_DIR, name)
        if not os.path.exists(path):
            raise JobError("Requirements file missing: " + path, kind="missing_dependency")
        ctx.progress(i / len(files), "Installing %s" % name, force=True)
        _pip(ctx, ["-r", path], name)
    info = {"profile": profile, "restartRequired": True}
    if profile == "diffusion-cuda":
        info["torchCuda"] = _check_torch_cuda(ctx)
    ctx.progress(1.0, "Installed %s" % profile, force=True)
    return {"outputs": {}, "info": info}


def _check_torch_cuda(ctx):
    """The CUDA profile must leave a CUDA build of torch behind (a CPU build
    from PyPI once replaced it silently). Runs in a fresh interpreter: torch
    was just installed, and importing it can take a while on first use."""
    ctx.progress(None, "Checking the PyTorch CUDA build", force=True)
    code = "import torch; print(torch.__version__); print(torch.version.cuda or '')"
    try:
        out = subprocess.run([sys.executable, "-c", code], stdin=subprocess.DEVNULL, capture_output=True,
                             text=True, timeout=600)
    except subprocess.TimeoutExpired:
        raise JobError("Importing PyTorch took more than 10 minutes", kind="missing_dependency")
    lines = out.stdout.split()
    if out.returncode != 0 or not lines:
        raise JobError("PyTorch was installed but does not import:\n" + out.stderr[-1500:], kind="missing_dependency")
    version, cuda = lines[0], (lines[1] if len(lines) > 1 else "")
    ctx.log("Installed PyTorch %s (CUDA %s)" % (version, cuda or "none"))
    if not cuda:
        raise JobError("pip installed a CPU-only PyTorch (%s) for the CUDA profile" % version, kind="missing_dependency")
    return cuda
