"""Text-to-image and image-to-image with Stable Diffusion XL (Diffusers).

params:
  model_dir         Diffusers-format SDXL folder (model_index.json, unet/, vae/, ...)
  prompt            text
  negative_prompt   text (optional)
  width, height     generation size, multiples of 8 (default 1024)
  steps             denoising steps (default 30)
  guidance          classifier-free guidance scale (default 6.0)
  seed              int; -1 or absent = random (the used seed is reported)
  count             images per job (default 1, max 8)
  lora              optional {"path": file.safetensors, "scale": 1.0} (e.g. pixel-art-xl)
  vae_dir           optional replacement VAE folder (e.g. sdxl-vae-fp16-fix)
  init_image        optional PNG path -> image-to-image
  strength          img2img strength 0..1 (default 0.6)
  device            auto | cuda | cpu
  offload           auto | none | model   (model = enable_model_cpu_offload, for <10 GB VRAM)
  pixel             optional post-process {"width": w, "height": h, "colors": n | "palette": ["#RRGGBB", ...]}
outputs:
  image0..imageN    PNGs (post-processed when `pixel` is given)
  raw0..rawN        the full-size generations when `pixel` is given
"""

import random
import threading

from ..imaging import load_rgba, require, save_png
from ..jobs import Cancelled, JobError

NAME = "image.generate"

_cache = {"key": None, "pipe": None, "img2img": None}
_cache_lock = threading.Lock()


def available():
    from . import has_module
    missing = [m for m in ("torch", "diffusers", "transformers", "numpy", "PIL") if not has_module(m)]
    if missing:
        return False, "Needs %s (AI Studio > Set Up AI Runtime > Image generation)" % ", ".join(missing)
    return True, ""


def _int(ctx, name, default, lo, hi):
    v = ctx.param(name, default)
    if not isinstance(v, int) or isinstance(v, bool) or not lo <= v <= hi:
        raise JobError("%s must be an integer between %d and %d" % (name, lo, hi), kind="invalid_input")
    return v


def _pipelines(ctx, model_dir, vae_dir, lora, device, offload):
    import os
    import torch
    from diffusers import AutoencoderKL, StableDiffusionXLImg2ImgPipeline, StableDiffusionXLPipeline
    if not model_dir or not os.path.exists(os.path.join(model_dir, "model_index.json")):
        raise JobError("SDXL model not found in %s. Download it in AI Studio > Models." % model_dir,
                       kind="missing_model")
    key = (model_dir, vae_dir, lora.get("path") if lora else None, lora.get("scale") if lora else None, device, offload)
    with _cache_lock:
        if _cache["key"] == key:
            return _cache["pipe"], _cache["img2img"]
        _cache.update(key=None, pipe=None, img2img=None)
        if device == "cuda":
            torch.cuda.empty_cache()
    dtype = torch.float16 if device == "cuda" else torch.float32
    kwargs = {"torch_dtype": dtype, "use_safetensors": True}
    if os.path.exists(os.path.join(model_dir, "unet", "diffusion_pytorch_model.fp16.safetensors")) and dtype == torch.float16:
        kwargs["variant"] = "fp16"
    if vae_dir:
        kwargs["vae"] = AutoencoderKL.from_pretrained(vae_dir, torch_dtype=dtype, use_safetensors=True)
    ctx.progress(0.02, "Loading SDXL (first run takes a while)", force=True)
    pipe = StableDiffusionXLPipeline.from_pretrained(model_dir, **kwargs)
    pipe.set_progress_bar_config(disable=True)
    if lora:
        if not os.path.exists(lora.get("path", "")):
            raise JobError("LoRA file not found: %s" % lora.get("path"), kind="missing_model")
        folder, name = os.path.split(lora["path"])
        pipe.load_lora_weights(folder, weight_name=name, adapter_name="style")
        pipe.set_adapters(["style"], adapter_weights=[float(lora.get("scale", 1.0))])
    if device == "cuda" and offload == "model":
        pipe.enable_model_cpu_offload()
    else:
        pipe.to(device)
    # Decode large images in tiles to bound VRAM (API moved to the VAE in
    # newer Diffusers releases).
    if hasattr(pipe.vae, "enable_tiling"):
        pipe.vae.enable_tiling()
    elif hasattr(pipe, "enable_vae_tiling"):
        pipe.enable_vae_tiling()
    img2img = StableDiffusionXLImg2ImgPipeline(**pipe.components)
    img2img.set_progress_bar_config(disable=True)
    with _cache_lock:
        _cache.update(key=key, pipe=pipe, img2img=img2img)
    return pipe, img2img


def pixelate(img, width, height, colors=None, palette=None):
    """Nearest-neighbour downscale (centre sampling) and palette reduction
    without dithering, so the result is editable pixel art."""
    from PIL import Image
    small = img.convert("RGB").resize((width, height), Image.NEAREST, reducing_gap=None)
    if palette:
        pal_img = Image.new("P", (1, 1))
        flat = []
        for hexcol in palette[:256]:
            h = hexcol.lstrip("#")
            flat += [int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16)]
        flat += flat[:3] * (256 - len(flat) // 3)
        pal_img.putpalette(flat)
        small = small.quantize(palette=pal_img, dither=Image.Dither.NONE)
    elif colors:
        small = small.quantize(colors=int(colors), method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    return small.convert("RGBA")


def run(ctx):
    require("torch", "diffusers", "transformers", "numpy", "PIL")
    import torch
    prompt = ctx.param("prompt", required=True, kind=str).strip()
    if not prompt:
        raise JobError("Enter a prompt", kind="invalid_input")
    width = _int(ctx, "width", 1024, 64, 2048)
    height = _int(ctx, "height", 1024, 64, 2048)
    if width % 8 or height % 8:
        raise JobError("width and height must be multiples of 8", kind="invalid_input")
    steps = _int(ctx, "steps", 30, 1, 150)
    count = _int(ctx, "count", 1, 1, 8)
    guidance = float(ctx.param("guidance", 6.0))
    seed = ctx.param("seed", -1)
    if not isinstance(seed, int) or seed < 0:
        seed = random.randrange(0, 2**31 - 1)
    want = ctx.param("device", "auto")
    device = "cuda" if want in ("auto", "cuda") and torch.cuda.is_available() else "cpu"
    if want == "cuda" and device != "cuda":
        raise JobError("No CUDA GPU is available to PyTorch", kind="missing_dependency")
    offload = ctx.param("offload", "auto")
    if offload == "auto":
        offload = "none"
        if device == "cuda":
            vram = torch.cuda.get_device_properties(0).total_memory / 2**30
            offload = "model" if vram < 10 else "none"
    lora = ctx.param("lora")
    pipe, img2img = _pipelines(ctx, ctx.param("model_dir", required=True, kind=str), ctx.param("vae_dir"),
                               lora, device, offload)
    init_path = ctx.param("init_image")
    total_steps = steps * count
    done_steps = [0]

    def on_step(p, step, timestep, kwargs):
        done_steps[0] += 1
        if ctx.cancelled():
            p._interrupt = True
        ctx.progress(0.05 + 0.9 * done_steps[0] / total_steps, "Step %d of %d" % (done_steps[0], total_steps),
                     step=done_steps[0], steps=total_steps, force=True)
        return kwargs

    outputs = {}
    pixel = ctx.param("pixel")
    try:
        for i in range(count):
            ctx.check_cancel()
            gen = torch.Generator(device="cpu").manual_seed(seed + i)
            common = dict(prompt=prompt, negative_prompt=ctx.param("negative_prompt") or None,
                          num_inference_steps=steps, guidance_scale=guidance, generator=gen,
                          callback_on_step_end=on_step)
            if init_path:
                init = load_rgba(init_path).convert("RGB").resize((width, height))
                strength = float(ctx.param("strength", 0.6))
                if not 0.0 < strength <= 1.0:
                    raise JobError("strength must be in (0, 1]", kind="invalid_input")
                result = img2img(image=init, strength=strength, **common)
            else:
                result = pipe(width=width, height=height, **common)
            ctx.check_cancel()
            img = result.images[0]
            if pixel:
                outputs["raw%d" % i] = save_png(img, ctx.path("raw%d.png" % i))
                img = pixelate(img, int(pixel["width"]), int(pixel["height"]), pixel.get("colors"), pixel.get("palette"))
            outputs["image%d" % i] = save_png(img, ctx.path("image%d.png" % i))
    except Cancelled:
        raise
    except torch.cuda.OutOfMemoryError:
        torch.cuda.empty_cache()
        with _cache_lock:
            _cache.update(key=None, pipe=None, img2img=None)
        raise JobError("The GPU ran out of memory. Try a smaller size, fewer images, or Offload = model.",
                       kind="oom")
    return {"outputs": outputs,
            "info": {"seed": seed, "device": device, "offload": offload, "steps": steps, "width": width,
                     "height": height, "img2img": bool(init_path)}}
