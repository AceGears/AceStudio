"""End-to-end tests of the worker protocol (stdlib only).

Run from the ai_worker folder:  python -m unittest discover -s tests
"""

import json
import os
import subprocess
import sys
import tempfile
import threading
import time
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class Worker:
    def __init__(self):
        env = dict(os.environ, ACESTUDIO_WORKER_DIAG="1", PYTHONPATH=ROOT)
        self.proc = subprocess.Popen([sys.executable, "-m", "acestudio_worker"], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env, text=True,
                                     encoding="utf-8", bufsize=1)
        self.lines = []
        self.cv = threading.Condition()
        self.stderr = []
        threading.Thread(target=self._read, daemon=True).start()
        threading.Thread(target=lambda: self.stderr.extend(self.proc.stderr), daemon=True).start()
        self.next_id = 1

    def _read(self):
        for line in self.proc.stdout:
            with self.cv:
                self.lines.append(json.loads(line))   # every stdout line must be valid JSON
                self.cv.notify_all()

    def send(self, method, params=None, raw=None):
        if raw is not None:
            self.proc.stdin.write(raw + "\n")
        else:
            msg = {"jsonrpc": "2.0", "id": self.next_id, "method": method, "params": params or {}}
            self.next_id += 1
            self.proc.stdin.write(json.dumps(msg) + "\n")
        self.proc.stdin.flush()
        return self.next_id - 1

    def wait_for(self, pred, timeout=20):
        end = time.time() + timeout
        with self.cv:
            while True:
                for m in self.lines:
                    if pred(m):
                        return m
                left = end - time.time()
                if left <= 0:
                    raise AssertionError("timed out; got %r" % self.lines[-5:])
                self.cv.wait(left)

    def response(self, req_id):
        return self.wait_for(lambda m: m.get("id") == req_id and ("result" in m or "error" in m))

    def close(self):
        if self.proc.poll() is None:
            self.proc.stdin.close()
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        for f in (self.proc.stdin, self.proc.stdout, self.proc.stderr):
            try:
                f.close()
            except Exception:
                pass


class ProtocolTests(unittest.TestCase):
    def setUp(self):
        self.w = Worker()
        self.tmp = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.w.close()
        self.tmp.cleanup()

    def submit(self, job, task, params=None):
        rid = self.w.send("job.submit", {"job": job, "task": task, "params": params or {},
                                         "dir": os.path.join(self.tmp.name, job)})
        return self.w.response(rid)

    def test_hello_reports_tasks_and_capabilities(self):
        r = self.w.response(self.w.send("hello", {"protocol": 1}))["result"]
        self.assertEqual(r["protocol"], 1)
        self.assertIn("diag.sleep", r["tasks"])
        self.assertIn("image.generate", r["tasks"])
        self.assertIn("torch", r["capabilities"])
        self.assertFalse(r["capabilities"]["probed"])   # hello never waits for torch
        self.assertEqual(r["pid"], self.w.proc.pid)
        caps = self.w.wait_for(lambda m: m.get("method") == "capabilities", 120)["params"]
        self.assertTrue(caps["probed"])
        self.assertIn("devices", caps)

    def test_job_progress_and_done(self):
        self.assertTrue(self.submit("j1", "diag.sleep", {"seconds": 0.3, "steps": 3})["result"]["accepted"])
        done = self.w.wait_for(lambda m: m.get("method") == "job.done")
        progress = [m for m in self.w.lines if m.get("method") == "job.progress"]
        self.assertEqual([p["params"]["step"] for p in progress], [1, 2, 3])
        out = done["params"]["outputs"]["result"]
        with open(out, encoding="utf-8") as f:
            self.assertIn("slept", f.read())

    def test_next_job_is_accepted_right_after_done(self):
        for i in range(20):   # regression: "done" used to arrive before the worker was free
            self.submit("q%d" % i, "diag.echo", {"i": i})
            self.w.wait_for(lambda m, i=i: m.get("method") == "job.done" and m["params"]["job"] == "q%d" % i)
        self.assertFalse(any("error" in m for m in self.w.lines if m.get("id")))

    def test_busy_worker_rejects_a_second_job(self):
        self.submit("a", "diag.sleep", {"seconds": 2, "steps": 20})
        r = self.submit("b", "diag.echo")
        self.assertEqual(r["error"]["code"], -32001)

    def test_cancel(self):
        self.submit("c", "diag.sleep", {"seconds": 10, "steps": 100})
        self.w.wait_for(lambda m: m.get("method") == "job.progress")
        t0 = time.time()
        r = self.w.response(self.w.send("job.cancel", {"job": "c"}))
        self.assertTrue(r["result"]["cancelling"])
        self.w.wait_for(lambda m: m.get("method") == "job.cancelled")
        self.assertLess(time.time() - t0, 2)
        # The worker is free again afterwards.
        self.assertTrue(self.submit("d", "diag.echo", {"x": 1})["result"]["accepted"])
        done = self.w.wait_for(lambda m: m.get("method") == "job.done")
        self.assertEqual(done["params"]["info"]["params"], {"x": 1})

    def test_failures_are_reported_not_fatal(self):
        self.submit("f", "diag.fail", {"message": "boom", "kind": "oom"})
        failed = self.w.wait_for(lambda m: m.get("method") == "job.failed")
        self.assertEqual(failed["params"]["error"], "boom")
        self.assertEqual(failed["params"]["kind"], "oom")
        self.assertEqual(self.submit("u", "no.such.task")["error"]["code"], -32002)
        self.w.send(None, raw="{not json")
        self.w.wait_for(lambda m: m.get("error", {}).get("code") == -32700)
        r = self.w.response(self.w.send("frobnicate"))
        self.assertEqual(r["error"]["code"], -32601)
        self.assertEqual(self.w.response(self.w.send("hello"))["result"]["protocol"], 1)

    def test_stray_prints_do_not_corrupt_the_protocol(self):
        code = "print('noise on stdout'); import os; os.write(1, b'raw fd write\\n')"
        # diag.echo cannot print, so exercise the redirection directly in-process:
        env = dict(os.environ, PYTHONPATH=ROOT)
        script = ("import sys, json\nfrom acestudio_worker import server\n"
                  "import builtins\norig = server.Server.on_hello\n"
                  "def noisy(self, p):\n    %s\n    return orig(self, p)\n"
                  "server.Server.on_hello = noisy\nsys.exit(server.main())\n") % code
        p = subprocess.run([sys.executable, "-c", script], input='{"jsonrpc":"2.0","id":1,"method":"hello"}\n',
                           capture_output=True, text=True, env=env, timeout=60)
        for line in p.stdout.splitlines():
            json.loads(line)
        self.assertIn("noise on stdout", p.stderr)
        self.assertIn("raw fd write", p.stderr)

    def test_crash_exits_with_the_code(self):
        self.submit("x", "diag.crash", {"after_steps": 1, "code": 17})
        self.assertEqual(self.w.proc.wait(20), 17)

    def test_stdin_eof_ends_the_worker(self):
        self.w.proc.stdin.close()
        self.assertEqual(self.w.proc.wait(20), 0)


class DownloadTests(unittest.TestCase):
    """model.download against a local HTTP server with Range support."""

    @classmethod
    def setUpClass(cls):
        import http.server
        import socketserver
        cls.root = tempfile.TemporaryDirectory()
        cls.payload = os.urandom(3 * 1024 * 1024 + 123)
        with open(os.path.join(cls.root.name, "model.bin"), "wb") as f:
            f.write(cls.payload)
        root = cls.root.name

        class Handler(http.server.SimpleHTTPRequestHandler):
            def __init__(self, *a, **k):
                super().__init__(*a, directory=root, **k)

            def log_message(self, *a):
                pass

            def send_head(self):   # minimal Range support for resume tests
                rng = self.headers.get("Range")
                path = self.translate_path(self.path)
                if not rng or not os.path.isfile(path):
                    return super().send_head()
                start = int(rng.split("=")[1].split("-")[0])
                size = os.path.getsize(path)
                f = open(path, "rb")
                f.seek(start)
                self.send_response(206)
                self.send_header("Content-Length", str(size - start))
                self.send_header("Content-Range", "bytes %d-%d/%d" % (start, size - 1, size))
                self.end_headers()
                return f

        cls.httpd = socketserver.ThreadingTCPServer(("127.0.0.1", 0), Handler)
        cls.port = cls.httpd.server_address[1]
        threading.Thread(target=cls.httpd.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.httpd.shutdown()
        cls.httpd.server_close()
        cls.root.cleanup()

    def setUp(self):
        self.w = Worker()
        self.tmp = tempfile.TemporaryDirectory()
        import hashlib
        self.sha = hashlib.sha256(self.payload).hexdigest()

    def tearDown(self):
        self.w.close()
        self.tmp.cleanup()

    def download(self, job, sha=None, accepted="CreativeML-OpenRAIL++-M"):
        dest = os.path.join(self.tmp.name, "models")
        self.w.send("job.submit", {"job": job, "task": "model.download", "dir": os.path.join(self.tmp.name, job),
                                   "params": {"dest_dir": dest, "license_accepted": accepted,
                                              "files": [{"url": "http://127.0.0.1:%d/model.bin" % self.port,
                                                         "path": "sub/model.bin", "size": len(self.payload),
                                                         "sha256": sha if sha is not None else self.sha}]}})
        return dest, self.w.wait_for(lambda m: m.get("method", "").startswith("job.") and
                                     m.get("method") != "job.progress" and m["params"].get("job") == job)

    def test_download_verifies_and_installs(self):
        dest, msg = self.download("d1")
        self.assertEqual(msg["method"], "job.done", msg)
        with open(os.path.join(dest, "sub", "model.bin"), "rb") as f:
            self.assertEqual(f.read(), self.payload)
        self.assertEqual(msg["params"]["info"]["sha256"]["sub/model.bin"], self.sha)
        self.assertFalse(os.path.exists(os.path.join(dest, "sub", "model.bin.part")))

    def test_resume_from_partial_file(self):
        dest = os.path.join(self.tmp.name, "models", "sub")
        os.makedirs(dest)
        with open(os.path.join(dest, "model.bin.part"), "wb") as f:
            f.write(self.payload[:1000000])
        _, msg = self.download("d2")
        self.assertEqual(msg["method"], "job.done", msg)
        with open(os.path.join(dest, "model.bin"), "rb") as f:
            self.assertEqual(f.read(), self.payload)

    def test_hash_mismatch_is_rejected_and_cleaned_up(self):
        dest, msg = self.download("d3", sha="0" * 64)
        self.assertEqual(msg["method"], "job.failed")
        self.assertIn("verification", msg["params"]["error"])
        self.assertFalse(os.path.exists(os.path.join(dest, "sub", "model.bin")))
        self.assertFalse(os.path.exists(os.path.join(dest, "sub", "model.bin.part")))

    def test_refused_without_licence_acceptance(self):
        _, msg = self.download("d4", accepted="")
        self.assertEqual(msg["method"], "job.failed")
        self.assertIn("licence", msg["params"]["error"])

    def test_unsafe_paths_are_refused(self):
        self.w.send("job.submit", {"job": "d5", "task": "model.download", "dir": self.tmp.name,
                                   "params": {"dest_dir": self.tmp.name, "license_accepted": "x",
                                              "files": [{"url": "http://127.0.0.1:1/x", "path": "../evil"}]}})
        msg = self.w.wait_for(lambda m: m.get("method") == "job.failed")
        self.assertIn("Unsafe", msg["params"]["error"])



def _has(*mods):
    import importlib.util
    return all(importlib.util.find_spec(m) is not None for m in mods)


@unittest.skipUnless(_has("PIL", "numpy"), "needs Pillow and NumPy")
class SubjectCropTests(unittest.TestCase):
    def setUp(self):
        sys.path.insert(0, ROOT)
        from acestudio_worker.tasks import generate
        self.g = generate

    def test_small_subject_is_framed(self):
        from PIL import Image
        img = Image.new("RGB", (1024, 1024), (240, 240, 235))
        img.paste((200, 30, 30), (400, 380, 600, 640))        # 200x260 subject
        box = self.g.subject_box(img, 128, 128)
        self.assertIsNotNone(box)
        x0, y0, x1, y1 = box
        self.assertEqual(x1 - x0, y1 - y0)                     # square, like the canvas
        self.assertTrue(x0 <= 400 and y0 <= 380 and x1 >= 600 and y1 >= 640)
        self.assertLess(x1 - x0, 400)                          # much tighter than 1024
        out = self.g.pixelate(img, 128, 128, crop=True)
        import numpy as np
        def reds(im):
            a = np.asarray(im.convert("RGB"), dtype=np.int16)
            return int(((a[..., 0] > 150) & (a[..., 1] < 100)).sum())
        red = reds(out)
        plain = self.g.pixelate(img, 128, 128)
        red_plain = reds(plain)
        self.assertGreater(red, 3 * red_plain)                 # subject fills more of the sprite

    def test_busy_or_full_images_are_left_alone(self):
        from PIL import Image
        import random
        rnd = random.Random(1)
        noisy = Image.new("RGB", (256, 256))
        noisy.putdata([(rnd.randrange(256), rnd.randrange(256), rnd.randrange(256)) for _ in range(256 * 256)])
        self.assertIsNone(self.g.subject_box(noisy, 64, 64))
        full = Image.new("RGB", (256, 256), (10, 10, 10))
        full.paste((250, 250, 0), (4, 4, 252, 252))
        self.assertIsNone(self.g.subject_box(full, 64, 64))
        empty = Image.new("RGB", (256, 256), (90, 90, 90))
        self.assertIsNone(self.g.subject_box(empty, 64, 64))

    def test_fixed_box_crop(self):
        from PIL import Image
        from acestudio_worker.jobs import JobError
        img = Image.new("RGB", (100, 100), (0, 0, 0))
        img.paste((255, 0, 0), (50, 50, 100, 100))
        out = self.g.pixelate(img, 10, 10, box=[0.5, 0.5, 1.0, 1.0])
        self.assertEqual(out.getpixel((0, 0))[:3], (255, 0, 0))   # only the red quarter remains
        with self.assertRaises(JobError):
            self.g.pixelate(img, 10, 10, box=[0.6, 0.5, 0.4, 1.0])

    def test_wide_canvas_gets_wide_box_inside_image(self):
        from PIL import Image
        img = Image.new("RGB", (1024, 1024), (0, 0, 0))
        img.paste((0, 200, 0), (900, 100, 1000, 300))          # near the right edge
        x0, y0, x1, y1 = self.g.subject_box(img, 128, 64)
        self.assertTrue(0 <= x0 < x1 <= 1024 and 0 <= y0 < y1 <= 1024)
        self.assertAlmostEqual((x1 - x0) / float(y1 - y0), 2.0, delta=0.05)
        self.assertTrue(x0 <= 900 and x1 >= 1000)


class LoraScaleTests(unittest.TestCase):
    def test_range_and_type(self):
        sys.path.insert(0, ROOT)
        from acestudio_worker.tasks import generate
        from acestudio_worker.jobs import JobError
        self.assertEqual(generate._lora_scale({"scale": 0.6}), 0.6)
        self.assertEqual(generate._lora_scale({}), 1.0)
        for bad in (-0.1, 2.5, "x", None):
            with self.assertRaises(JobError):
                generate._lora_scale({"scale": bad})


class RuntimeInstallTests(unittest.TestCase):
    """pip must not inherit the protocol stdin: on Windows a child sharing the
    pipe the worker's main thread is reading stalls at startup."""

    def test_pip_gets_no_stdin(self):
        sys.path.insert(0, ROOT)
        from acestudio_worker.tasks import runtime_install as ri

        seen = {}

        class FakeProc:
            def __init__(self, cmd, **kw):
                seen.update(kw)
                seen["cmd"] = cmd
                self.stdout = iter(["Collecting numpy==1.0\n", "Successfully installed numpy-1.0\n"])

            def poll(self):
                return 0

            def wait(self):
                return 0

        class Ctx:
            logs = []

            def log(self, m, level="info"):
                self.logs.append(m)

            def progress(self, *a, **k):
                pass

            def cancelled(self):
                return False

        real = ri.subprocess.Popen
        ri.subprocess.Popen = FakeProc
        try:
            ctx = Ctx()
            ri._pip(ctx, ["-r", "x.txt"], "x.txt")
        finally:
            ri.subprocess.Popen = real
        self.assertIs(seen.get("stdin"), subprocess.DEVNULL)
        self.assertIn("--only-binary", seen["cmd"])
        self.assertTrue(any(m.startswith("pip: Collecting") for m in ctx.logs))


class OnnxFallbackTests(unittest.TestCase):
    """A GPU provider that fails while running falls back: fewer graph
    fusions first, then the CPU; the working choice is remembered."""

    def test_falls_back_and_remembers(self):
        sys.path.insert(0, ROOT)
        from acestudio_worker import imaging

        calls = []

        def fake_session(path, device="auto", provider=None, optimization="all"):
            used = provider or "DmlExecutionProvider"
            calls.append((used, optimization))
            return object(), used

        def fn(sess, used):
            if used == "DmlExecutionProvider":
                raise RuntimeError("RUNTIME_EXCEPTION in DmlFusedNode")
            return "ok"

        class Ctx:
            def __init__(self):
                self.logs = []

            def log(self, m, level="info"):
                self.logs.append((level, m))

        real = imaging.onnx_session
        imaging.onnx_session = fake_session
        imaging._working.clear(); imaging._sessions.clear()
        try:
            ctx = Ctx()
            result, used = imaging.run_onnx(ctx, "m.onnx", "auto", fn)
            self.assertEqual((result, used), ("ok", "CPUExecutionProvider"))
            self.assertEqual(calls, [("DmlExecutionProvider", "all"), ("DmlExecutionProvider", "basic"),
                                     ("DmlExecutionProvider", "none"), ("CPUExecutionProvider", "all")])
            self.assertTrue(any(level == "warning" for level, _ in ctx.logs))
            calls.clear()
            self.assertEqual(imaging.run_onnx(ctx, "m.onnx", "auto", fn), ("ok", "CPUExecutionProvider"))
            self.assertEqual(calls, [])   # remembered, and the CPU session is reused
            self.assertEqual(list(imaging._sessions), [("m.onnx", "CPUExecutionProvider", "all")])   # failing GPU sessions evicted
        finally:
            imaging.onnx_session = real
            imaging._working.clear(); imaging._sessions.clear()

    def test_cpu_errors_are_not_swallowed(self):
        sys.path.insert(0, ROOT)
        from acestudio_worker import imaging

        real = imaging.onnx_session
        imaging.onnx_session = lambda *a, **k: (object(), "CPUExecutionProvider")
        imaging._working.clear(); imaging._sessions.clear()
        try:
            with self.assertRaises(ValueError):
                imaging.run_onnx(None, "m.onnx", "cpu", lambda s, u: (_ for _ in ()).throw(ValueError("bad")))
        finally:
            imaging.onnx_session = real


class OnnxSessionFallbackTests(unittest.TestCase):
    def test_session_creation_failure_falls_back_to_next_provider(self):
        sys.path.insert(0, ROOT)
        from acestudio_worker import imaging

        calls = []

        def fake_session(path, device="auto", provider=None, optimization="all"):
            calls.append((provider, optimization))
            if provider in (None, "CUDAExecutionProvider"):
                raise RuntimeError("LoadLibrary failed with error 126 for onnxruntime_providers_cuda.dll")
            return object(), provider

        class Ctx:
            logs = []

            def log(self, m, level="info"):
                self.logs.append((level, m))

        real_s, real_n = imaging.onnx_session, imaging._next_gpu_provider
        imaging.onnx_session = fake_session
        imaging._next_gpu_provider = lambda: "DmlExecutionProvider"
        imaging._working.clear(); imaging._sessions.clear()
        try:
            result, used = imaging.run_onnx(Ctx(), "m.onnx", "auto", lambda s, u: u)
            self.assertEqual(used, "DmlExecutionProvider")
            self.assertEqual(calls[:2], [(None, "all"), ("DmlExecutionProvider", "all")])
        finally:
            imaging.onnx_session, imaging._next_gpu_provider = real_s, real_n
            imaging._working.clear(); imaging._sessions.clear()


class DriverTests(unittest.TestCase):
    def test_cuda13_threshold(self):
        sys.path.insert(0, ROOT)
        from acestudio_worker import gpu
        self.assertTrue(gpu.supports_cuda13("591.44"))
        self.assertTrue(gpu.supports_cuda13("580.97"))
        self.assertFalse(gpu.supports_cuda13("566.36"))
        self.assertFalse(gpu.supports_cuda13(None))


@unittest.skipUnless(_has("PIL", "numpy"), "needs Pillow and NumPy")
class UpscaleFramesTests(unittest.TestCase):
    def test_several_frames_in_one_job(self):
        sys.path.insert(0, ROOT)
        import numpy as np
        from PIL import Image
        from acestudio_worker.tasks import upscale
        tmp = tempfile.mkdtemp()
        model = os.path.join(tmp, "x2.onnx")
        open(model, "wb").close()
        paths = []
        for i in range(3):
            p = os.path.join(tmp, "f%d.png" % i)
            Image.new("RGBA", (40, 20), (50 * i, 10, 200, 255 if i else 128)).save(p)
            paths.append(p)

        def fake_run_onnx(ctx, model_path, device, fn):
            def infer(x):
                return np.repeat(np.repeat(x, 2, axis=2), 2, axis=3)     # exact ×2 nearest
            return (infer, 2), "CPUExecutionProvider"

        class Ctx:
            def __init__(self):
                self.params = {"model": model, "frames": 3, "tile": 32}
                self.params.update({"input%d" % i: p for i, p in enumerate(paths)})
                self.progressed = []

            def param(self, name, default=None, required=False, kind=None):
                if required and name not in self.params:
                    raise KeyError(name)
                return self.params.get(name, default)

            def progress(self, v, msg="", **k):
                self.progressed.append(v)

            def check_cancel(self):
                pass

            def path(self, name):
                return os.path.join(tmp, name)

        real_run, real_req = upscale.run_onnx, upscale.require
        upscale.run_onnx, upscale.require = fake_run_onnx, lambda *m: None
        try:
            ctx = Ctx()
            res = upscale.run(ctx)
        finally:
            upscale.run_onnx, upscale.require = real_run, real_req
        self.assertEqual(sorted(res["outputs"]), ["image0", "image1", "image2"])
        self.assertEqual(res["info"]["frames"], 3)
        for i in range(3):
            out = Image.open(res["outputs"]["image%d" % i])
            self.assertEqual(out.size, (80, 40))
            self.assertEqual(out.getpixel((5, 5))[:3], (50 * i, 10, 200))
        self.assertEqual(ctx.progressed, sorted(ctx.progressed))   # monotonic across frames
        self.assertLessEqual(max(ctx.progressed), 0.96)


class CpuMemoryCheckTests(unittest.TestCase):
    def test_fails_fast_with_a_clear_message(self):
        sys.path.insert(0, ROOT)
        from acestudio_worker import gpu, imaging
        from acestudio_worker.jobs import JobError
        real = gpu.available_memory
        try:
            gpu.available_memory = lambda: 3 * 2**30
            with self.assertRaises(JobError) as cm:
                imaging._check_cpu_memory(6500)
            self.assertEqual(cm.exception.kind, "oom")
            self.assertIn("6.3 GB", str(cm.exception))
            self.assertIn("3.0 GB", str(cm.exception))
            gpu.available_memory = lambda: 8 * 2**30
            imaging._check_cpu_memory(6500)          # enough: no error
            gpu.available_memory = lambda: None
            imaging._check_cpu_memory(6500)          # unknown: no error
            imaging._check_cpu_memory(None)
        finally:
            gpu.available_memory = real

    def test_available_memory_is_sane(self):
        sys.path.insert(0, ROOT)
        from acestudio_worker import gpu
        v = gpu.available_memory()
        self.assertTrue(v is None or v > 0)

if __name__ == "__main__":
    unittest.main()
