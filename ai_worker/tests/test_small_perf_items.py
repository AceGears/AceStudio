"""Real-ESRGAN model cache, background preview PNG writes, and tile stitching
without a per-tile device sync. Stdlib-only tests always run; the stitching
tests need numpy (and torch for the tensor path) and are skipped without."""

import importlib.util
import os
import sys
import tempfile
import threading
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)

from fake_ml import FakeServer  # noqa: E402
from acestudio_worker import imaging, residency  # noqa: E402
from acestudio_worker.jobs import Context  # noqa: E402
from acestudio_worker.tasks import upscale  # noqa: E402


def has(*mods):
    return all(importlib.util.find_spec(m) is not None for m in mods)


class FakeImage:
    def __init__(self, tag, delay=0.0, fail=False):
        self.tag, self.delay, self.fail = tag, delay, fail

    def save(self, path, format=None, optimize=False):
        time.sleep(self.delay)
        if self.fail:
            raise OSError("disk full")
        with open(path, "w") as f:
            f.write(self.tag)


class PngWriterTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()

    def p(self, name):
        return os.path.join(self.dir, name)

    def test_submit_returns_at_once_and_announces_complete_files(self):
        seen = []

        def on_written(path, payload):
            with open(path) as f:
                seen.append((f.read(), payload))   # the file is complete when announced

        w = imaging.PngWriter(on_written=on_written)
        t0 = time.monotonic()
        w.submit(FakeImage("a", delay=0.3), self.p("a.png"), 1)
        self.assertLess(time.monotonic() - t0, 0.1)   # the step thread does not wait for the write
        w.close()
        self.assertEqual(seen, [("a", 1)])

    def test_latest_preview_wins_and_close_flushes(self):
        w = imaging.PngWriter()
        w.submit(FakeImage("1", delay=0.2), self.p("1.png"))
        time.sleep(0.05)                       # 1 is being written
        w.submit(FakeImage("2"), self.p("2.png"))
        w.submit(FakeImage("3"), self.p("3.png"))   # replaces 2, still waiting
        w.close()
        self.assertEqual([os.path.basename(x) for x in w.written], ["1.png", "3.png"])
        self.assertEqual(w.dropped, 1)
        self.assertFalse(os.path.exists(self.p("2.png")))

    def test_errors_are_reported_not_raised(self):
        errors = []
        w = imaging.PngWriter(on_error=errors.append)
        w.submit(FakeImage("x", fail=True), self.p("x.png"))
        w.close()
        self.assertEqual(len(errors), 1)
        w.submit(FakeImage("late"), self.p("late.png"))   # after close: ignored
        self.assertFalse(os.path.exists(self.p("late.png")))

    def test_progress_notification_carries_the_preview(self):
        srv = FakeServer()
        ctx = Context("j", self.dir, {}, srv)
        w = imaging.PngWriter(on_written=lambda path, _p: ctx.progress(None, "Step 5 of 30", force=True, preview=path))
        w.submit(FakeImage("p"), ctx.path("preview_5.png"))
        w.close()
        prog = [m for k, m in srv.sent if k == "job.progress"]
        self.assertEqual(prog[-1]["preview"], ctx.path("preview_5.png"))
        self.assertNotIn("progress", prog[-1])   # no fraction: it cannot move the bar backwards


class EsrganCacheTests(unittest.TestCase):
    def setUp(self):
        upscale.unload()
        fd, self.model = tempfile.mkstemp(suffix=".pth")
        os.write(fd, b"weights")
        os.close(fd)
        self.loads = []

    def tearDown(self):
        upscale.unload()
        os.remove(self.model)

    def load(self, path, dev):
        self.loads.append((path, dev))

        class Net:
            def to(self, dtype):
                self.dtype = dtype
                return self
        return Net(), 4

    def test_model_is_loaded_once_per_file_device_and_dtype(self):
        a, s = upscale.esrgan_model(self.model, "cuda", "fp16", self.load)
        b, _ = upscale.esrgan_model(self.model, "cuda", "fp16", self.load)
        self.assertIs(a, b)
        self.assertEqual((s, a.dtype, len(self.loads)), (4, "fp16", 1))
        upscale.esrgan_model(self.model, "cpu", "fp32", self.load)
        self.assertEqual(len(self.loads), 2)

    def test_changed_file_is_reloaded(self):
        upscale.esrgan_model(self.model, "cuda", "fp16", self.load)
        with open(self.model, "ab") as f:
            f.write(b"more")
        upscale.esrgan_model(self.model, "cuda", "fp16", self.load)
        self.assertEqual(len(self.loads), 2)

    def test_idle_unload_drops_it(self):
        upscale.esrgan_model(self.model, "cuda", "fp16", self.load)
        self.assertIn("upscale.esrgan", residency._unloaders)
        self.assertTrue(residency._unloaders["upscale.esrgan"]())
        upscale.esrgan_model(self.model, "cuda", "fp16", self.load)
        self.assertEqual(len(self.loads), 2)


def _ctx():
    return Context("j", tempfile.mkdtemp(), {}, FakeServer())


@unittest.skipUnless(has("numpy"), "numpy missing")
class StitchTests(unittest.TestCase):
    def test_numpy_stitch_matches_whole_image(self):
        import numpy as np
        rng = np.random.default_rng(1)
        rgb = rng.random((70, 101, 3), dtype=np.float32)

        def infer(patch):   # nearest-neighbour x2, HWC in and out
            return patch.repeat(2, axis=0).repeat(2, axis=1)

        out = upscale.upscale_rgb(_ctx(), rgb, infer, 2, 32)
        np.testing.assert_array_equal(out, infer(rgb))

    @unittest.skipUnless(has("torch"), "torch missing")
    def test_tensor_stitch_has_no_per_tile_host_copy(self):
        import numpy as np
        import torch
        rgb = np.random.default_rng(2).random((50, 66, 3), dtype=np.float32)
        copies = []

        class Tracked(torch.Tensor):
            def cpu(self, *a, **k):
                copies.append(1)
                return super().cpu(*a, **k)

        def infer(patch):
            return patch.repeat_interleave(2, 0).repeat_interleave(2, 1)

        src = torch.from_numpy(rgb).as_subclass(Tracked)
        out = upscale.upscale_rgb(_ctx(), src, infer, 2, 16,
                                  new_canvas=lambda shape: torch.zeros(shape).as_subclass(Tracked))
        self.assertEqual(copies, [])   # nothing copied while stitching
        np.testing.assert_array_equal(out.cpu().numpy(), rgb.repeat(2, 0).repeat(2, 1))


@unittest.skipUnless(has("numpy", "PIL", "torch"), "numpy, Pillow or torch missing")
class TorchUpscaleTaskTests(unittest.TestCase):
    """The .pth path end to end on the CPU with a tiny randomly initialised
    RRDBNet (no real model): tiled output equals the whole-image forward
    pass away from tile seams, and the model is cached between jobs."""

    def test_tiled_task_matches_whole_image_and_caches_the_model(self):
        import numpy as np
        import torch
        from PIL import Image
        from acestudio_worker.rrdbnet import RRDBNet
        torch.manual_seed(0)
        tmp = tempfile.mkdtemp()
        net = RRDBNet(scale=4, num_feat=8, num_block=1)   # grow 32: what rrdbnet.load assumes.eval()
        model = os.path.join(tmp, "tiny.pth")
        torch.save({"params_ema": net.state_dict()}, model)
        rgb = (np.random.default_rng(3).random((50, 70, 3)) * 255).astype(np.uint8)
        Image.fromarray(rgb, "RGB").convert("RGBA").save(os.path.join(tmp, "in.png"))
        upscale.unload()
        loads = []
        real_load = upscale.esrgan_model

        def counting(path, dev, dtype, load):
            return real_load(path, dev, dtype, lambda p, d: loads.append(p) or load(p, d))

        params = {"input": os.path.join(tmp, "in.png"), "model": model, "tile": 32, "device": "cpu"}   # 3 x 2 tiles
        try:
            upscale.esrgan_model = counting
            for job in ("a", "b"):
                d = os.path.join(tmp, job)
                os.makedirs(d)
                r = upscale.run(Context(job, d, dict(params), FakeServer()))
        finally:
            upscale.esrgan_model = real_load
            upscale.unload()
        self.assertEqual(len(loads), 1)   # cached for the second job
        out = np.asarray(Image.open(r["outputs"]["image"]).convert("RGB"), dtype=np.float32)
        with torch.inference_mode():
            ref = net(torch.from_numpy(rgb.astype(np.float32) / 255.0).permute(2, 0, 1)[None])[0]
        ref = (ref.clamp(0, 1).permute(1, 2, 0).numpy() * 255.0 + 0.5).astype(np.uint8)
        self.assertEqual(out.shape, (200, 280, 3))
        self.assertLessEqual(np.abs(out - ref.astype(np.float32)).max(), 1.0)


if __name__ == "__main__":
    unittest.main()
