"""Post-processing shared by the generation tasks.

cutout   {"model": BiRefNet .onnx path, "threshold": 0..1 (default 0.5)}: removes the
         background of the full-size generation before it is reduced to pixel art, so
         the sprite comes out transparent with hard edges.
"""

from ..imaging import release_sessions, run_onnx
from ..jobs import JobError


def apply_cutout(ctx, img, cutout):
    """Returns `img` (PIL) as RGBA with the background made transparent."""
    import numpy as np
    from PIL import Image
    from .remove_background import matte
    if not isinstance(cutout, dict) or not isinstance(cutout.get("model"), str):
        raise JobError("cutout needs a background-removal model", kind="invalid_input")
    threshold = float(cutout.get("threshold", 0.5))
    if not 0.0 <= threshold <= 1.0:
        raise JobError("cutout threshold must be between 0 and 1", kind="invalid_input")
    m, _ = run_onnx(ctx, cutout["model"], cutout.get("device", "auto"), lambda sess, prov: matte(sess, img.convert("RGB")))
    m = (m >= threshold).astype(np.float32)
    rgba = np.array(img.convert("RGBA"), dtype=np.float32)
    rgba[..., 3] = rgba[..., 3] * m
    return Image.fromarray(np.clip(rgba + 0.5, 0, 255).astype(np.uint8), mode="RGBA")


def finish_images(ctx, images, pipe, torch, device, pixelate, save_png, crop):
    """Cutout, pixel reduction and saving for a batch of generated images.

    Runs after the whole batch is generated. On a GPU the diffusion models are
    moved off it first and the background-removal session is released at the
    end: measured on an 8 GB laptop GPU, keeping BiRefNet's CUDA session next to
    FLUX.2 klein pushed klein into shared system memory and the steps after the
    first image took about 90 s each (the first image, model load included,
    took under a minute).
    """
    outputs = {}
    cutout = ctx.param("cutout")
    pixel = ctx.param("pixel")
    if cutout and device == "cuda":
        free = getattr(pipe, "maybe_free_model_hooks", None)
        if free:
            free()
        torch.cuda.empty_cache()
    try:
        for i, img in enumerate(images):
            ctx.check_cancel()
            if cutout:
                img = apply_cutout(ctx, img, cutout)
            if pixel:
                outputs["raw%d" % i] = save_png(img, ctx.path("raw%d.png" % i))
                img = pixelate(img, int(pixel["width"]), int(pixel["height"]), pixel.get("colors"), pixel.get("palette"),
                               crop=crop, box=pixel.get("box"))
            outputs["image%d" % i] = save_png(img, ctx.path("image%d.png" % i))
    finally:
        if cutout and device == "cuda":
            release_sessions()
    return outputs
