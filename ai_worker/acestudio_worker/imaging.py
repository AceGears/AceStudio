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


_GPU_PROVIDERS = ("CUDAExecutionProvider", "DmlExecutionProvider", "CoreMLExecutionProvider")
# model path -> (provider, optimization) that last worked, so a model that
# fails on the GPU does not fail again on every job in this worker.
_working = {}


def onnx_session(model_path, device="auto", provider=None, optimization="all"):
    """Creates an ONNX Runtime session on the best available provider (or
    the given one). optimization: all | basic | none (graph fusions)."""
    import onnxruntime as ort
    # No usage telemetry from the user's machine.
    if hasattr(ort, "disable_telemetry_events"):
        ort.disable_telemetry_events()
    if not model_path or not os.path.exists(model_path):
        raise JobError("Model file not found: %s. Download it in AI Studio > Models." % model_path,
                       kind="missing_model")
    available = ort.get_available_providers()
    if provider is not None:
        order = [provider]
    elif device == "cpu":
        order = ["CPUExecutionProvider"]
    else:
        order = list(_GPU_PROVIDERS) + ["CPUExecutionProvider"]
    providers = [p for p in order if p in available] or ["CPUExecutionProvider"]
    if providers[0] != "CPUExecutionProvider" and "CPUExecutionProvider" not in providers:
        providers.append("CPUExecutionProvider")   # for operators the GPU provider lacks
    opts = ort.SessionOptions()
    opts.log_severity_level = 3
    opts.graph_optimization_level = {
        "all": ort.GraphOptimizationLevel.ORT_ENABLE_ALL,
        "basic": ort.GraphOptimizationLevel.ORT_ENABLE_BASIC,
        "none": ort.GraphOptimizationLevel.ORT_DISABLE_ALL,
    }[optimization]
    if providers[0] == "DmlExecutionProvider":
        # Required by the DirectML provider (see its documentation): no
        # memory-pattern optimisation and sequential execution.
        opts.enable_mem_pattern = False
        opts.execution_mode = ort.ExecutionMode.ORT_SEQUENTIAL
    sess = ort.InferenceSession(model_path, sess_options=opts, providers=providers)
    return sess, sess.get_providers()[0]


def run_onnx(ctx, model_path, device, fn):
    """Runs fn(session) on the best provider. If the GPU provider fails while
    running (some DirectML/driver combinations reject fused nodes), retries
    with fewer graph fusions, then on the CPU. Returns (result, provider)."""
    attempts = []
    if model_path in _working and device != "cpu":
        attempts.append(_working[model_path])
    attempts += [(None, "all"), ("gpu", "basic"), ("gpu", "none"), ("CPUExecutionProvider", "all")]
    tried = set()
    last_error = None
    first_gpu = None
    for prov, opt in attempts:
        if prov == "gpu":
            if first_gpu is None:
                continue   # the default attempt already ran on the CPU
            prov = first_gpu
        key = (prov, opt)
        if key in tried:
            continue
        tried.add(key)
        sess, used = onnx_session(model_path, device, provider=prov, optimization=opt)
        if first_gpu is None and used != "CPUExecutionProvider" and prov is None:
            first_gpu = used
        try:
            result = fn(sess, used)
        except Exception as e:   # onnxruntime raises its own exception types
            if used == "CPUExecutionProvider":
                raise
            last_error = e
            ctx.log("%s failed with graph optimisation '%s': %s" % (used, opt, str(e).splitlines()[0][:300]),
                    level="warning")
            continue
        _working[model_path] = (used, opt)
        if last_error is not None:
            ctx.log("Using %s (graph optimisation '%s') for %s" % (used, opt, os.path.basename(model_path)),
                    level="warning")
        return result, used
    raise last_error
