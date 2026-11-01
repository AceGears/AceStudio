"""Post-processing shared by the generation tasks.

cutout   {"model": BiRefNet .onnx path, "threshold": 0..1 (default 0.5)}: removes the
         background of the full-size generation before it is reduced to pixel art, so
         the sprite comes out transparent with hard edges.
"""

from ..imaging import run_onnx
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
