"""Background removal with BiRefNet (MIT) through ONNX Runtime.

params:
  input       path to the source PNG
  model       path to a BiRefNet .onnx file (input 1x3xSxS, output 1x1xSxS logits)
  threshold   optional 0..1: hard alpha cut for pixel art (default: soft matte)
  device      auto | cpu
  cpu_memory_mb  optional: free RAM needed on the CPU path; the job fails early with a
              clear message instead of being killed when less is free
outputs:
  image       RGBA PNG with the matte applied to the original alpha
  mask        8-bit grayscale matte
"""

from ..imaging import load_rgba, require, run_onnx, save_png
from ..jobs import JobError

NAME = "background.remove"
MEAN = (0.485, 0.456, 0.406)
STD = (0.229, 0.224, 0.225)


def available():
    from . import has_module
    missing = [m for m in ("numpy", "PIL", "onnxruntime") if not has_module(m)]
    if missing:
        return False, "Needs %s (AI Studio > Set Up AI Runtime)" % ", ".join(missing)
    return True, ""


def matte(sess, rgb_img):
    """Runs the network; returns a float32 HxW matte in [0, 1] at the input size."""
    import numpy as np
    from PIL import Image
    inp = sess.get_inputs()[0]
    size = inp.shape[2] if isinstance(inp.shape[2], int) else 1024
    w, h = rgb_img.size
    x = np.asarray(rgb_img.convert("RGB").resize((size, size), Image.BICUBIC), dtype=np.float32) / 255.0
    x = (x - np.array(MEAN, dtype=np.float32)) / np.array(STD, dtype=np.float32)
    x = x.transpose(2, 0, 1)[None]
    if inp.type == "tensor(float16)":
        x = x.astype(np.float16)
    out = sess.run(None, {inp.name: x})[0]
    logits = np.asarray(out, dtype=np.float32).reshape(out.shape[-2], out.shape[-1])
    m = 1.0 / (1.0 + np.exp(-logits))
    mask = Image.fromarray(np.clip(m * 255.0 + 0.5, 0, 255).astype(np.uint8), mode="L").resize((w, h), Image.BILINEAR)
    return np.asarray(mask, dtype=np.float32) / 255.0


def run(ctx):
    require("numpy", "PIL", "onnxruntime")
    import numpy as np
    from PIL import Image
    img = load_rgba(ctx.param("input", required=True, kind=str))
    ctx.progress(0.05, "Loading BiRefNet", force=True)

    def segment(sess, provider):
        ctx.check_cancel()
        ctx.progress(0.3, "Segmenting on %s" % provider.replace("ExecutionProvider", ""), force=True)
        return matte(sess, img)

    cpu_mb = ctx.param("cpu_memory_mb")
    m, provider = run_onnx(ctx, ctx.param("model", required=True, kind=str), ctx.param("device", "auto"), segment,
                           cpu_memory_mb=cpu_mb if isinstance(cpu_mb, int) and cpu_mb > 0 else None)
    ctx.check_cancel()
    threshold = ctx.param("threshold")
    if threshold is not None:
        if not isinstance(threshold, (int, float)) or not 0 <= threshold <= 1:
            raise JobError("threshold must be between 0 and 1", kind="invalid_input")
        m = (m >= float(threshold)).astype(np.float32)
    rgba = np.array(img, dtype=np.float32)
    rgba[..., 3] = rgba[..., 3] * m
    out = Image.fromarray(np.clip(rgba + 0.5, 0, 255).astype(np.uint8), mode="RGBA")
    mask = Image.fromarray(np.clip(m * 255.0 + 0.5, 0, 255).astype(np.uint8), mode="L")
    ctx.progress(0.95, "Saving", force=True)
    return {"outputs": {"image": save_png(out, ctx.path("image.png")), "mask": save_png(mask, ctx.path("mask.png"))},
            "info": {"provider": provider}}
