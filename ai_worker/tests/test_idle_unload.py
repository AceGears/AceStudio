"""Idle trim/unload policy driven by the resource profile (no GPU needed)."""

import os
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)

from fake_ml import FakeML  # noqa: E402
from acestudio_worker import imaging, residency  # noqa: E402


def wait_until(pred, timeout=5.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if pred():
            return True
        time.sleep(0.01)
    return pred()


class PolicyTests(unittest.TestCase):
    def test_defaults_trim_only(self):
        self.assertEqual(residency.idle_policy({}, environ={}), (residency.DEFAULT_TRIM_SECONDS, None))
        self.assertEqual(residency.idle_policy(None, environ={}), (residency.DEFAULT_TRIM_SECONDS, None))

    def test_profile_values_and_zero_means_never(self):
        self.assertEqual(residency.idle_policy({"idleTrimSeconds": 10, "idleUnloadSeconds": 60}, environ={}), (10.0, 60.0))
        self.assertEqual(residency.idle_policy({"idleTrimSeconds": 0, "idleUnloadSeconds": 0}, environ={}), (None, None))

    def test_environment_fallback_and_bad_values(self):
        self.assertEqual(residency.idle_policy({}, environ={"ACESTUDIO_IDLE_UNLOAD_SECONDS": "120"})[1], 120.0)
        self.assertIsNone(residency.idle_policy({}, environ={"ACESTUDIO_IDLE_UNLOAD_SECONDS": "soon"})[1])
        self.assertEqual(residency.idle_policy({"idleUnloadSeconds": True}, environ={})[1], None)   # bool is not a number
        self.assertEqual(residency.idle_policy({"idleUnloadSeconds": "60"}, environ={})[1], None)


class ReaperTests(unittest.TestCase):
    def setUp(self):
        self.lock = threading.Lock()
        self.busy = False
        self.calls = []
        self.reaper = residency.IdleReaper(self.lock, lambda: self.busy, log=None,
                                           trim=lambda: self.calls.append("trim"),
                                           unload=lambda: self.calls.append("unload") or ["image.klein"]).start()

    def tearDown(self):
        self.reaper.stop()

    def finish(self, **res):
        with self.lock:
            self.reaper.job_finished(res)

    def test_trim_then_unload_after_the_idle_times(self):
        self.finish(idleTrimSeconds=0.05, idleUnloadSeconds=0.2)
        self.assertTrue(wait_until(lambda: self.calls == ["trim", "unload"]))
        names = [a[0] for a in self.reaper.actions]
        self.assertEqual(names, ["trim", "unload"])
        self.assertGreaterEqual(self.reaper.actions[1][1], 0.19)

    def test_unload_sooner_than_trim_replaces_it(self):
        self.finish(idleTrimSeconds=5, idleUnloadSeconds=0.05)
        self.assertTrue(wait_until(lambda: self.calls == ["unload"]))
        self.assertEqual(self.reaper.pending(), [])

    def test_a_new_job_cancels_the_pending_unload(self):
        self.finish(idleTrimSeconds=0, idleUnloadSeconds=0.2)
        with self.lock:
            self.busy = True
            self.reaper.job_started()
        time.sleep(0.4)
        self.assertEqual(self.calls, [])

    def test_never_runs_while_a_job_is_busy(self):
        self.busy = True   # e.g. a job registered without job_started (defensive)
        self.finish(idleTrimSeconds=0.02, idleUnloadSeconds=0.05)
        time.sleep(0.3)
        self.assertEqual(self.calls, [])

    def test_submit_waits_for_a_running_unload(self):
        started, done = threading.Event(), threading.Event()

        def slow_unload():
            started.set()
            time.sleep(0.3)
            done.set()
            return []

        self.reaper._unload = slow_unload
        self.finish(idleTrimSeconds=0, idleUnloadSeconds=0.01)
        self.assertTrue(started.wait(5))
        with self.lock:   # what job.submit does
            self.assertTrue(done.is_set())   # the lock was only free once the unload finished

    def test_nothing_without_policy(self):
        self.finish(idleTrimSeconds=0)
        time.sleep(0.2)
        self.assertEqual(self.calls, [])


class UnloadAllTests(unittest.TestCase):
    def test_releases_registered_caches_sessions_and_cuda_cache(self):
        fx = FakeML()
        state = {"loaded": True}

        def unloader():
            had, state["loaded"] = state["loaded"], False
            return had

        with fx.installed(), mock.patch.dict(residency._unloaders, {"test.model": unloader}, clear=True):
            imaging._sessions[("m.onnx", "CUDAExecutionProvider", "all")] = object()
            released = residency.unload_all()
            self.assertEqual(released, ["test.model", "onnx"])
            self.assertIn("empty_cache", fx.names())
            self.assertEqual(residency.unload_all(), [])   # second time: nothing left

    def test_task_unloaders_are_registered(self):
        from acestudio_worker.tasks import generate, klein  # noqa: F401 - registration on import
        self.assertIn("image.klein", residency._unloaders)
        self.assertIn("image.generate", residency._unloaders)
        klein._cache.update(key="k", pipe=object(), inpaint=None)
        self.assertTrue(klein.unload())
        self.assertIsNone(klein._cache["pipe"])
        self.assertFalse(klein.unload())

    def test_failing_unloader_does_not_stop_the_others(self):
        logs = []

        def bad():
            raise RuntimeError("boom")

        with mock.patch.dict(residency._unloaders, {"bad": bad, "good": lambda: True}, clear=True):
            self.assertEqual(residency.unload_all(lambda lvl, m: logs.append(m)), ["good"])
        self.assertTrue(any("boom" in m for m in logs))


class ProtocolIdleTests(unittest.TestCase):
    """End to end through the worker process: a job's resources.idleUnloadSeconds
    leads to an unload log after the job, not during it."""

    def test_idle_unload_after_a_job(self):
        from test_server import Worker
        w = Worker()
        try:
            tmp = tempfile.mkdtemp()
            w.send("job.submit", {"job": "a", "task": "diag.echo", "dir": os.path.join(tmp, "a"),
                                  "params": {"resources": {"idleUnloadSeconds": 0.3, "idleTrimSeconds": 0}}})
            done = w.wait_for(lambda m: m.get("method") == "job.done")
            log = w.wait_for(lambda m: m.get("method") == "log" and "released models" in m["params"]["message"], 10)
            with w.cv:
                self.assertLess(w.lines.index(done), w.lines.index(log))   # after the job, never during it
            self.assertIn("Idle for", log["params"]["message"])
            self.assertEqual(done["params"]["job"], "a")
        finally:
            w.close()


if __name__ == "__main__":
    unittest.main()
