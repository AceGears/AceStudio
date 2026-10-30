"""Protocol loop: reads requests on stdin, runs one job at a time on a
background thread, and writes responses/notifications on stdout.

stdout is reserved for the protocol. Anything else that writes to file
descriptor 1 (print(), C libraries, tqdm) is redirected to stderr, which the
host captures into its log.
"""

import json
import os
import platform
import sys
import threading
import traceback

from . import PROTOCOL_VERSION, __version__
from .jobs import Cancelled, Context, JobError
from . import tasks as _tasks

# Error codes (JSON-RPC reserves -32768..-32000; these are implementation-defined).
ERR_PARSE = -32700
ERR_INVALID = -32600
ERR_METHOD = -32601
ERR_PARAMS = -32602
ERR_BUSY = -32001
ERR_UNKNOWN_TASK = -32002
ERR_UNAVAILABLE = -32003


class Server:
    def __init__(self, out):
        self._out = out
        self._write_lock = threading.Lock()
        self._job = None            # (job_id, Context, thread)
        self._job_lock = threading.Lock()
        self.shutting_down = False

    # -- output -----------------------------------------------------------
    def send(self, obj):
        line = json.dumps(obj, ensure_ascii=False, separators=(",", ":"))
        with self._write_lock:
            self._out.write(line + "\n")
            self._out.flush()

    def notify(self, method, params):
        self.send({"jsonrpc": "2.0", "method": method, "params": params})

    def respond(self, req_id, result=None, error=None):
        msg = {"jsonrpc": "2.0", "id": req_id}
        if error is not None:
            msg["error"] = error
        else:
            msg["result"] = result if result is not None else {}
        self.send(msg)

    def log(self, level, message):
        self.notify("log", {"level": level, "message": message})

    # -- requests ---------------------------------------------------------
    def handle_line(self, line):
        line = line.strip()
        if not line:
            return
        try:
            req = json.loads(line)
        except ValueError as e:
            self.respond(None, error={"code": ERR_PARSE, "message": "Parse error: %s" % e})
            return
        if not isinstance(req, dict) or not isinstance(req.get("method"), str):
            self.respond(req.get("id") if isinstance(req, dict) else None,
                         error={"code": ERR_INVALID, "message": "Invalid request"})
            return
        req_id = req.get("id")
        params = req.get("params") or {}
        method = req["method"]
        handler = {
            "hello": self.on_hello,
            "job.submit": self.on_submit,
            "job.cancel": self.on_cancel,
            "shutdown": self.on_shutdown,
        }.get(method)
        if handler is None:
            self.respond(req_id, error={"code": ERR_METHOD, "message": "Unknown method: " + method})
            return
        try:
            result = handler(params)
        except JobError as e:
            self.respond(req_id, error={"code": e.code, "message": str(e)})
            return
        except Exception as e:  # never let a bad request kill the worker
            self.respond(req_id, error={"code": ERR_INVALID, "message": "%s: %s" % (type(e).__name__, e)})
            return
        if req_id is not None:
            self.respond(req_id, result)
        if method == "hello":
            self.probe_devices()

    def on_hello(self, params):
        return {
            "protocol": PROTOCOL_VERSION,
            "version": __version__,
            "python": platform.python_version(),
            "platform": platform.platform(),
            "executable": sys.executable,
            "pid": os.getpid(),
            "capabilities": _tasks.registry.capabilities(probe=False),
            "tasks": _tasks.registry.describe(),
        }

    def probe_devices(self):
        """Sends a 'capabilities' notification once torch/ONNX devices are known."""
        def work():
            try:
                caps = _tasks.registry.capabilities(probe=True)
            except Exception as e:
                caps = {"probed": True, "probeError": "%s: %s" % (type(e).__name__, e)}
            self.notify("capabilities", caps)
        threading.Thread(target=work, name="probe", daemon=True).start()

    def on_submit(self, params):
        job_id = params.get("job")
        task_name = params.get("task")
        job_dir = params.get("dir")
        if not isinstance(job_id, str) or not isinstance(task_name, str) or not isinstance(job_dir, str):
            raise JobError(ERR_PARAMS, "job.submit needs string 'job', 'task' and 'dir'")
        task = _tasks.registry.get(task_name)
        if task is None:
            raise JobError(ERR_UNKNOWN_TASK, "Unknown task: " + task_name)
        ok, reason = _tasks.registry.availability(task_name)
        if not ok:
            raise JobError(ERR_UNAVAILABLE, reason)
        with self._job_lock:
            if self._job is not None:
                raise JobError(ERR_BUSY, "Worker is busy with job " + self._job[0])
            ctx = Context(job_id, job_dir, params.get("params") or {}, self)
            thread = threading.Thread(target=self._run, args=(task, ctx), name="job-" + job_id, daemon=True)
            self._job = (job_id, ctx, thread)
        thread.start()
        return {"accepted": True}

    def on_cancel(self, params):
        with self._job_lock:
            if self._job is None or self._job[0] != params.get("job"):
                return {"cancelling": False}
            self._job[1].request_cancel()
        return {"cancelling": True}

    def on_shutdown(self, params):
        self.shutting_down = True
        with self._job_lock:
            if self._job is not None:
                self._job[1].request_cancel()
        return {}

    # -- job thread -------------------------------------------------------
    def _run(self, task, ctx):
        # Decide the outcome first, free the worker, and only then announce
        # it: the host may submit the next job the moment it hears "done".
        try:
            os.makedirs(ctx.dir, exist_ok=True)
            result = task.run(ctx) or {}
            ctx.check_cancel()
            outcome = ("job.done", {"job": ctx.job_id, "outputs": result.get("outputs", {}),
                                    "info": result.get("info", {})})
        except Cancelled:
            outcome = ("job.cancelled", {"job": ctx.job_id})
        except JobError as e:
            outcome = ("job.failed", {"job": ctx.job_id, "error": str(e), "kind": e.kind})
        except MemoryError:
            outcome = ("job.failed", {"job": ctx.job_id, "error": "Out of memory", "kind": "oom"})
        except Exception as e:
            kind = "oom" if "out of memory" in str(e).lower() else "error"
            sys.stderr.write(traceback.format_exc())
            outcome = ("job.failed", {"job": ctx.job_id, "error": "%s: %s" % (type(e).__name__, e), "kind": kind})
        with self._job_lock:
            self._job = None
        self.notify(*outcome)


def main():
    # Keep a private handle on the real stdout for the protocol, then point
    # fd 1 at stderr so stray prints cannot corrupt the stream.
    proto_fd = os.dup(1)
    os.dup2(2, 1)
    sys.stdout = sys.stderr
    out = os.fdopen(proto_fd, "w", encoding="utf-8", newline="\n")
    stdin = open(sys.stdin.fileno(), "r", encoding="utf-8", newline="\n", closefd=False)

    server = Server(out)
    server.log("info", "AceStudio worker %s (Python %s) ready" % (__version__, platform.python_version()))
    for line in stdin:   # EOF (host closed the pipe or died) ends the worker
        server.handle_line(line)
        if server.shutting_down:
            break
    return 0
