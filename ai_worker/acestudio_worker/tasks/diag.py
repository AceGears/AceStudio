"""Diagnostic tasks for the test suite (enabled by ACESTUDIO_WORKER_DIAG=1)."""

import os
import sys
import time

from ..jobs import JobError


class Echo:
    NAME = "diag.echo"

    @staticmethod
    def available():
        return True, ""

    @staticmethod
    def run(ctx):
        return {"outputs": {}, "info": {"params": ctx.params, "pid": os.getpid()}}


class Sleep:
    """Sleeps in `steps` slices, reporting progress and honouring cancel."""
    NAME = "diag.sleep"

    @staticmethod
    def available():
        return True, ""

    @staticmethod
    def run(ctx):
        seconds = float(ctx.param("seconds", 1.0))
        steps = int(ctx.param("steps", 10))
        out = ctx.path("result.txt")
        # Simulates loading a model: a stage that ignores cancellation.
        hold = float(ctx.param("uninterruptible_seconds", 0.0))
        if hold > 0:
            ctx.progress(0.0, "loading", force=True, uninterruptible=True)
            time.sleep(hold)
            ctx.progress(0.0, "loaded", force=True)
        for i in range(steps):
            ctx.check_cancel()
            time.sleep(seconds / steps)
            ctx.progress((i + 1) / steps, "step %d of %d" % (i + 1, steps), step=i + 1, steps=steps, force=True)
        with open(out, "w", encoding="utf-8") as f:
            f.write("slept %.2f\n" % seconds)
        return {"outputs": {"result": out}, "info": {"steps": steps}}


class Crash:
    """Exits the process abruptly after a few steps, like a native crash or
    the OS killing an out-of-memory process."""
    NAME = "diag.crash"

    @staticmethod
    def available():
        return True, ""

    @staticmethod
    def run(ctx):
        for i in range(int(ctx.param("after_steps", 2))):
            time.sleep(0.05)
            ctx.progress(0.1 * (i + 1), "about to crash", force=True)
        sys.stderr.write("diag.crash: simulated fatal error\n")
        sys.stderr.flush()
        os._exit(int(ctx.param("code", 17)))


class Fail:
    NAME = "diag.fail"

    @staticmethod
    def available():
        return True, ""

    @staticmethod
    def run(ctx):
        raise JobError(ctx.param("message", "requested failure"), kind=ctx.param("kind", "error"))


class Stubborn:
    """Ignores cancellation (like a long native call). Lets the host test its
    hard-cancel path, which kills and restarts the worker."""
    NAME = "diag.stubborn"

    @staticmethod
    def available():
        return True, ""

    @staticmethod
    def run(ctx):
        end = time.monotonic() + float(ctx.param("seconds", 30))
        ctx.progress(0.0, "busy", force=True)
        while time.monotonic() < end:
            time.sleep(0.05)
        return {"outputs": {}, "info": {}}


TASKS = [Echo, Sleep, Crash, Fail, Stubborn]
