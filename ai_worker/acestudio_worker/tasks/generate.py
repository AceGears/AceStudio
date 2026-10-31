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
  preview           optional {"dir": TAESD-XL folder, "every": n}: a small preview PNG every n steps
  controlnet        optional {"dir": SDXL ControlNet folder, "image": conditioning PNG (e.g. a pose),
                    "scale": 0..2 (default 0.8)} (text-to-image only)
  ip_adapter        optional {"dir": IP-Adapter folder, "weight": file under sdxl_models/,
                    "encoder": image encoder folder relative to "dir", "images": [PNG paths], "scale": 0..1}
  pixel             optional post-process {"width": w, "height": h, "colors": n | "palette": ["#RRGGBB", ...],
                    "crop": true to frame the subject before reducing (plain backdrops only),
                    "box": [x0, y0, x1, y1] fixed normalised crop instead (animation frames)}
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


def _lora_scale(lora):
    try:
        scale = float(lora.get("scale", 1.0))
    except (TypeError, ValueError):
        raise JobError("LoRA scale must be a number", kind="invalid_input")
    if not 0.0 <= scale <= 2.0:
        raise JobError("LoRA scale must be between 0 and 2", kind="invalid_input")
    return scale


def _ip_key(ip):
    return (ip.get("dir"), ip.get("weight"), ip.get("encoder")) if ip else None


def _pipelines(ctx, model_dir, vae_dir, lora, device, offload, controlnet=None, ip=None):
    import os
    import torch
    from diffusers import AutoencoderKL, StableDiffusionXLImg2ImgPipeline, StableDiffusionXLPipeline
    if not model_dir or not os.path.exists(os.path.join(model_dir, "model_index.json")):
        raise JobError("SDXL model not found in %s. Download it in AI Studio > Models." % model_dir,
                       kind="missing_model")
    # The LoRA strength is not part of the key: it is set per run on the
    # cached pipeline, so changing it does not reload SDXL.
    key = (model_dir, vae_dir, lora.get("path") if lora else None, device, offload,
           controlnet.get("dir") if controlnet else None, _ip_key(ip))
    with _cache_lock:
        if _cache["key"] == key:
            if lora:
                _cache["pipe"].set_adapters(["style"], adapter_weights=[_lora_scale(lora)])
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
        pipe.set_adapters(["style"], adapter_weights=[_lora_scale(lora)])
    if ip:
        # Reference-image conditioning (IP-Adapter, Apache-2.0). The image
        # encoder is loaded from the adapter folder's own layout.
        ip_dir, weight, encoder = ip.get("dir", ""), ip.get("weight", ""), ip.get("encoder", "")
        if not os.path.exists(os.path.join(ip_dir, "sdxl_models", weight)):
            raise JobError("IP-Adapter weights not found: %s" % os.path.join(ip_dir, "sdxl_models", weight),
                           kind="missing_model")
        if not os.path.exists(os.path.join(ip_dir, encoder, "config.json")):
            raise JobError("IP-Adapter image encoder not found in %s" % os.path.join(ip_dir, encoder), kind="missing_model")
        ctx.progress(0.03, "Loading the reference-image adapter", force=True)
        pipe.load_ip_adapter(ip_dir, subfolder="sdxl_models", weight_name=weight, image_encoder_folder=encoder)
    if controlnet:
        from diffusers import ControlNetModel, StableDiffusionXLControlNetPipeline
        cn_dir = controlnet.get("dir", "")
        if not os.path.exists(os.path.join(cn_dir, "config.json")):
            raise JobError("ControlNet not found in %s. Download it in AI Studio > Models." % cn_dir, kind="missing_model")
        ctx.progress(0.04, "Loading the pose ControlNet", force=True)
        cn = ControlNetModel.from_pretrained(cn_dir, torch_dtype=dtype, use_safetensors=True)
        pipe = StableDiffusionXLControlNetPipeline(**pipe.components, controlnet=cn)
        pipe.set_progress_bar_config(disable=True)
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
    img2img = None
    if not controlnet:
        img2img = StableDiffusionXLImg2ImgPipeline(**pipe.components)
        img2img.set_progress_bar_config(disable=True)
    with _cache_lock:
        _cache.update(key=key, pipe=pipe, img2img=img2img)
    return pipe, img2img


def subject_box(img, width, height, threshold=40, pad=0.08):
    """Box (left, top, right, bottom) around the subject of a generation, grown
    to the target aspect ratio, or None when the subject fills the image or
    cannot be told from the background. The background colour is the median
    of the border pixels (SDXL sprites are usually on a plain backdrop)."""
    import numpy as np
    a = np.asarray(img.convert("RGB"), dtype=np.int16)
    h, w = a.shape[:2]
    if w < 8 or h < 8:
        return None
    border = np.concatenate([a[0], a[-1], a[:, 0], a[:, -1]])
    bg = np.median(border, axis=0)
    # The backdrop must be fairly uniform, or "background" means nothing.
    if np.mean(np.max(np.abs(border - bg), axis=1) > threshold) > 0.2:
        return None
    fg = np.max(np.abs(a - bg), axis=2) > threshold
    if fg.mean() < 0.005:
        return None
    ys, xs = np.nonzero(fg)
    left, right, top, bottom = int(xs.min()), int(xs.max()) + 1, int(ys.min()), int(ys.max()) + 1
    bw, bh = right - left, bottom - top
    if bw * bh > 0.85 * w * h:
        return None
    # Pad, then grow the short side to the target aspect, centred on the subject.
    bw, bh = bw * (1 + 2 * pad), bh * (1 + 2 * pad)
    aspect = width / float(height)
    if bw / bh < aspect:
        bw = bh * aspect
    else:
        bh = bw / aspect
    bw, bh = min(bw, w), min(bh, h)
    # Never crop below the output size (no upscaling of the crop).
    bw, bh = max(bw, min(w, width)), max(bh, min(h, height))
    cx, cy = (left + right) / 2.0, (top + bottom) / 2.0
    x0 = int(round(min(max(cx - bw / 2, 0), w - bw)))
    y0 = int(round(min(max(cy - bh / 2, 0), h - bh)))
    x1, y1 = min(w, x0 + int(round(bw))), min(h, y0 + int(round(bh)))
    if (x1 - x0) * (y1 - y0) > 0.85 * w * h:
        return None
    return (x0, y0, x1, y1)


_preview_cache = {"dir": None, "device": None, "model": None}


def _preview_decoder(folder, device):
    """TAESD-XL (MIT) decodes latents in milliseconds, for live previews."""
    import os
    import torch
    from diffusers import AutoencoderTiny
    if not os.path.exists(os.path.join(folder, "config.json")):
        raise JobError("Live preview model not found in %s" % folder, kind="missing_model")
    if _preview_cache["dir"] != folder or _preview_cache["device"] != device:
        dtype = torch.float16 if device == "cuda" else torch.float32
        model = AutoencoderTiny.from_pretrained(folder, torch_dtype=dtype, use_safetensors=True).to(device)
        model.eval()
        _preview_cache.update(dir=folder, device=device, model=model)
    return _preview_cache["model"]


def preview_image(decoder, latents, max_side=256):
    """Latents (1x4xhxw, the pipeline's own scale) -> small RGB PIL image."""
    import torch
    from PIL import Image
    with torch.inference_mode():
        x = decoder.decode(latents.to(decoder.device, decoder.dtype)).sample[0]
    x = ((x.float().clamp(-1, 1) + 1) * 127.5).round().byte().permute(1, 2, 0).cpu().numpy()
    img = Image.fromarray(x, mode="RGB")
    scale = max_side / float(max(img.size))
    if scale < 1:
        img = img.resize((max(1, round(img.width * scale)), max(1, round(img.height * scale))), Image.BILINEAR)
    return img


def pixelate(img, width, height, colors=None, palette=None, crop=False, box=None):
    """Nearest-neighbour downscale (centre sampling) and palette reduction
    without dithering, so the result is editable pixel art."""
    from PIL import Image
    if box is not None:
        # A fixed crop (normalised x0, y0, x1, y1), the same for every frame of
        # an animation so scale and ground line stay put.
        x0, y0, x1, y1 = [float(v) for v in box]
        if not (0 <= x0 < x1 <= 1 and 0 <= y0 < y1 <= 1):
            raise JobError("pixel.box must be 0 <= x0 < x1 <= 1 and 0 <= y0 < y1 <= 1", kind="invalid_input")
        img = img.crop((round(x0 * img.width), round(y0 * img.height), round(x1 * img.width), round(y1 * img.height)))
    elif crop:
        sb = subject_box(img, width, height)
        if sb:
            img = img.crop(sb)
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
    if lora:
        _lora_scale(lora)   # reject a bad value before loading anything
    controlnet = ctx.param("controlnet")
    ip = ctx.param("ip_adapter")
    cn_image = ip_images = None
    if controlnet is not None:
        if not isinstance(controlnet, dict) or not isinstance(controlnet.get("image"), str):
            raise JobError("controlnet needs a conditioning image", kind="invalid_input")
        cn_scale = float(controlnet.get("scale", 0.8))
        if not 0.0 <= cn_scale <= 2.0:
            raise JobError("controlnet scale must be between 0 and 2", kind="invalid_input")
        if ctx.param("init_image"):
            raise JobError("Pose control works with text-to-image only", kind="invalid_input")
        cn_image = load_rgba(controlnet["image"]).convert("RGB").resize((width, height))
    if ip is not None:
        if not isinstance(ip, dict) or not ip.get("images"):
            raise JobError("ip_adapter needs at least one reference image", kind="invalid_input")
        ip_scale = float(ip.get("scale", 0.6))
        if not 0.0 <= ip_scale <= 1.0:
            raise JobError("ip_adapter scale must be between 0 and 1", kind="invalid_input")
        ip_images = [load_rgba(pth).convert("RGB") for pth in ip["images"][:4]]
    pipe, img2img = _pipelines(ctx, ctx.param("model_dir", required=True, kind=str), ctx.param("vae_dir"),
                               lora, device, offload, controlnet, ip)
    if ip is not None:
        pipe.set_ip_adapter_scale(ip_scale)
    init_path = ctx.param("init_image")
    total_steps = steps * count
    done_steps = [0]
    preview = ctx.param("preview")
    decoder, every = None, 0
    if preview:
        try:
            every = max(1, int(preview.get("every", 5)))
            decoder = _preview_decoder(preview["dir"], "cuda" if device == "cuda" else "cpu")
        except JobError as e:
            ctx.log("Live preview off: %s" % e, "warning")   # never fails the generation
        except Exception as e:   # noqa: BLE001 - an optional feature must not stop the job
            ctx.log("Live preview off: %s" % e, "warning")
            decoder = None

    def on_step(p, step, timestep, kwargs):
        done_steps[0] += 1
        if ctx.cancelled():
            p._interrupt = True
        path = None
        if decoder is not None and "latents" in kwargs and (step + 1) % every == 0:
            try:
                path = save_png(preview_image(decoder, kwargs["latents"]), ctx.path("preview_%d.png" % done_steps[0]))
            except Exception as e:   # noqa: BLE001
                ctx.log("Preview failed: %s" % e, "warning")
        ctx.progress(0.05 + 0.9 * done_steps[0] / total_steps, "Step %d of %d" % (done_steps[0], total_steps),
                     step=done_steps[0], steps=total_steps, force=True, preview=path)
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
            if ip_images:
                # One adapter; several references are combined.
                common["ip_adapter_image"] = [ip_images] if len(ip_images) > 1 else ip_images
            if cn_image is not None:
                common.update(image=cn_image, controlnet_conditioning_scale=cn_scale)
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
                img = pixelate(img, int(pixel["width"]), int(pixel["height"]), pixel.get("colors"), pixel.get("palette"),
                               crop=bool(pixel.get("crop", False)), box=pixel.get("box"))
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
                     "height": height, "img2img": bool(init_path), "controlnet": cn_image is not None,
                     "ip_adapter": bool(ip_images)}}
