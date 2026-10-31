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

from ..imaging import load_rgba, require, run_onnx, save_png
from ..jobs import JobError

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


def upscale_rgb(ctx, rgb, infer, scale, tile, base=0.1, span=0.85, label=""):
    """Runs `infer` (float32 1x3xhxw in [0,1] -> 1x3x(h*s)x(w*s)) over
    overlapping tiles and stitches the centres. rgb: HxWx3 float32."""
    import numpy as np
    h, w = rgb.shape[:2]
    out = np.zeros((h * scale, w * scale, 3), dtype=np.float32)
    tiles = list(_tiles(w, h, tile))
    for i, (x, y, tw, th) in enumerate(tiles):
        ctx.check_cancel()
        x0, y0 = max(0, x - OVERLAP), max(0, y - OVERLAP)
        x1, y1 = min(w, x + tw + OVERLAP), min(h, y + th + OVERLAP)
        patch = rgb[y0:y1, x0:x1].transpose(2, 0, 1)[None]
        res = infer(np.ascontiguousarray(patch))[0].transpose(1, 2, 0)
        oy, ox = (y - y0) * scale, (x - x0) * scale
        out[y * scale:(y + th) * scale, x * scale:(x + tw) * scale] = res[oy:oy + th * scale, ox:ox + tw * scale]
        ctx.progress(base + span * (i + 1) / len(tiles), "%sTile %d of %d" % (label, i + 1, len(tiles)),
                     step=i + 1, steps=len(tiles))
    return np.clip(out, 0.0, 1.0)


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

            def infer(x):
                y = sess.run(None, {inp.name: x.astype(np.float16) if half else x})[0]
                return np.asarray(y, dtype=np.float32)
            # A small probe run: a provider that cannot run the model fails
            # here, inside run_onnx, which then falls back.
            return infer, infer(np.zeros((1, 3, 16, 16), dtype=np.float32)).shape[-1] // 16
        (infer, scale), device = run_onnx(ctx, model, ctx.param("device", "auto"), make_infer)
    else:
        require("torch")
        import torch
        from ..rrdbnet import load
        dev = "cuda" if ctx.param("device", "auto") != "cpu" and torch.cuda.is_available() else "cpu"
        net, scale = load(model, dev)
        device = dev
        dtype = torch.float16 if dev == "cuda" else torch.float32
        net = net.to(dtype)

        def infer(x):
            with torch.inference_mode():
                t = torch.from_numpy(x).to(dev, dtype)
                return net(t).float().cpu().numpy()
    if scale not in (1, 2, 3, 4, 8):
        raise JobError("Model produced an unexpected scale (%s)" % scale)
    ctx.progress(0.1, "Upscaling ×%d on %s" % (scale, device), force=True)
    outputs = {}
    n = len(images)
    for k, img in enumerate(images):
        rgb = np.asarray(img.convert("RGB"), dtype=np.float32) / 255.0
        label = "Frame %d of %d · " % (k + 1, n) if n > 1 else ""
        up = upscale_rgb(ctx, rgb, infer, scale, tile, base=0.1 + 0.85 * k / n, span=0.85 / n, label=label)
        alpha = img.getchannel("A").resize((img.width * scale, img.height * scale), Image.LANCZOS)
        out = Image.fromarray((up * 255.0 + 0.5).astype(np.uint8), mode="RGB")
        out.putalpha(alpha)
        if frames is None:
            outputs["image"] = save_png(out, ctx.path("image.png"))
        else:
            outputs["image%d" % k] = save_png(out, ctx.path("image%d.png" % k))
    return {"outputs": outputs, "info": {"scale": scale, "device": str(device), "frames": n}}
