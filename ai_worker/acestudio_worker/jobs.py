import os
import threading
import time

from .metrics import Stages


class Cancelled(Exception):
    """Raised inside a task when the host asked to cancel the job."""


class JobError(Exception):
    """A failure with a user-facing message. `kind` lets the host react:
    error | oom | missing_dependency | missing_model | invalid_input."""

    def __init__(self, code_or_message, message=None, kind="error"):
        if message is None:
            super().__init__(code_or_message)
            self.code = -32000
        else:
            super().__init__(message)
            self.code = code_or_message
        self.kind = kind


class Context:
    def __init__(self, job_id, job_dir, params, server):
        self.job_id = job_id
        self.dir = job_dir
        self.params = params
        self._server = server
        self._cancel = threading.Event()
        self._last_progress = 0.0
        self.stages = Stages()
        self._running = threading.Event()   # cleared while paused (Pause All)
        self._running.set()
        self.paused_seconds = 0.0

    # Pause All: the job holds at its next safe point (between diffusion steps).
    def pause(self):
        self._running.clear()

    def resume(self):
        self._running.set()

    def wait_if_paused(self):
        if self._running.is_set():
            return
        t0 = time.monotonic()
        self.progress(None, "Paused", force=True)
        while not self._running.wait(0.2):
            self.check_cancel()
        self.paused_seconds += time.monotonic() - t0
        self.check_cancel()

    def rest_after_step(self, step_seconds):
        """Resource profile: with gpuDutyPercent < 100 the worker rests after a
        step so the GPU averages that share (lower heat, fan noise and power)
        instead of running flat out."""
        res = self.params.get("resources") or {}
        duty = res.get("gpuDutyPercent", 100) if isinstance(res, dict) else 100
        if not isinstance(duty, (int, float)) or duty >= 100 or duty <= 0 or step_seconds <= 0:
            return
        rest = step_seconds * (100.0 / max(20.0, float(duty)) - 1.0)
        end = time.monotonic() + rest
        while True:
            left = end - time.monotonic()
            if left <= 0:
                break
            if self._cancel.wait(min(0.2, left)):
                break
        self.stages.add("dutyRest", rest)

    def stage(self, name):
        """`with ctx.stage("diffusion"):` times a part of the job for info["metrics"]."""
        return self.stages.stage(name)

    # Cancellation
    def request_cancel(self):
        self._cancel.set()

    def cancelled(self):
        return self._cancel.is_set()

    def check_cancel(self):
        if self._cancel.is_set():
            raise Cancelled()

    # Reporting
    def progress(self, fraction=None, message="", step=None, steps=None, force=False, preview=None,
                 uninterruptible=False):
        """Report progress. Rate-limited to ~20/s unless forced, so tight loops
        cannot flood the pipe. `uninterruptible` tells the app the job is in a
        stage it cannot stop (loading a model): a cancel then waits for the
        stage to end instead of restarting the worker and losing the model."""
        now = time.monotonic()
        if not force and now - self._last_progress < 0.05 and (fraction is None or fraction < 1.0):
            return
        self._last_progress = now
        p = {"job": self.job_id, "message": message}
        if fraction is not None:
            p["progress"] = max(0.0, min(1.0, float(fraction)))
        if step is not None:
            p["step"] = int(step)
        if steps is not None:
            p["steps"] = int(steps)
        if preview is not None:
            p["preview"] = str(preview)   # a PNG inside the job folder
        if uninterruptible:
            p["uninterruptible"] = True
        self._server.notify("job.progress", p)

    def log(self, message, level="info"):
        self._server.log(level, message)

    def path(self, name):
        return os.path.join(self.dir, name)

    def param(self, name, default=None, kind=None, required=False):
        if name not in self.params:
            if required:
                raise JobError(-32602, "Missing parameter '%s'" % name, kind="invalid_input")
            return default
        value = self.params[name]
        if kind is not None and not isinstance(value, kind):
            raise JobError(-32602, "Parameter '%s' has the wrong type" % name, kind="invalid_input")
        return value
