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
    if providers[0] == "CUDAExecutionProvider" and hasattr(ort, "preload_dlls"):
        # Load CUDA/cuDNN (from torch's lib folder first, per the ONNX Runtime
        # docs) before the session; failures surface as a session error and
        # run_onnx falls back.
        try:
            ort.preload_dlls()
        except Exception:   # noqa: BLE001 - the session creation below reports it
            pass
    sess = ort.InferenceSession(model_path, sess_options=opts, providers=providers)
    return sess, sess.get_providers()[0]


def _next_gpu_provider():
    """The second GPU provider in preference order, if one is installed."""
    import onnxruntime as ort
    gpus = [p for p in _GPU_PROVIDERS if p in ort.get_available_providers()]
    return gpus[1] if len(gpus) > 1 else None


def run_onnx(ctx, model_path, device, fn):
    """Runs fn(session, provider) on the best provider. A GPU provider that
    cannot create a session (e.g. CUDA DLLs missing) or fails while running
    (some DirectML/driver combinations reject fused nodes) is retried with
    fewer graph fusions, then the next GPU provider, then the CPU. What worked
    is remembered for the worker's lifetime. Returns (result, provider)."""
    from collections import deque
    queue = deque()
    if model_path in _working and device != "cpu":
        queue.append(_working[model_path])
    queue += [(None, "all"), ("gpu", "basic"), ("gpu", "none"), ("CPUExecutionProvider", "all")]
    tried = set()
    last_error = None
    first_gpu = None
    while queue:
        prov, opt = queue.popleft()
        if prov == "next-gpu":
            prov = _next_gpu_provider()
            if prov is None:
                continue
        if prov == "gpu":
            if first_gpu is None:
                continue   # no GPU session worked; the CPU attempt follows
            prov = first_gpu
        if (prov, opt) in tried:
            continue
        tried.add((prov, opt))
        try:
            sess, used = onnx_session(model_path, device, provider=prov, optimization=opt)
        except JobError:
            raise
        except Exception as e:   # onnxruntime raises its own exception types
            if prov == "CPUExecutionProvider" or device == "cpu":
                raise
            last_error = e
            ctx.log("Could not create a session on %s: %s" % (prov or "the preferred GPU provider",
                                                               str(e).splitlines()[0][:300]), level="warning")
            if prov is None:
                queue.appendleft(("next-gpu", "all"))
            continue
        if first_gpu is None and used != "CPUExecutionProvider":
            first_gpu = used
        try:
            result = fn(sess, used)
        except Exception as e:
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
