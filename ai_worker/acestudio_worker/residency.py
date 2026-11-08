"""Model residency: what the worker keeps loaded between jobs, and when it
lets go of it.

Tasks register an unloader for each model cache they keep (FLUX.2 klein,
SDXL, the live-preview decoder, Real-ESRGAN); ONNX sessions are released
through imaging.release_sessions().

Idle policy (per the resource profile; the host sends it in each job's
params.resources, the last job's values apply):

  idleTrimSeconds    after this long without a job, PyTorch's cached but
                     unused GPU blocks are given back (empty_cache). Models
                     stay loaded. Default 60 s; 0 = never.
  idleUnloadSeconds  after this long without a job, every cached model and
                     ONNX session is dropped, then empty_cache. 0 = never.
                     Default: ACESTUDIO_IDLE_UNLOAD_SECONDS, else never (the
                     host then stops the whole worker after its own idle
                     time, as before).

Nothing is released while a job runs: the reaper acts only while holding the
server's job lock with no job present, and job.submit takes the same lock,
so a job can neither start during an unload nor be unloaded under.
"""

import gc
import os
import sys
import threading
import time

DEFAULT_TRIM_SECONDS = 60.0

_unloaders = {}


def register(name, fn):
    """fn() releases one model cache and returns True if anything was loaded."""
    _unloaders[name] = fn


def free_cuda_cache():
    """torch.cuda.empty_cache() when this process has initialised CUDA."""
    torch = sys.modules.get("torch")
    if torch is None:
        return False
    try:
        if not (torch.cuda.is_available() and torch.cuda.is_initialized()):
            return False
        torch.cuda.empty_cache()
        try:
            torch.cuda.ipc_collect()
        except Exception:   # noqa: BLE001 - optional
            pass
        return True
    except Exception:   # noqa: BLE001 - releasing memory never fails a job
        return False


def unload_all(log=None):
    """Drops every registered model cache and the ONNX sessions, then gives
    the freed GPU memory back. Returns the names of what was released."""
    released = []
    for name, fn in list(_unloaders.items()):
        try:
            if fn():
                released.append(name)
        except Exception as e:   # noqa: BLE001
            if log:
                log("warning", "Could not unload %s: %s" % (name, e))
    from .imaging import release_sessions
    if release_sessions():
        released.append("onnx")
    gc.collect()
    free_cuda_cache()
    return released


def _seconds(value):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    return max(0.0, float(value))


def idle_policy(resources, environ=None):
    """(trim_seconds, unload_seconds) from a job's params.resources; None = never."""
    environ = os.environ if environ is None else environ
    res = resources if isinstance(resources, dict) else {}
    trim = _seconds(res.get("idleTrimSeconds"))
    if trim is None:
        trim = DEFAULT_TRIM_SECONDS
    unload = _seconds(res.get("idleUnloadSeconds"))
    if unload is None:
        try:
            unload = _seconds(float(environ.get("ACESTUDIO_IDLE_UNLOAD_SECONDS", "")))
        except ValueError:
            unload = None
    return (trim or None), (unload or None)


class IdleReaper:
    """Runs the idle policy on a daemon thread.

    `lock` is the server's job lock: job_started() and job_finished() are
    called with it held, and the reaper holds it while it trims or unloads.
    `busy()` (checked under the lock) says whether a job is running."""

    def __init__(self, lock, busy, log=None, clock=time.monotonic, trim=None, unload=None):
        self._cond = threading.Condition(lock)
        self._busy = busy
        self._log = log
        self._clock = clock
        self._trim = trim or free_cuda_cache
        self._unload = unload or (lambda: unload_all(log))
        self._plan = []
        self._stopped = False
        self._thread = None
        self.actions = []   # (name, idle seconds) of what ran, for tests and logs

    def start(self):
        if self._thread is None:
            self._thread = threading.Thread(target=self._loop, name="idle-reaper", daemon=True)
            self._thread.start()
        return self

    def stop(self):
        with self._cond:
            self._stopped = True
            self._cond.notify_all()

    # Called with the lock held.
    def job_started(self):
        self._plan = []
        self._cond.notify_all()

    def job_finished(self, resources):
        trim, unload = idle_policy(resources)
        now = self._clock()
        plan = []
        if unload is not None:
            plan.append((now + unload, "unload", now))
        if trim is not None and (unload is None or trim < unload):
            plan.append((now + trim, "trim", now))
        plan.sort()
        self._plan = plan
        self._cond.notify_all()

    def pending(self):
        with self._cond:
            return [name for _, name, _ in self._plan]

    def _loop(self):
        with self._cond:
            while not self._stopped:
                if not self._plan:
                    self._cond.wait()
                    continue
                deadline, name, since = self._plan[0]
                left = deadline - self._clock()
                if left > 0:
                    self._cond.wait(min(left, 5.0))
                    continue
                self._plan.pop(0)
                if self._busy():
                    self._plan = []   # a job is running: never release under it
                    continue
                idle = self._clock() - since
                try:
                    result = self._trim() if name == "trim" else self._unload()
                except Exception as e:   # noqa: BLE001
                    if self._log:
                        self._log("warning", "Idle %s failed: %s" % (name, e))
                    continue
                self.actions.append((name, idle))
                if self._log and name == "unload":
                    what = ", ".join(result) if isinstance(result, (list, tuple)) and result else "nothing was loaded"
                    self._log("info", "Idle for %d s: released models (%s)" % (idle, what))
