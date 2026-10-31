import os
import threading
import time


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

    # Cancellation
    def request_cancel(self):
        self._cancel.set()

    def cancelled(self):
        return self._cancel.is_set()

    def check_cancel(self):
        if self._cancel.is_set():
            raise Cancelled()

    # Reporting
    def progress(self, fraction=None, message="", step=None, steps=None, force=False, preview=None):
        """Report progress. Rate-limited to ~20/s unless forced, so tight loops
        cannot flood the pipe."""
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
