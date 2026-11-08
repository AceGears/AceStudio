"""Out-of-memory recovery: free caches, retry once with less memory, then a
clear error (fakes for torch/diffusers; no GPU needed)."""

import os
import sys
import tempfile
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)

from fake_ml import FakeML, FakeOOM, FakeServer, klein_model_dir  # noqa: E402
from acestudio_worker import imaging, residency  # noqa: E402
from acestudio_worker.jobs import Cancelled, Context, JobError  # noqa: E402
from acestudio_worker.tasks import klein, upscale  # noqa: E402


class IsOomTests(unittest.TestCase):
    def test_detection(self):
        fx = FakeML()
        with fx.installed():
            self.assertTrue(residency.is_oom(FakeOOM("x")))
        self.assertTrue(residency.is_oom(RuntimeError("CUDA error: out of memory")))
        self.assertFalse(residency.is_oom(RuntimeError("shape mismatch")))
        self.assertFalse(residency.is_oom(ValueError("out of memory")))   # not a runtime error


class KleinOomTests(unittest.TestCase):
    def setUp(self):
        klein._cache.update(key=None, pipe=None, inpaint=None)
        self.server = FakeServer()

    def tearDown(self):
        klein._cache.update(key=None, pipe=None, inpaint=None)
        imaging._sessions.clear()

    def run_job(self, fx, **params):
        p = {"prompt": "a knight", "model_dir": klein_model_dir(), "width": 512, "height": 512, "seed": 7}
        p.update(params)
        ctx = Context("j", tempfile.mkdtemp(), p, self.server)
        with fx.installed(), mock.patch.object(klein, "require", lambda *a: None), \
                mock.patch.object(klein, "save_png", lambda img, path: path):
            return klein.run(ctx)

    def warnings(self):
        return [m["message"] for k, m in self.server.sent if k == "log" and m["level"] == "warning"]

    def test_one_oom_is_retried_with_less_memory(self):
        fx = FakeML()
        fx.oom_calls = 1
        imaging._sessions[("cutout.onnx", "CUDAExecutionProvider", "all")] = object()   # a kept cutout session
        r = self.run_job(fx)
        names = fx.names()
        self.assertEqual(names.count("oom"), 1)
        self.assertEqual(names.count("generate"), 1)
        retry = names.index("oom")
        self.assertIn("vae_tiling", names[retry:])
        self.assertIn("empty_cache", names[retry:names.index("generate")])
        self.assertEqual(imaging._sessions, {})   # the cutout session made room
        self.assertIn("tiled VAE", r["info"]["oomRetry"])
        self.assertIn("image0", r["outputs"])
        self.assertTrue(any("retrying once" in w for w in self.warnings()))
        self.assertIsNotNone(klein._cache["pipe"])   # the pipeline stays cached

    def test_full_gpu_pipeline_switches_to_model_offload(self):
        fx = FakeML(vram_gb=24)
        fx.oom_calls = 1
        r = self.run_job(fx, weights="bf16", offload="none")
        self.assertIn("model offload", r["info"]["oomRetry"])
        self.assertIn(("offload", "model"), fx.events[fx.names().index("oom"):])

    def test_second_oom_fails_with_a_clear_error(self):
        fx = FakeML()
        fx.oom_calls = 2
        with self.assertRaises(JobError) as cm:
            self.run_job(fx)
        self.assertEqual(cm.exception.kind, "oom")
        self.assertIn("also after a retry", str(cm.exception))
        self.assertIsNone(klein._cache["pipe"])   # dropped, as before
        self.assertEqual(fx.names().count("oom"), 2)

    def test_other_errors_are_not_retried(self):
        fx = FakeML()

        def boom(**kw):
            raise RuntimeError("shape mismatch")

        with fx.installed():
            pipe = klein._pipelines(Context("j", tempfile.mkdtemp(), {}, self.server), klein_model_dir(),
                                    None, "cuda", "fp8", "model")
        pipe.__class__.__call__ = lambda self, **kw: boom(**kw)
        try:
            with self.assertRaises(RuntimeError):
                self.run_job(fx, model_dir=klein._cache["key"][0])
        finally:
            del pipe.__class__.__call__
        self.assertNotIn("vae_tiling", fx.names())

    def test_cancel_is_not_treated_as_oom(self):
        fx = FakeML()
        p = {"prompt": "a knight", "model_dir": klein_model_dir(), "width": 512, "height": 512, "steps": 4}
        ctx = Context("j", tempfile.mkdtemp(), p, self.server)
        real_progress = ctx.progress

        def progress(*a, **k):
            if k.get("step") == 2:
                ctx.request_cancel()
            return real_progress(*a, **k)

        ctx.progress = progress
        with fx.installed(), mock.patch.object(klein, "require", lambda *a: None), \
                mock.patch.object(klein, "save_png", lambda img, path: path):
            with self.assertRaises(Cancelled):
                klein.run(ctx)
        self.assertNotIn("vae_tiling", fx.names())


class UpscaleOomTests(unittest.TestCase):
    def ctx(self):
        return Context("j", tempfile.mkdtemp(), {}, FakeServer())

    def test_retry_with_half_the_tile(self):
        tiles = []

        def run_image(tile):
            tiles.append(tile)
            if tile > 96:
                raise RuntimeError("CUDA out of memory. Tried to allocate 1.2 GiB")
            return "up"

        state = {"tile": 192, "retried": None}
        self.assertEqual(upscale.upscale_with_retry(self.ctx(), run_image, state), "up")
        self.assertEqual(tiles, [192, 96])
        self.assertEqual(state, {"tile": 96, "retried": 192})
        # Later frames keep the smaller tile and are not retried again.
        upscale.upscale_with_retry(self.ctx(), run_image, state)
        self.assertEqual(tiles[-1], 96)

    def test_second_oom_raises(self):
        def run_image(tile):
            raise RuntimeError("CUDA out of memory")

        state = {"tile": 128, "retried": None}
        with self.assertRaises(RuntimeError):
            upscale.upscale_with_retry(self.ctx(), run_image, state)
        self.assertEqual(state["tile"], 64)

    def test_smallest_tile_is_not_retried(self):
        calls = []

        def run_image(tile):
            calls.append(tile)
            raise RuntimeError("CUDA out of memory")

        with self.assertRaises(RuntimeError):
            upscale.upscale_with_retry(self.ctx(), run_image, {"tile": 48, "retried": None})
        self.assertEqual(calls, [48])


if __name__ == "__main__":
    unittest.main()
