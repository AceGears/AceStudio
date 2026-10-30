"""Image helpers shared by tasks. Imports numpy/Pillow lazily."""

import os

from .jobs import JobError


def require(*modules):
    """Raise a missing_dependency JobError naming what to install."""
    import importlib.util
    missing = [m for m in modules if importlib.util.find_spec(m) is None]
    if missing:
        raise JobError("Missing Python packages: %s. Use AI Studio > Set Up AI Runtime." % ", ".join(missing),
                       kind="missing_dependency")


def load_rgba(path):
    from PIL import Image
    if not path or not os.path.exists(path):
        raise JobError("Input image not found: %s" % path, kind="invalid_input")
    img = Image.open(path)
    img.load()
    return img.convert("RGBA")


def save_png(img, path):
    img.save(path, format="PNG", optimize=False)
    return path


def onnx_session(model_path, device="auto"):
    """Creates an ONNX Runtime session on the best available provider."""
    import onnxruntime as ort
    # No usage telemetry from the user's machine.
    if hasattr(ort, "disable_telemetry_events"):
        ort.disable_telemetry_events()
    if not model_path or not os.path.exists(model_path):
        raise JobError("Model file not found: %s. Download it in AI Studio > Models." % model_path,
                       kind="missing_model")
    available = ort.get_available_providers()
    if device == "cpu":
        order = ["CPUExecutionProvider"]
    else:
        order = ["CUDAExecutionProvider", "DmlExecutionProvider", "CoreMLExecutionProvider", "CPUExecutionProvider"]
    providers = [p for p in order if p in available] or ["CPUExecutionProvider"]
    opts = ort.SessionOptions()
    opts.log_severity_level = 3
    sess = ort.InferenceSession(model_path, sess_options=opts, providers=providers)
    return sess, sess.get_providers()[0]
