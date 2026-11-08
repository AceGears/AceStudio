"""Stand-ins for torch / diffusers / transformers, so the worker's model
lifecycle code (loading order, caching, OOM retry, idle unload) is tested
without the packages, a GPU or model files. Not a test module itself.

Usage:
    fakes = FakeML()
    with fakes.installed():
        ...   # `import torch` etc. now give the fakes; fakes.events records calls
"""

import contextlib
import json
import os
import sys
import tempfile
import types
from unittest import mock

GB = 2**30


class FakeOOM(RuntimeError):
    """torch.cuda.OutOfMemoryError stand-in (a RuntimeError, as in PyTorch)."""


class FakeML:
    def __init__(self, vram_gb=8, transformers_version="5.22.0"):
        self.events = []
        self.vram = vram_gb * GB
        self.free = 6 * GB
        self.oom_calls = 0          # the next N pipeline calls raise OOM
        self.te_load_error = None   # exception the standalone text-encoder load raises
        self.transformers_version = transformers_version
        self.torch = self._torch()
        self.diffusers, self.hooks = self._diffusers()
        self.transformers = self._transformers()

    # -- torch ----------------------------------------------------------------
    def _torch(self):
        fx = self
        t = types.ModuleType("torch")
        t.bfloat16, t.float16, t.float32, t.float8_e4m3fn = "bf16", "fp16", "fp32", "fp8"

        class Cuda:
            OutOfMemoryError = FakeOOM

            @staticmethod
            def is_available():
                return True

            @staticmethod
            def is_initialized():
                return True

            @staticmethod
            def empty_cache():
                fx.events.append(("empty_cache",))

            @staticmethod
            def ipc_collect():
                pass

            @staticmethod
            def mem_get_info():
                return (fx.free, fx.vram)

            @staticmethod
            def get_device_properties(i):
                return types.SimpleNamespace(total_memory=fx.vram, name="Fake GPU")

            @staticmethod
            def reset_peak_memory_stats():
                pass

            @staticmethod
            def max_memory_allocated():
                return 0

        class Generator:
            def __init__(self, device=None):
                pass

            def manual_seed(self, s):
                return self

        t.cuda = Cuda
        t.Generator = Generator
        t.set_num_threads = lambda n: None
        return t

    # -- diffusers ----------------------------------------------------------
    def _diffusers(self):
        fx = self

        class Module:
            def __init__(self, name):
                self.name = name

            def enable_layerwise_casting(self, **kw):
                fx.events.append(("cast", self.name, kw.get("storage_dtype")))

        class Vae(Module):
            def enable_tiling(self):
                fx.events.append(("vae_tiling",))

            def enable_slicing(self):
                fx.events.append(("vae_slicing",))

        class Pipe:
            def __init__(self, kwargs):
                self.kwargs = kwargs
                self.transformer = Module("transformer")
                self.text_encoder = kwargs.get("text_encoder") or Module("text_encoder")
                self.vae = Vae("vae")
                self.offload = None

            def set_progress_bar_config(self, **kw):
                pass

            def enable_model_cpu_offload(self):
                self.offload = "model"
                fx.events.append(("offload", "model"))

            def to(self, device):
                self.offload = "none"
                fx.events.append(("to", device))
                return self

            def maybe_free_model_hooks(self):
                fx.events.append(("free_hooks",))

            def __call__(self, **kw):
                cb = kw.get("callback_on_step_end")
                if fx.oom_calls > 0:
                    fx.oom_calls -= 1
                    fx.events.append(("oom",))
                    raise FakeOOM("CUDA out of memory. Tried to allocate 2.00 GiB")
                for step in range(kw.get("num_inference_steps", 1)):
                    if cb:
                        cb(self, step, 0, {})
                fx.events.append(("generate", kw.get("width"), kw.get("height")))
                return types.SimpleNamespace(images=["img"])

        class Flux2KleinPipeline:
            @classmethod
            def from_pretrained(cls, path, **kwargs):
                fx.events.append(("pipeline_load", {k: v for k, v in kwargs.items() if k != "text_encoder"},
                                  "text_encoder" in kwargs))
                return Pipe(kwargs)

        def apply_layerwise_casting(module, **kw):
            fx.events.append(("cast", getattr(module, "name", "?"), kw.get("storage_dtype")))

        d = types.ModuleType("diffusers")
        d.Flux2KleinPipeline = Flux2KleinPipeline
        d.Module = Module
        hooks = types.ModuleType("diffusers.hooks")
        hooks.apply_layerwise_casting = apply_layerwise_casting
        d.hooks = hooks
        return d, hooks

    # -- transformers -------------------------------------------------------
    def _transformers(self):
        fx = self
        tr = types.ModuleType("transformers")
        tr.__version__ = self.transformers_version

        class Qwen3ForCausalLM:
            @classmethod
            def from_pretrained(cls, path, **kwargs):
                fx.events.append(("te_load", os.path.basename(path), dict(kwargs)))
                if fx.te_load_error is not None:
                    raise fx.te_load_error
                return fx.diffusers.Module("text_encoder")

        tr.Qwen3ForCausalLM = Qwen3ForCausalLM
        return tr

    @contextlib.contextmanager
    def installed(self):
        with mock.patch.dict(sys.modules, {"torch": self.torch, "diffusers": self.diffusers,
                                           "diffusers.hooks": self.hooks, "transformers": self.transformers}):
            yield self

    def names(self):
        return [e[0] for e in self.events]


def klein_model_dir():
    d = tempfile.mkdtemp()
    with open(os.path.join(d, "model_index.json"), "w", encoding="utf-8") as f:
        json.dump({"_class_name": "Flux2KleinPipeline", "text_encoder": ["transformers", "Qwen3ForCausalLM"],
                   "transformer": ["diffusers", "Flux2Transformer2DModel"]}, f)
    return d


class FakeServer:
    def __init__(self):
        self.sent = []

    def notify(self, method, params):
        self.sent.append((method, params))

    def log(self, level, message):
        self.sent.append(("log", {"level": level, "message": message}))

    def send(self, obj):
        self.sent.append(("send", obj))
