"""SEC-002: runtime.install never lets one pip run search two indexes (stdlib only).

Run from the ai_worker folder:  python -m unittest discover -s tests
"""

import glob
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

from acestudio_worker.tasks import runtime_install as ri  # noqa: E402


class Ctx:
    def __init__(self):
        self.logs = []

    def log(self, m, level="info"):
        self.logs.append(m)

    def progress(self, *a, **k):
        pass

    def cancelled(self):
        return False


class IndexSeparationTests(unittest.TestCase):
    def setUp(self):
        self.calls = []
        self.real_pip, self.real_reqs = ri._pip, ri._requirements_of
        ri._pip = lambda ctx, args, label: self.calls.append(list(args))
        ri._requirements_of = lambda ctx, dist: ["filelock", "sympy>=1.13.3", "typing-extensions>=4.10.0"]

    def tearDown(self):
        ri._pip, ri._requirements_of = self.real_pip, self.real_reqs

    def indexes(self, args):
        return [args[i + 1] for i, a in enumerate(args) if a == "--index-url"]

    def test_requirement_files_name_no_index(self):
        for path in glob.glob(os.path.join(ri.REQ_DIR, "*.txt")):
            with open(path, encoding="utf-8") as f:
                for line in f:
                    with self.subTest(file=os.path.basename(path), line=line):
                        self.assertFalse(line.lstrip().startswith(("--index-url", "--extra-index-url", "-i ",
                                                                   "--find-links", "-f ")))

    def test_every_torch_file_has_its_pytorch_index(self):
        for name in ("torch-cpu.txt", "torch-cuda.txt", "torch-cuda13.txt"):
            self.assertTrue(ri.TORCH_INDEX[name].startswith("https://download.pytorch.org/whl/"))
        for files in ri.PROFILES.values():
            for name in files:
                if name.startswith("torch-"):
                    self.assertIn(name, ri.TORCH_INDEX)

    def test_torch_alone_from_pytorch_then_dependencies_from_pypi(self):
        ri._install_file(Ctx(), "torch-cuda.txt", os.path.join(ri.REQ_DIR, "torch-cuda.txt"))
        self.assertEqual(len(self.calls), 2)
        torch, deps = self.calls
        self.assertEqual(self.indexes(torch), ["https://download.pytorch.org/whl/cu126"])
        self.assertIn("--no-deps", torch)
        self.assertEqual(self.indexes(deps), [ri.PYPI_INDEX])
        self.assertIn("sympy>=1.13.3", deps)
        self.assertNotIn("--no-deps", deps)

    def test_other_files_from_pypi_only(self):
        ri._install_file(Ctx(), "diffusion.txt", os.path.join(ri.REQ_DIR, "diffusion.txt"))
        self.assertEqual(self.calls, [["--index-url", ri.PYPI_INDEX, "-r", os.path.join(ri.REQ_DIR, "diffusion.txt")]])

    def test_no_run_mixes_indexes(self):
        for name in ("torch-cpu.txt", "torch-cuda13.txt", "base.txt", "onnx-cuda.txt"):
            ri._install_file(Ctx(), name, os.path.join(ri.REQ_DIR, name))
        for args in self.calls:
            with self.subTest(args=args):
                self.assertEqual(len(self.indexes(args)), 1)
                self.assertNotIn("--extra-index-url", args)


class PipEnvironmentTests(unittest.TestCase):
    def test_extra_index_from_environment_is_dropped(self):
        seen = {}

        class FakeProc:
            def __init__(self, cmd, **kw):
                seen.update(kw)
                self.stdout = iter([])

            def poll(self):
                return 0

            def wait(self):
                return 0

        real = ri.subprocess.Popen
        os.environ["PIP_EXTRA_INDEX_URL"] = "https://evil.invalid/simple"
        ri.subprocess.Popen = FakeProc
        try:
            ri._pip(Ctx(), ["--index-url", ri.PYPI_INDEX, "-r", "x.txt"], "x.txt")
        finally:
            ri.subprocess.Popen = real
            del os.environ["PIP_EXTRA_INDEX_URL"]
        self.assertNotIn("PIP_EXTRA_INDEX_URL", seen["env"])


class RequirementsOfTests(unittest.TestCase):
    """Reads Requires-Dist of an installed distribution, keeping only what
    applies to this platform and no optional extras."""

    def test_markers_and_extras(self):
        try:
            import pip._vendor.packaging  # noqa: F401
        except ImportError:
            self.skipTest("pip is not installed")
        with tempfile.TemporaryDirectory() as tmp:
            info = os.path.join(tmp, "fakepkg-1.0.dist-info")
            os.makedirs(info)
            with open(os.path.join(info, "METADATA"), "w", encoding="utf-8") as f:
                f.write("Metadata-Version: 2.1\nName: fakepkg\nVersion: 1.0\n"
                        "Requires-Dist: filelock\n"
                        "Requires-Dist: sympy>=1.13.3\n"
                        "Requires-Dist: networkx; python_version >= \"3.0\"\n"
                        "Requires-Dist: neverhere==1.0; python_version < \"3.0\"\n"
                        "Requires-Dist: optdep; extra == \"opt\"\n")
            old = os.environ.get("PYTHONPATH")
            os.environ["PYTHONPATH"] = tmp
            try:
                deps = ri._requirements_of(Ctx(), "fakepkg")
            finally:
                if old is None:
                    del os.environ["PYTHONPATH"]
                else:
                    os.environ["PYTHONPATH"] = old
        self.assertEqual(sorted(deps), ["filelock", "networkx", "sympy>=1.13.3"])

    def test_missing_distribution_is_a_clear_error(self):
        with self.assertRaises(ri.JobError):
            ri._requirements_of(Ctx(), "no-such-distribution-acestudio")


if __name__ == "__main__":
    unittest.main()
