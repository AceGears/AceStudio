"""ONNX Runtime session reuse and the capped CUDA arena (no GPU, no
onnxruntime needed: a fake module stands in for it)."""

import os
import sys
import tempfile
import types
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from acestudio_worker import imaging  # noqa: E402

GB = 2**30


def fake_ort(providers=("CUDAExecutionProvider", "CPUExecutionProvider")):
    created = []

    class SessionOptions:
        pass

    class Session:
        def __init__(self, path, sess_options=None, providers=None):
            self.path = path
            self.providers = providers
            created.append(self)

        def get_providers(self):
            first = self.providers[0]
            return [first[0] if isinstance(first, tuple) else first]

    ort = types.ModuleType("onnxruntime")
    ort.get_available_providers = lambda: list(providers)
    ort.SessionOptions = SessionOptions
    ort.GraphOptimizationLevel = types.SimpleNamespace(ORT_ENABLE_ALL=99, ORT_ENABLE_BASIC=1, ORT_DISABLE_ALL=0)
    ort.ExecutionMode = types.SimpleNamespace(ORT_SEQUENTIAL=0)
    ort.InferenceSession = Session
    ort.created = created
    return ort


class Ctx:
    def __init__(self):
        self.logs = []

    def log(self, message, level="info"):
        self.logs.append((level, message))


class ArenaCapTests(unittest.TestCase):
    def setUp(self):
        self.env = mock.patch.dict(os.environ, {}, clear=False)
        self.env.start()
        os.environ.pop("ACESTUDIO_ORT_GPU_MEM_MB", None)

    def tearDown(self):
        self.env.stop()

    def test_limit_follows_free_vram_within_bounds(self):
        self.assertEqual(imaging.ort_gpu_mem_limit(8 * GB), imaging.ORT_GPU_MAX_BYTES)
        self.assertEqual(imaging.ort_gpu_mem_limit(2 * GB), 2 * GB - imaging.ORT_GPU_HEADROOM_BYTES)
        self.assertEqual(imaging.ort_gpu_mem_limit(GB // 2), imaging.ORT_GPU_MIN_BYTES)

    def test_unknown_free_vram_uses_the_maximum(self):
        with mock.patch("acestudio_worker.gpu.free_vram", lambda: None):
            self.assertEqual(imaging.ort_gpu_mem_limit(), imaging.ORT_GPU_MAX_BYTES)

    def test_environment_override(self):
        os.environ["ACESTUDIO_ORT_GPU_MEM_MB"] = "1536"
        self.assertEqual(imaging.ort_gpu_mem_limit(8 * GB), 1536 * 2**20)
        os.environ["ACESTUDIO_ORT_GPU_MEM_MB"] = "0"
        self.assertIsNone(imaging.ort_gpu_mem_limit(8 * GB))
        self.assertNotIn("gpu_mem_limit", imaging.cuda_provider_options(8 * GB))

    def test_provider_options(self):
        o = imaging.cuda_provider_options(3 * GB)
        self.assertEqual(o["arena_extend_strategy"], "kSameAsRequested")
        self.assertEqual(o["cudnn_conv_algo_search"], "HEURISTIC")
        self.assertEqual(int(o["gpu_mem_limit"]), 3 * GB - imaging.ORT_GPU_HEADROOM_BYTES)


class SessionReuseTests(unittest.TestCase):
    def setUp(self):
        self.ort = fake_ort()
        self.mods = mock.patch.dict(sys.modules, {"onnxruntime": self.ort})
        self.mods.start()
        self.free = mock.patch("acestudio_worker.gpu.free_vram", lambda: 6 * GB)
        self.free.start()
        imaging._sessions.clear()
        imaging._working.clear()
        fd, self.model = tempfile.mkstemp(suffix=".onnx")
        os.close(fd)

    def tearDown(self):
        imaging._sessions.clear()
        imaging._working.clear()
        self.free.stop()
        self.mods.stop()
        os.remove(self.model)

    def test_cuda_session_gets_the_capped_arena(self):
        sess, used = imaging.onnx_session(self.model)
        self.assertEqual(used, "CUDAExecutionProvider")
        name, opts = sess.providers[0]
        self.assertEqual(name, "CUDAExecutionProvider")
        self.assertEqual(int(opts["gpu_mem_limit"]), imaging.ORT_GPU_MAX_BYTES)
        self.assertEqual(opts["cudnn_conv_algo_search"], "HEURISTIC")
        self.assertIn("CPUExecutionProvider", sess.providers)   # fallback for missing operators

    def test_cpu_session_has_no_cuda_options(self):
        sess, used = imaging.onnx_session(self.model, device="cpu")
        self.assertEqual(used, "CPUExecutionProvider")
        self.assertEqual(sess.providers, ["CPUExecutionProvider"])

    def test_session_is_reused_between_jobs_and_released_cleanly(self):
        ctx = Ctx()
        for _ in range(3):
            r, used = imaging.run_onnx(ctx, self.model, "auto", lambda s, p: id(s))
            self.assertEqual(used, "CUDAExecutionProvider")
        self.assertEqual(len(self.ort.created), 1)          # built once, reused twice
        self.assertEqual(imaging.cached_session_count(), 1)
        self.assertEqual(imaging.release_sessions(), 1)
        self.assertEqual(imaging.cached_session_count(), 0)
        imaging.run_onnx(ctx, self.model, "auto", lambda s, p: None)
        self.assertEqual(len(self.ort.created), 2)          # rebuilt after the release

    def test_failed_run_drops_the_session(self):
        ctx = Ctx()

        def fn(sess, provider):
            if provider != "CPUExecutionProvider":
                raise RuntimeError("Failed to allocate memory (arena limit)")
            return "cpu"

        r, used = imaging.run_onnx(ctx, self.model, "auto", fn)
        self.assertEqual((r, used), ("cpu", "CPUExecutionProvider"))
        self.assertTrue(all(k[1] == "CPUExecutionProvider" for k in imaging._sessions))


if __name__ == "__main__":
    unittest.main()
