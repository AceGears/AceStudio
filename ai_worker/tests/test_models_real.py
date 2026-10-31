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


def build_tiny_controlnet(model_dir, path):
    """A random ControlNet derived from the tiny SDXL UNet."""
    from diffusers import ControlNetModel, UNet2DConditionModel
    import torch
    unet = UNet2DConditionModel.from_pretrained(os.path.join(model_dir, "unet"))
    cn = ControlNetModel.from_unet(unet, conditioning_embedding_out_channels=(8, 16))
    # A fresh ControlNet's output convolutions are zero (it starts as a no-op);
    # randomise them so the conditioning image really affects the result.
    torch.manual_seed(2)
    with torch.no_grad():
        for m in list(cn.controlnet_down_blocks) + [cn.controlnet_mid_block, cn.controlnet_cond_embedding.conv_out]:
            m.weight.normal_(0, 0.5)
    cn.save_pretrained(path)


def build_tiny_ip_adapter(model_dir, path):
    """A random IP-Adapter (ImageProjection format) for the tiny UNet, plus a tiny
    CLIP vision encoder, in the folder layout of the h94/IP-Adapter repository."""
    import torch
    from diffusers import UNet2DConditionModel
    from transformers import CLIPVisionConfig, CLIPVisionModelWithProjection
    torch.manual_seed(1)
    unet = UNet2DConditionModel.from_pretrained(os.path.join(model_dir, "unet"))
    cross = unet.config.cross_attention_dim
    clip_dim = 32
    channels = list(unet.config.block_out_channels)
    ip = {}
    key_id = 1
    for name in unet.attn_processors.keys():
        if name.endswith("attn1.processor"):
            continue
        if name.startswith("mid_block"):
            hidden = channels[-1]
        elif name.startswith("up_blocks"):
            hidden = list(reversed(channels))[int(name[len("up_blocks.")])]
        else:
            hidden = channels[int(name[len("down_blocks.")])]
        ip["%d.to_k_ip.weight" % key_id] = torch.randn(hidden, cross) * 0.02
        ip["%d.to_v_ip.weight" % key_id] = torch.randn(hidden, cross) * 0.02
        key_id += 2
    proj = {"proj.weight": torch.randn(4 * cross, clip_dim) * 0.02, "proj.bias": torch.zeros(4 * cross),
            "norm.weight": torch.ones(cross), "norm.bias": torch.zeros(cross)}
    os.makedirs(os.path.join(path, "sdxl_models"), exist_ok=True)
    torch.save({"image_proj": proj, "ip_adapter": ip}, os.path.join(path, "sdxl_models", "ip-adapter_tiny.bin"))
    enc = CLIPVisionModelWithProjection(CLIPVisionConfig(hidden_size=32, intermediate_size=37, projection_dim=clip_dim,
                                                         num_attention_heads=4, num_hidden_layers=2, image_size=224,
                                                         patch_size=32))
    enc.save_pretrained(os.path.join(path, "models", "image_encoder"))


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

    def test_pose_controlnet_and_reference_adapter(self):
        from PIL import Image
        cn = os.path.join(self.tmp, "tiny-controlnet")
        ipd = os.path.join(self.tmp, "tiny-ip")
        build_tiny_controlnet(self.model, cn)
        build_tiny_ip_adapter(self.model, ipd)
        pose = os.path.join(self.tmp, "pose.png")
        Image.new("RGB", (64, 64), (0, 0, 0)).save(pose)
        ref = os.path.join(self.tmp, "ref.png")
        Image.new("RGBA", (40, 40), (200, 30, 30, 255)).save(ref)
        w_ = Worker()
        try:
            base = {"model_dir": self.model, "prompt": "a red knight", "width": 64, "height": 64, "steps": 3,
                    "device": "cpu", "guidance": 5.0, "seed": 11}
            a = run_job(w_, self.tmp, "c1", "image.generate", dict(base, controlnet={"dir": cn, "image": pose, "scale": 0.9}))
            self.assertEqual(a["method"], "job.done", a)
            self.assertTrue(a["params"]["info"]["controlnet"])
            self.assertEqual(a["params"]["info"]["weights"], "fp32")
            # FP8 weight storage (used automatically on <10 GB GPUs) runs the
            # same pipeline with the UNet and ControlNet held in float8.
            q = run_job(w_, self.tmp, "c1q", "image.generate",
                        dict(base, weights="fp8", controlnet={"dir": cn, "image": pose, "scale": 0.9}))
            self.assertEqual(q["method"], "job.done", q)
            self.assertEqual(q["params"]["info"]["weights"], "fp8-storage")
            bad = run_job(w_, self.tmp, "c1b", "image.generate", dict(base, weights="half"))
            self.assertEqual(bad["params"]["kind"], "invalid_input")
            # A different pose image changes the result (the ControlNet is wired in).
            pose2 = os.path.join(self.tmp, "pose2.png")
            Image.new("RGB", (64, 64), (255, 255, 255)).save(pose2)
            b = run_job(w_, self.tmp, "c2", "image.generate", dict(base, controlnet={"dir": cn, "image": pose2, "scale": 0.9}))
            with open(a["params"]["outputs"]["image0"], "rb") as fa, open(b["params"]["outputs"]["image0"], "rb") as fb:
                self.assertNotEqual(fa.read(), fb.read())
            ipp = {"dir": ipd, "weight": "ip-adapter_tiny.bin", "encoder": "models/image_encoder", "images": [ref], "scale": 0.7}
            c = run_job(w_, self.tmp, "c3", "image.generate", dict(base, controlnet={"dir": cn, "image": pose}, ip_adapter=ipp))
            self.assertEqual(c["method"], "job.done", c)
            self.assertTrue(c["params"]["info"]["ip_adapter"])
            two = dict(ipp, images=[ref, ref])
            d = run_job(w_, self.tmp, "c4", "image.generate", dict(base, ip_adapter=two))
            self.assertEqual(d["method"], "job.done", d)
            # Bad parameters are reported, not fatal.
            e = run_job(w_, self.tmp, "c5", "image.generate", dict(base, controlnet={"dir": cn}))
            self.assertEqual(e["method"], "job.failed")
            f = run_job(w_, self.tmp, "c6", "image.generate", dict(base, ip_adapter=dict(ipp, weight="missing.bin")))
            self.assertEqual(f["params"]["kind"], "missing_model")
        finally:
            w_.close()

    def test_live_preview_and_lora_free_paths(self):
        from diffusers import AutoencoderTiny
        from PIL import Image
        tae = os.path.join(self.tmp, "tiny-taesd")
        AutoencoderTiny(encoder_block_out_channels=(8, 8, 8, 8), decoder_block_out_channels=(8, 8, 8, 8),
                        num_encoder_blocks=(1, 1, 1, 1), num_decoder_blocks=(1, 1, 1, 1)).save_pretrained(tae)
        w_ = Worker()
        try:
            base = {"model_dir": self.model, "prompt": "a red knight", "width": 64, "height": 64, "steps": 4,
                    "device": "cpu", "guidance": 5.0, "seed": 7}
            a = run_job(w_, self.tmp, "p1", "image.generate", dict(base, preview={"dir": tae, "every": 2}))
            self.assertEqual(a["method"], "job.done", a)
            previews = [m["params"]["preview"] for m in w_.lines if m.get("method") == "job.progress"
                        and m["params"].get("job") == "p1" and "preview" in m["params"]]
            self.assertEqual(len(previews), 2)                       # steps 2 and 4
            for pth in previews:
                self.assertTrue(pth.startswith(os.path.join(self.tmp, "p1")))
                im = Image.open(pth)
                self.assertEqual(im.mode, "RGB")
                self.assertEqual(im.size[0], im.size[1])
                self.assertLessEqual(max(im.size), 256)              # previews are capped at 256 px
            # A missing preview model never fails the generation.
            b = run_job(w_, self.tmp, "p2", "image.generate", dict(base, preview={"dir": os.path.join(self.tmp, "none")}))
            self.assertEqual(b["method"], "job.done", b)
        finally:
            w_.close()


if __name__ == "__main__":
    unittest.main()
