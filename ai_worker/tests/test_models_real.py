"""Runs the real AI tasks through the worker protocol.

Skipped unless the optional packages are installed. Model-file tests also
need ACESTUDIO_TEST_MODELS pointing at a folder with the catalogue files:
  BiRefNet-general-bb_swin_v1_tiny-epoch_232.onnx, RealESRGAN_x4plus.pth

The SDXL test builds a *tiny randomly initialised* SDXL pipeline locally: it
proves the generation code path (loading, progress, cancel, seeds, img2img,
pixel post-processing) works with the real Diffusers library, not image
quality. Real SDXL weights must be tested on a GPU machine.
"""

import importlib.util
import json
import math
import os
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_server import Worker  # noqa: E402

MODELS = os.environ.get("ACESTUDIO_TEST_MODELS", "")


def has(*mods):
    return all(importlib.util.find_spec(m) is not None for m in mods)


def run_job(w, tmp, job, task, params, timeout=600):
    w.send("job.submit", {"job": job, "task": task, "params": params, "dir": os.path.join(tmp, job)})
    return w.wait_for(lambda m: m.get("method") in ("job.done", "job.failed", "job.cancelled")
                      and m["params"]["job"] == job, timeout)


@unittest.skipUnless(has("numpy", "PIL", "onnxruntime") and os.path.exists(
    os.path.join(MODELS, "BiRefNet-general-bb_swin_v1_tiny-epoch_232.onnx")), "BiRefNet or onnxruntime missing")
class BiRefNetTest(unittest.TestCase):
    def test_matte_matches_ground_truth(self):
        import numpy as np
        from PIL import Image
        tmp = tempfile.mkdtemp()
        # A filled character silhouette over a busy background; the true
        # foreground mask is known exactly.
        rng = np.random.default_rng(3)
        h, w = 512, 512
        yy, xx = np.mgrid[0:h, 0:w]
        bg = np.stack([xx / w * 120 + 60, yy / h * 100 + 80, np.full((h, w), 150.0)], -1)
        for _ in range(25):   # soft blobs of clutter
            cx, cy, r = rng.integers(0, w), rng.integers(0, h), rng.integers(15, 50)
            d = ((xx - cx) ** 2 + (yy - cy) ** 2) < r * r
            bg[d] = rng.integers(40, 220, 3)
        fg = np.zeros((h, w), bool)
        fg |= ((xx - 256) ** 2 + (yy - 150) ** 2) < 60 ** 2               # head
        fg |= (abs(xx - 256) < 70) & (yy > 200) & (yy < 380)               # body
        fg |= (abs(xx - 220) < 22) & (yy >= 380) & (yy < 480)              # legs
        fg |= (abs(xx - 292) < 22) & (yy >= 380) & (yy < 480)
        img = bg.copy()
        img[fg] = [200, 40, 40]
        img[fg & (yy < 200)] = [235, 190, 150]
        src = os.path.join(tmp, "in.png")
        Image.fromarray(img.astype(np.uint8), "RGB").save(src)

        w_ = Worker()
        try:
            t0 = time.time()
            msg = run_job(w_, tmp, "b1", "background.remove",
                          {"input": src, "model": os.path.join(MODELS, "BiRefNet-general-bb_swin_v1_tiny-epoch_232.onnx"),
                           "device": "cpu"})
            elapsed = time.time() - t0
        finally:
            w_.close()
        self.assertEqual(msg["method"], "job.done", msg)
        mask = np.asarray(Image.open(msg["params"]["outputs"]["mask"])) > 127
        iou = (mask & fg).sum() / (mask | fg).sum()
        out = np.asarray(Image.open(msg["params"]["outputs"]["image"]))
        print("\nBiRefNet lite CPU: IoU %.3f, %.1f s" % (iou, elapsed))
        self.assertGreater(iou, 0.85)
        self.assertEqual(out.shape, (h, w, 4))
        self.assertGreater(out[300, 256, 3], 200)   # body kept
        self.assertLess(out[20, 20, 3], 60)         # corner removed


@unittest.skipUnless(has("numpy", "PIL", "torch") and os.path.exists(os.path.join(MODELS, "RealESRGAN_x4plus.pth")),
                     "Real-ESRGAN weights or torch missing")
class EsrganTest(unittest.TestCase):
    def test_upscale_reconstructs_detail(self):
        import numpy as np
        from PIL import Image
        tmp = tempfile.mkdtemp()
        # Smooth shapes and edges: downscale x4, upscale with the model and
        # compare with the original. Random weights would score far lower.
        yy, xx = np.mgrid[0:256, 0:256]
        orig = np.stack([128 + 100 * np.sin(xx / 17.0), 128 + 90 * np.cos(yy / 23.0),
                         np.where(((xx - 128) ** 2 + (yy - 128) ** 2) < 70 ** 2, 220, 40)], -1).astype(np.uint8)
        hi = Image.fromarray(orig, "RGB").convert("RGBA")
        lo = hi.resize((64, 64), Image.BICUBIC)
        src = os.path.join(tmp, "lo.png")
        lo.save(src)
        w_ = Worker()
        try:
            t0 = time.time()
            msg = run_job(w_, tmp, "u1", "upscale.esrgan",
                          {"input": src, "model": os.path.join(MODELS, "RealESRGAN_x4plus.pth"), "tile": 40,
                           "device": "cpu"})
            elapsed = time.time() - t0
        finally:
            w_.close()
        self.assertEqual(msg["method"], "job.done", msg)
        self.assertEqual(msg["params"]["info"]["scale"], 4)
        up = np.asarray(Image.open(msg["params"]["outputs"]["image"]).convert("RGB"), dtype=np.float64)
        self.assertEqual(up.shape, (256, 256, 3))
        bicubic = np.asarray(lo.convert("RGB").resize((256, 256), Image.BICUBIC), dtype=np.float64)

        def psnr(a):
            mse = ((a - orig.astype(np.float64)) ** 2).mean()
            return 10 * math.log10(255 ** 2 / mse)
        print("\nReal-ESRGAN x4plus CPU 64->256 (4 tiles): PSNR %.1f dB (bicubic %.1f dB), %.1f s"
              % (psnr(up), psnr(bicubic), elapsed))
        self.assertGreater(psnr(up), 22.0)
        alpha = np.asarray(Image.open(msg["params"]["outputs"]["image"]))[..., 3]
        self.assertTrue((alpha == 255).all())


def build_tiny_sdxl(path):
    """Tiny randomly initialised SDXL pipeline saved in Diffusers format."""
    import torch
    from diffusers import AutoencoderKL, EulerDiscreteScheduler, StableDiffusionXLPipeline, UNet2DConditionModel
    from transformers import CLIPTextConfig, CLIPTextModel, CLIPTextModelWithProjection, CLIPTokenizer
    torch.manual_seed(0)
    unet = UNet2DConditionModel(
        block_out_channels=(32, 64), layers_per_block=1, sample_size=32, in_channels=4, out_channels=4,
        down_block_types=("DownBlock2D", "CrossAttnDownBlock2D"), up_block_types=("CrossAttnUpBlock2D", "UpBlock2D"),
        attention_head_dim=(2, 4), use_linear_projection=True, addition_embed_type="text_time",
        addition_time_embed_dim=8, transformer_layers_per_block=(1, 1), projection_class_embeddings_input_dim=80,
        cross_attention_dim=64, norm_num_groups=8)
    vae = AutoencoderKL(block_out_channels=[16, 32], in_channels=3, out_channels=3,
                        down_block_types=["DownEncoderBlock2D", "DownEncoderBlock2D"],
                        up_block_types=["UpDecoderBlock2D", "UpDecoderBlock2D"], latent_channels=4,
                        norm_num_groups=8, sample_size=64)
    tok_dir = os.path.join(path, "_tok")
    os.makedirs(tok_dir, exist_ok=True)
    chars = list("abcdefghijklmnopqrstuvwxyz")
    vocab = {"<|startoftext|>": 0, "<|endoftext|>": 1, "!": 2}
    for c in chars:
        vocab[c] = len(vocab)
        vocab[c + "</w>"] = len(vocab)
    with open(os.path.join(tok_dir, "vocab.json"), "w") as f:
        json.dump(vocab, f)
    with open(os.path.join(tok_dir, "merges.txt"), "w") as f:
        f.write("#version: 0.2\n")
    tok = CLIPTokenizer(os.path.join(tok_dir, "vocab.json"), os.path.join(tok_dir, "merges.txt"), pad_token="!",
                        model_max_length=77)
    cfg = dict(bos_token_id=0, eos_token_id=1, pad_token_id=2, hidden_size=32, intermediate_size=37,
               layer_norm_eps=1e-5, num_attention_heads=4, num_hidden_layers=2, vocab_size=len(vocab),
               hidden_act="gelu", projection_dim=32)
    te = CLIPTextModel(CLIPTextConfig(**cfg))
    te2 = CLIPTextModelWithProjection(CLIPTextConfig(**cfg))
    sched = EulerDiscreteScheduler(beta_start=0.00085, beta_end=0.012, beta_schedule="scaled_linear",
                                   timestep_spacing="leading", steps_offset=1)
    pipe = StableDiffusionXLPipeline(vae=vae, text_encoder=te, text_encoder_2=te2, tokenizer=tok, tokenizer_2=tok,
                                     unet=unet, scheduler=sched)
    pipe.save_pretrained(path, safe_serialization=True)


@unittest.skipUnless(has("torch", "diffusers", "transformers", "numpy", "PIL"), "diffusers stack missing")
class TinySdxlPlumbingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp()
        cls.model = os.path.join(cls.tmp, "tiny-sdxl")
        build_tiny_sdxl(cls.model)

    def test_generate_seeds_pixelate_img2img_cancel(self):
        import numpy as np
        from PIL import Image
        w_ = Worker()
        try:
            base = {"model_dir": self.model, "prompt": "a red knight", "width": 64, "height": 64, "steps": 3,
                    "device": "cpu", "guidance": 5.0}
            a = run_job(w_, self.tmp, "g1", "image.generate", dict(base, seed=42, count=2))
            self.assertEqual(a["method"], "job.done", a)
            self.assertEqual(a["params"]["info"]["seed"], 42)
            steps = [m["params"]["step"] for m in w_.lines if m.get("method") == "job.progress"
                     and m["params"].get("job") == "g1" and "step" in m["params"]]
            self.assertEqual(steps[-1], 6)                      # 3 steps x 2 images
            img0 = np.asarray(Image.open(a["params"]["outputs"]["image0"]))
            self.assertEqual(img0.shape[:2], (64, 64))
            # Same seed, same image; pipeline is reused from the cache.
            b = run_job(w_, self.tmp, "g2", "image.generate", dict(base, seed=42))
            self.assertTrue((np.asarray(Image.open(b["params"]["outputs"]["image0"])) == img0).all())
            # Pixel-art post-process: exact size and palette.
            pal = ["#000000", "#FF0000", "#00FF00", "#FFFFFF"]
            c = run_job(w_, self.tmp, "g3", "image.generate",
                        dict(base, seed=1, pixel={"width": 16, "height": 12, "palette": pal}))
            self.assertEqual(c["method"], "job.done", c)
            px = np.asarray(Image.open(c["params"]["outputs"]["image0"]).convert("RGB"))
            self.assertEqual(px.shape, (12, 16, 3))
            allowed = {tuple(int(h[i:i + 2], 16) for i in (1, 3, 5)) for h in pal}
            self.assertTrue({tuple(p) for p in px.reshape(-1, 3)} <= allowed)
            self.assertIn("raw0", c["params"]["outputs"])
            # img2img from the first result.
            d = run_job(w_, self.tmp, "g4", "image.generate",
                        dict(base, seed=5, init_image=a["params"]["outputs"]["image0"], strength=0.5))
            self.assertEqual(d["method"], "job.done", d)
            self.assertTrue(d["params"]["info"]["img2img"])
            # Cancel mid-generation stops at the next step.
            w_.send("job.submit", {"job": "g5", "task": "image.generate", "dir": os.path.join(self.tmp, "g5"),
                                   "params": dict(base, steps=150, seed=3)})
            w_.wait_for(lambda m: m.get("method") == "job.progress" and m["params"]["job"] == "g5"
                        and m["params"].get("step", 0) >= 2)
            w_.send("job.cancel", {"job": "g5"})
            e = w_.wait_for(lambda m: m.get("method") in ("job.done", "job.failed", "job.cancelled")
                            and m["params"]["job"] == "g5", 120)
            self.assertEqual(e["method"], "job.cancelled", e)
            # Bad input is reported, not fatal.
            f = run_job(w_, self.tmp, "g6", "image.generate", dict(base, width=100))
            self.assertEqual(f["method"], "job.failed")
            self.assertIn("multiples of 8", f["params"]["error"])
            g = run_job(w_, self.tmp, "g7", "image.generate", dict(base, model_dir=os.path.join(self.tmp, "nope")))
            self.assertEqual(g["params"]["kind"], "missing_model")
        finally:
            w_.close()


if __name__ == "__main__":
    unittest.main()
