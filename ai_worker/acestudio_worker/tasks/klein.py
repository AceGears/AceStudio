"""Generation and reference-guided editing with FLUX.2 [klein] 4B (Diffusers).

params:
  model_dir         Diffusers-format FLUX.2 klein folder (model_index.json, transformer/, ...)
  prompt            text
  width, height     generation size, multiples of 16 (default 1024)
  steps             denoising steps (default 4: the 4B model is step-distilled)
  guidance          guidance scale (default 1.0 for the distilled model)
  seed              int; -1 or absent = random (the used seed is reported)
  count             images per job (default 1, max 8)
  references        optional list of up to 4 PNG paths: images the result should follow
                    (a character's look, an art style, a pose drawing, an object to keep)
  init_image        optional PNG path: edit this image (needs mask; inpainting)
  mask              optional PNG path: white = repaint, black = keep (with init_image)
  strength          inpaint strength 0..1 (default 0.85)
  lora              optional {"path": file.safetensors, "scale": 0..2} (e.g. the pixel-art LoRA);
                    kept as an adapter; a different file or scale reloads the model
  device            auto | cuda | cpu
  weights           auto | fp8 | bf16: auto stores the transformer and text encoder in FP8
                    (computed in BF16) on GPUs under 14 GB, so each fits next to the other's offload
  offload           auto | none | model: auto offloads under 20 GB (one model on the GPU at a time)
  reference0..3     the same as references, one path each (how the app passes images)
  cutout            optional {"model": BiRefNet .onnx, "threshold": 0.5}: transparent background,
                    removed at full size before the pixel post-process
  pixel             optional post-process, as in image.generate: {"width", "height", "colors" | "palette",
                    "crop", "box"}
outputs:
  image0..imageN    PNGs (post-processed when `pixel` is given)
  raw0..rawN        full-size generations when `pixel` is given
"""

import os
import random
import threading
import time

from ..imaging import load_rgba, require, save_png
from ..jobs import Cancelled, JobError
from .generate import pixelate

NAME = "image.klein"

_cache = {"key": None, "pipe": None, "inpaint": None}
_cache_lock = threading.Lock()


def available():
    from . import has_module
    missing = [m for m in ("torch", "diffusers", "transformers", "numpy", "PIL") if not has_module(m)]
    if missing:
        return False, "Needs %s (AI Studio > Set Up AI Runtime > Image generation)" % ", ".join(missing)
    try:
        import diffusers  # noqa: F401 - cheap once torch is importable
        if not hasattr(diffusers, "Flux2KleinPipeline"):
            return False, "The installed Diffusers has no FLUX.2 klein support; re-run AI setup"
    except Exception as e:   # noqa: BLE001
        return False, "Diffusers: %s" % e
    return True, ""


def _int(ctx, name, default, lo, hi):
    v = ctx.param(name, default)
    if not isinstance(v, int) or isinstance(v, bool) or not lo <= v <= hi:
        raise JobError("%s must be an integer between %d and %d" % (name, lo, hi), kind="invalid_input")
    return v


def _memory_plan(torch, device, weights, offload):
    """Measured choices for this model's two big parts (transformer 7.75 GB,
    Qwen3 text encoder 8.0 GB in BF16). With model offload only one is on the
    GPU at a time; FP8 storage halves each, so an 8 GB card holds either."""
    vram = 0.0
    if device == "cuda":
        vram = torch.cuda.get_device_properties(0).total_memory / 2**30
    if weights == "auto":
        weights = "fp8" if device == "cuda" and vram < 14 else "bf16"
    if offload == "auto":
        offload = "model" if device == "cuda" and vram < 20 else "none"
    return weights, offload, vram


def _pipelines(ctx, model_dir, lora, device, weights, offload):
    import torch
    from diffusers import Flux2KleinPipeline
    from diffusers.hooks import apply_layerwise_casting
    if not model_dir or not os.path.exists(os.path.join(model_dir, "model_index.json")):
        raise JobError("FLUX.2 klein not found in %s. Download it in AI Studio > Models." % model_dir, kind="missing_model")
    key = (model_dir, lora.get("path") if lora else None, round(float(lora.get("scale", 1.0)), 3) if lora else None,
           device, weights, offload)
    with _cache_lock:
        if _cache["key"] == key:
            return _cache["pipe"]
        _cache.update(key=None, pipe=None, inpaint=None)
        if device == "cuda":
            torch.cuda.empty_cache()
    dtype = torch.bfloat16 if device == "cuda" else torch.float32
    ctx.progress(0.02, "Loading FLUX.2 klein (the first run takes a few minutes)", force=True, uninterruptible=True)
    pipe = Flux2KleinPipeline.from_pretrained(model_dir, torch_dtype=dtype, use_safetensors=True)
    pipe.set_progress_bar_config(disable=True)
    if lora:
        path = lora.get("path", "")
        if not os.path.exists(path):
            raise JobError("LoRA file not found: %s" % path, kind="missing_model")
        scale = float(lora.get("scale", 1.0))
        if not 0.0 <= scale <= 2.0:
            raise JobError("LoRA scale must be between 0 and 2", kind="invalid_input")
        folder, name = os.path.split(path)
        pipe.load_lora_weights(folder, weight_name=name, adapter_name="style")
        # Kept as an adapter, as the LoRA's own instructions do. Fusing it into
        # the weights and unloading it (the first approach) gave pure noise on
        # the test PC, while the same settings without the LoRA gave clean
        # images.
        pipe.set_adapters(["style"], adapter_weights=[scale])
    if weights == "fp8" and device == "cuda":
        # The LoRA's own small matrices stay in BF16 (skip "lora_").
        skip = tuple(getattr(pipe.transformer, "_skip_layerwise_casting_patterns", None) or ()) + (
            "pos_embed", "patch_embed", "norm", "^proj_in$", "^proj_out$", "lora_")
        pipe.transformer.enable_layerwise_casting(storage_dtype=torch.float8_e4m3fn, compute_dtype=dtype,
                                                  skip_modules_pattern=skip)
        apply_layerwise_casting(pipe.text_encoder, storage_dtype=torch.float8_e4m3fn, compute_dtype=dtype,
                                skip_modules_pattern=("embed", "norm", "lm_head", "rotary"))
    if device == "cuda":
        if offload == "model":
            pipe.enable_model_cpu_offload()
        else:
            pipe.to("cuda")
    with _cache_lock:
        _cache.update(key=key, pipe=pipe, inpaint=None)
    return pipe


def _inpaint_pipeline(pipe):
    from diffusers import Flux2KleinInpaintPipeline
    with _cache_lock:
        if _cache["inpaint"] is None:
            _cache["inpaint"] = Flux2KleinInpaintPipeline.from_pipe(pipe)
            _cache["inpaint"].set_progress_bar_config(disable=True)
        return _cache["inpaint"]


def run(ctx):
    ctx.progress(0.01, "Starting PyTorch", force=True, uninterruptible=True)
    require("torch", "diffusers", "transformers", "numpy", "PIL")
    import torch
    prompt = ctx.param("prompt", required=True, kind=str).strip()
    if not prompt:
        raise JobError("Enter a prompt", kind="invalid_input")
    width = _int(ctx, "width", 1024, 64, 2048)
    height = _int(ctx, "height", 1024, 64, 2048)
    if width % 16 or height % 16:
        raise JobError("width and height must be multiples of 16", kind="invalid_input")
    steps = _int(ctx, "steps", 4, 1, 100)
    count = _int(ctx, "count", 1, 1, 8)
    guidance = float(ctx.param("guidance", 1.0))
    seed = ctx.param("seed", -1)
    if not isinstance(seed, int) or isinstance(seed, bool) or seed < 0:
        seed = random.randrange(0, 2**31 - 1)
    want = ctx.param("device", "auto")
    device = "cuda" if want in ("auto", "cuda") and torch.cuda.is_available() else "cpu"
    if want == "cuda" and device != "cuda":
        raise JobError("No CUDA GPU is available to PyTorch", kind="missing_dependency")
    weights = ctx.param("weights", "auto")
    if weights not in ("auto", "fp8", "bf16"):
        raise JobError("weights must be auto, fp8 or bf16", kind="invalid_input")
    offload = ctx.param("offload", "auto")
    if offload not in ("auto", "none", "model"):
        raise JobError("offload must be auto, none or model", kind="invalid_input")
    weights, offload, vram = _memory_plan(torch, device, weights, offload)
    refs = ctx.param("references") or []
    # The app passes images as reference0..reference3 (one file parameter each).
    refs = list(refs) + [ctx.param("reference%d" % k) for k in range(4) if ctx.param("reference%d" % k)]
    if not isinstance(refs, list) or len(refs) > 4 or not all(isinstance(r, str) for r in refs):
        raise JobError("references must be a list of up to 4 image paths", kind="invalid_input")
    ref_images = [load_rgba(r).convert("RGB") for r in refs]
    init_path, mask_path = ctx.param("init_image"), ctx.param("mask")
    if bool(init_path) != bool(mask_path):
        raise JobError("Region edits need both init_image and mask", kind="invalid_input")
    strength = float(ctx.param("strength", 0.85))
    if not 0.0 < strength <= 1.0:
        raise JobError("strength must be in (0, 1]", kind="invalid_input")
    lora = ctx.param("lora")

    t0 = time.monotonic()
    pipe = _pipelines(ctx, ctx.param("model_dir", required=True, kind=str), lora, device, weights, offload)
    load_s = time.monotonic() - t0
    ctx.progress(0.05, "Generating", force=True)
    ctx.check_cancel()
    runner = _inpaint_pipeline(pipe) if init_path else pipe

    total = steps * count
    done = [0]

    def on_step(p, step, timestep, kwargs):
        done[0] += 1
        if ctx.cancelled():
            p._interrupt = True
            ctx.check_cancel()
        ctx.progress(0.05 + 0.9 * done[0] / total, "Step %d of %d" % (done[0], total), step=done[0], steps=total, force=True)
        return kwargs

    generated = []
    pixel = ctx.param("pixel")
    step_times = []
    peak = 0.0
    if device == "cuda":
        torch.cuda.reset_peak_memory_stats()
    try:
        for i in range(count):
            ctx.check_cancel()
            gen = torch.Generator(device="cpu").manual_seed(seed + i)
            t1 = time.monotonic()
            common = dict(prompt=prompt, num_inference_steps=steps, guidance_scale=guidance, generator=gen,
                          width=width, height=height, callback_on_step_end=on_step)
            if init_path:
                init = load_rgba(init_path).convert("RGB").resize((width, height))
                mask = load_rgba(mask_path).convert("L").resize((width, height))
                if ref_images:
                    common["image_reference"] = ref_images
                result = runner(image=init, mask_image=mask, strength=strength, **common)
            else:
                if ref_images:
                    common["image"] = ref_images
                result = runner(**common)
            step_times.append((time.monotonic() - t1) / steps)
            ctx.check_cancel()
            generated.append(result.images[0])
        peak = torch.cuda.max_memory_allocated() / 2**30 if device == "cuda" else 0.0
        from .postprocess import finish_images
        outputs = finish_images(ctx, generated, pipe, torch, device, pixelate, save_png,
                                bool((pixel or {}).get("crop", False)))
    except Cancelled:
        raise
    except torch.cuda.OutOfMemoryError:
        torch.cuda.empty_cache()
        with _cache_lock:
            _cache.update(key=None, pipe=None, inpaint=None)
        raise JobError("The GPU ran out of memory. Try a smaller size or fewer reference images.", kind="oom")
    info = {"seed": seed, "device": device, "weights": weights, "offload": offload, "steps": steps,
            "width": width, "height": height, "references": len(ref_images), "inpaint": bool(init_path),
            "loadSeconds": round(load_s, 2), "secondsPerStep": round(sum(step_times) / max(1, len(step_times)), 3)}
    if device == "cuda":
        info["vramGb"] = round(vram, 1)
        info["peakVramGb"] = round(peak, 2)   # during generation (the cutout runs afterwards)
    return {"outputs": outputs, "info": info}
