"""Low-RAM model loading for FLUX.2 klein (fakes for torch/diffusers/transformers)."""

import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)

from fake_ml import FakeML, FakeServer, klein_model_dir  # noqa: E402
from acestudio_worker.jobs import Context  # noqa: E402
from acestudio_worker.tasks import klein  # noqa: E402


def ctx(params=None):
    return Context("j", tempfile.mkdtemp(), params or {}, FakeServer())


class KleinLowRamLoadTests(unittest.TestCase):
    def setUp(self):
        klein._cache.update(key=None, pipe=None, inpaint=None)
        self.model = klein_model_dir()

    def tearDown(self):
        klein._cache.update(key=None, pipe=None, inpaint=None)

    def load(self, fx, weights="fp8", device="cuda"):
        with fx.installed():
            return klein._pipelines(ctx(), self.model, None, device, weights, "model")

    def test_text_encoder_is_stored_in_fp8_before_the_transformer_is_read(self):
        fx = FakeML()
        pipe = self.load(fx)
        names = fx.names()
        te_load, te_cast, pipe_load = names.index("te_load"), fx.events.index(("cast", "text_encoder", "fp8")), names.index("pipeline_load")
        self.assertLess(te_load, te_cast)
        self.assertLess(te_cast, pipe_load)         # cast before the transformer's BF16 weights are loaded
        _, kwargs, got_te = fx.events[pipe_load]
        self.assertTrue(got_te)                     # the pre-cast encoder is handed to the pipeline
        self.assertTrue(kwargs["low_cpu_mem_usage"])
        self.assertTrue(kwargs["use_safetensors"])
        # The text encoder is cast exactly once; the transformer is cast after loading.
        self.assertEqual(fx.events.count(("cast", "text_encoder", "fp8")), 1)
        self.assertIn(("cast", "transformer", "fp8"), fx.events)
        self.assertEqual(pipe.offload, "model")

    def test_transformers_5_gets_dtype_and_4_gets_low_cpu_mem_usage(self):
        fx = FakeML(transformers_version="5.22.0")
        self.load(fx)
        kw = next(e[2] for e in fx.events if e[0] == "te_load")
        self.assertEqual(kw.get("dtype"), "bf16")
        self.assertNotIn("torch_dtype", kw)
        klein._cache.update(key=None, pipe=None, inpaint=None)
        fx4 = FakeML(transformers_version="4.57.1")
        self.load(fx4)
        kw = next(e[2] for e in fx4.events if e[0] == "te_load")
        self.assertEqual(kw.get("torch_dtype"), "bf16")
        self.assertTrue(kw.get("low_cpu_mem_usage"))

    def test_failed_staged_load_falls_back_to_the_one_step_load(self):
        fx = FakeML()
        fx.te_load_error = OSError("shard missing")
        self.load(fx)
        _, kwargs, got_te = next(e for e in fx.events if e[0] == "pipeline_load")
        self.assertFalse(got_te)
        self.assertEqual(fx.events.count(("cast", "text_encoder", "fp8")), 1)   # cast after the load instead

    def test_bf16_weights_load_in_one_step_without_casting(self):
        fx = FakeML()
        self.load(fx, weights="bf16")
        self.assertNotIn("te_load", fx.names())
        self.assertNotIn("cast", fx.names())

    def test_cached_pipeline_is_not_reloaded(self):
        fx = FakeML()
        with fx.installed():
            a = klein._pipelines(ctx(), self.model, None, "cuda", "fp8", "model")
            b = klein._pipelines(ctx(), self.model, None, "cuda", "fp8", "model")
        self.assertIs(a, b)
        self.assertEqual(fx.names().count("pipeline_load"), 1)


if __name__ == "__main__":
    unittest.main()
