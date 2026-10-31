"""Task registry. Each task module provides:

    NAME          task name used in job.submit
    available()   -> (bool, reason)  cheap check, no heavy imports executed
    run(ctx)      -> {"outputs": {name: path}, "info": {...}}

Diagnostic tasks (used by the test suite) are only registered when the
ACESTUDIO_WORKER_DIAG environment variable is "1".
"""

import importlib
import importlib.util
import os

_MODULES = [
    "acestudio_worker.tasks.model_download",
    "acestudio_worker.tasks.runtime_install",
    "acestudio_worker.tasks.upscale",
    "acestudio_worker.tasks.remove_background",
    "acestudio_worker.tasks.generate",
]
_DIAG_MODULES = ["acestudio_worker.tasks.diag"]


class _Registry:
    def __init__(self):
        self._tasks = {}
        mods = list(_MODULES)
        if os.environ.get("ACESTUDIO_WORKER_DIAG") == "1":
            mods += _DIAG_MODULES
        for name in mods:
            mod = importlib.import_module(name)
            for task in getattr(mod, "TASKS", [mod]):
                self._tasks[task.NAME] = task

    def get(self, name):
        return self._tasks.get(name)

    def availability(self, name):
        task = self._tasks.get(name)
        if task is None:
            return False, "Unknown task"
        try:
            return task.available()
        except Exception as e:  # a broken optional package must not break hello
            return False, "%s: %s" % (type(e).__name__, e)

    def describe(self):
        out = {}
        for name in sorted(self._tasks):
            ok, reason = self.availability(name)
            out[name] = {"available": ok, "reason": reason}
        return out

    def capabilities(self, probe=True):
        return capabilities(probe)


_registry = None


def __getattr__(attr):
    global _registry
    if attr == "registry":
        if _registry is None:
            _registry = _Registry()
        return _registry
    raise AttributeError(attr)


def has_module(name):
    try:
        return importlib.util.find_spec(name) is not None
    except (ImportError, ValueError):
        return False


def capabilities(probe=True):
    """What is installed and which accelerators exist. Importing torch can
    take tens of seconds, so `probe=False` reports only what is installed;
    the server probes devices on a background thread after hello."""
    caps = {
        "numpy": has_module("numpy"),
        "pillow": has_module("PIL"),
        "torch": has_module("torch"),
        "diffusers": has_module("diffusers"),
        "onnxruntime": has_module("onnxruntime"),
        "devices": [],
        "onnxProviders": [],
        "probed": bool(probe),
    }
    if not probe:
        return caps
    from ..gpu import nvidia_driver
    driver = nvidia_driver()
    if driver:
        caps["nvidiaDriver"] = driver
    if caps["torch"]:
        try:
            import torch
            caps["torchVersion"] = torch.__version__
            caps["cuda"] = bool(torch.cuda.is_available())
            if caps["cuda"]:
                for i in range(torch.cuda.device_count()):
                    prop = torch.cuda.get_device_properties(i)
                    caps["devices"].append({"index": i, "name": prop.name,
                                            "vramMb": int(prop.total_memory // (1024 * 1024))})
        except Exception as e:
            caps["torchError"] = "%s: %s" % (type(e).__name__, e)
    if caps["onnxruntime"]:
        try:
            import onnxruntime
            caps["onnxProviders"] = list(onnxruntime.get_available_providers())
        except Exception as e:
            caps["onnxError"] = "%s: %s" % (type(e).__name__, e)
    return caps
