"""AI upscaling with Real-ESRGAN (BSD-3-Clause).

Pixel art should be scaled with nearest neighbour instead (done in the
editor itself, no model needed); this task is for HD/painted assets.

params:
  input     path to the source PNG, or
  frames    n, with input0..input{n-1}: several images upscaled in one job (one model load)
  model     .pth (official Real-ESRGAN, needs torch) or .onnx (needs onnxruntime)
  tile      tile size in input pixels (default 192; lower uses less memory)
  device    auto | cpu
outputs:
  image     RGBA PNG at the model's scale; alpha is resized with Lanczos
  image0..  one per input when `frames` is given
"""

import os

from .. import residency
from ..imaging import load_rgba, require, run_onnx, save_png
from ..jobs import Cancelled, JobError

NAME = "upscale.esrgan"
OVERLAP = 16


def available():
    from . import has_module
    if not (has_module("numpy") and has_module("PIL")):
        return False, "Needs numpy and Pillow (AI Studio > Set Up AI Runtime)"
    if not (has_module("torch") or has_module("onnxruntime")):
        return False, "Needs PyTorch (for .pth models) or ONNX Runtime (for .onnx models)"
    return True, ""


def _tiles(w, h, tile):
    for y in range(0, h, tile):
        for x in range(0, w, tile):
            yield x, y, min(tile, w - x), min(tile, h - y)


def upscale_rgb(ctx, rgb, infer, scale, tile, base=0.1, span=0.85, label="", new_canvas=None):
    """Runs `infer` (an hxwx3 patch in [0,1] -> (h*s)x(w*s)x3) over
    overlapping tiles and stitches the centres into
    new_canvas((H*s, W*s, 3)) (default: a float32 numpy array). Only slicing
    is used, so rgb, patches and canvas may be numpy arrays or torch
    tensors: the PyTorch path keeps everything on the GPU and copies the
    result to the CPU once per image instead of once per tile. Returns the
    canvas, not clipped."""
    h, w = rgb.shape[:2]
    if new_canvas is None:
        import numpy as np

        def new_canvas(shape):
            return np.zeros(shape, dtype=np.float32)
    out = new_canvas((h * scale, w * scale, 3))
    tiles = list(_tiles(w, h, tile))
    for i, (x, y, tw, th) in enumerate(tiles):
        ctx.check_cancel()
        x0, y0 = max(0, x - OVERLAP), max(0, y - OVERLAP)
        x1, y1 = min(w, x + tw + OVERLAP), min(h, y + th + OVERLAP)
        res = infer(rgb[y0:y1, x0:x1])
        oy, ox = (y - y0) * scale, (x - x0) * scale
        out[y * scale:(y + th) * scale, x * scale:(x + tw) * scale] = res[oy:oy + th * scale, ox:ox + tw * scale]
        ctx.progress(base + span * (i + 1) / len(tiles), "%sTile %d of %d" % (label, i + 1, len(tiles)),
                     step=i + 1, steps=len(tiles))
    return out


# Real-ESRGAN (.pth) kept between jobs: otherwise every job reads the
# checkpoint, builds the network and moves it to the GPU again. Dropped by
# the idle policy (residency).
_esrgan = {"key": None, "net": None, "scale": None}


def unload():
    had = _esrgan["net"] is not None
    _esrgan.update(key=None, net=None, scale=None)
    return had


residency.register(NAME, unload)


def esrgan_model(path, dev, dtype, load):
    """The cached (net, scale) for this file, device and dtype; `load(path,
    dev)` builds it otherwise. A changed file (size or time) is reloaded."""
    st = os.stat(path)
    key = (os.path.abspath(path), st.st_size, st.st_mtime_ns, dev, str(dtype))
    if _esrgan["key"] != key:
        _esrgan.update(key=None, net=None, scale=None)
        net, scale = load(path, dev)
        _esrgan.update(key=key, net=net.to(dtype), scale=scale)
    return _esrgan["net"], _esrgan["scale"]


# Largest output kept on the GPU while stitching (fp16); bigger results are
# stitched in system memory with one copy per tile, as before.
GPU_CANVAS_MAX_BYTES = 1 * 2**30


def upscale_with_retry(ctx, run_image, state):
    """run_image(tile) with OOM recovery: on a CUDA out-of-memory error the
    caches are freed and the image is redone once with half the tile size
    (at least 32 px), which later images keep. state: {"tile": n, "retried": None}."""
    oom = False
    try:
        return run_image(state["tile"])
    except Cancelled:
        raise
    except Exception as e:
        if state["retried"] is not None or state["tile"] // 2 < 32 or not residency.is_oom(e):
            raise
        oom = True
    if oom:   # outside the except block: the failed tile's tensors are released first
        state["retried"] = state["tile"]
        state["tile"] = max(32, state["tile"] // 2)
        residency.free_for_retry()
        ctx.log("The GPU ran out of memory; retrying once with %d px tiles" % state["tile"], level="warning")
    return run_image(state["tile"])


def run(ctx):
    require("numpy", "PIL")
    import numpy as np
    from PIL import Image
    frames = ctx.param("frames")
    if frames is None:
        paths = [ctx.param("input", required=True, kind=str)]
    else:
        if not isinstance(frames, int) or not 1 <= frames <= 10000:
            raise JobError("frames must be an integer between 1 and 10000", kind="invalid_input")
        paths = [ctx.param("input%d" % i, required=True, kind=str) for i in range(frames)]
    images = [load_rgba(p) for p in paths]
    model = ctx.param("model", required=True, kind=str)
    tile = int(ctx.param("tile", 192))
    if tile < 32:
        raise JobError("tile must be at least 32", kind="invalid_input")
    if not os.path.exists(model):
        raise JobError("Model file not found: %s. Download it in AI Studio > Models." % model, kind="missing_model")
    ctx.progress(0.02, "Loading model", force=True)
    if model.lower().endswith(".onnx"):
        require("onnxruntime")

        def make_infer(sess, _provider):
            inp = sess.get_inputs()[0]
            half = inp.type == "tensor(float16)"

            def infer(patch):
                x = np.ascontiguousarray(patch.transpose(2, 0, 1)[None])
                y = sess.run(None, {inp.name: x.astype(np.float16) if half else x})[0]
                return np.asarray(y[0], dtype=np.float32).transpose(1, 2, 0)
            # A small probe run: a provider that cannot run the model fails
            # here, inside run_onnx, which then falls back.
            return infer, infer(np.zeros((16, 16, 3), dtype=np.float32)).shape[0] // 16
        (infer, scale), device = run_onnx(ctx, model, ctx.param("device", "auto"), make_infer)

        def upscale_image(rgb, tile, **kw):
            return np.clip(upscale_rgb(ctx, rgb, infer, scale, tile, **kw), 0.0, 1.0)
    else:
        require("torch")
        import torch
        from ..rrdbnet import load
        dev = "cuda" if ctx.param("device", "auto") != "cpu" and torch.cuda.is_available() else "cpu"
        device = dev
        dtype = torch.float16 if dev == "cuda" else torch.float32
        net, scale = esrgan_model(model, dev, dtype, load)

        def infer(patch):
            return net(patch.permute(2, 0, 1).unsqueeze(0))[0].permute(1, 2, 0)

        def upscale_image(rgb, tile, **kw):
            h, w = rgb.shape[:2]
            on_gpu = dev == "cuda" and h * w * scale * scale * 3 * 2 <= GPU_CANVAS_MAX_BYTES
            with torch.inference_mode():
                src = torch.from_numpy(rgb).to(dev, dtype)   # the whole image, uploaded once
                if on_gpu:
                    canvas = upscale_rgb(ctx, src, infer, scale, tile, **kw,
                                         new_canvas=lambda shape: torch.zeros(shape, device=dev, dtype=dtype))
                else:
                    canvas = upscale_rgb(ctx, src, lambda p: infer(p).float().cpu(), scale, tile, **kw,
                                         new_canvas=lambda shape: torch.zeros(shape, dtype=torch.float32))
                return canvas.clamp_(0.0, 1.0).float().cpu().numpy()   # one device-to-host copy per image
    if scale not in (1, 2, 3, 4, 8):
        raise JobError("Model produced an unexpected scale (%s)" % scale)
    ctx.progress(0.1, "Upscaling ×%d on %s" % (scale, device), force=True)
    outputs = {}
    n = len(images)
    state = {"tile": tile, "retried": None}
    for k, img in enumerate(images):
        rgb = np.asarray(img.convert("RGB"), dtype=np.float32) / 255.0
        label = "Frame %d of %d · " % (k + 1, n) if n > 1 else ""
        kw = dict(base=0.1 + 0.85 * k / n, span=0.85 / n, label=label)
        try:
            up = upscale_with_retry(ctx, lambda t: upscale_image(rgb, t, **kw), state)
        except Cancelled:
            raise
        except Exception as e:
            if not residency.is_oom(e):
                raise
            residency.free_for_retry()
            raise JobError("The GPU ran out of memory while upscaling, also with %d px tiles. Use a smaller "
                           "image, or the CPU (device: cpu)." % state["tile"], kind="oom")
        alpha = img.getchannel("A").resize((img.width * scale, img.height * scale), Image.LANCZOS)
        out = Image.fromarray((up * 255.0 + 0.5).astype(np.uint8), mode="RGB")
        out.putalpha(alpha)
        if frames is None:
            outputs["image"] = save_png(out, ctx.path("image.png"))
        else:
            outputs["image%d" % k] = save_png(out, ctx.path("image%d.png" % k))
    info = {"scale": scale, "device": str(device), "frames": n}
    if state["retried"] is not None:
        info["oomRetry"] = ["tile %d -> %d" % (state["retried"], state["tile"])]
    return {"outputs": outputs, "info": info}
