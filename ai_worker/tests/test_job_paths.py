"""SEC-005: the worker refuses relative and '..' job paths (stdlib only).

Run from the ai_worker folder:  python -m unittest discover -s tests
"""

import os
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

from acestudio_worker.jobs import JobError, check_job_paths  # noqa: E402

from test_server import Worker  # noqa: E402


class CheckJobPathsTests(unittest.TestCase):
    def setUp(self):
        self.base = os.path.abspath(tempfile.gettempdir())
        self.job = os.path.join(self.base, "jobs", "j1-abc")

    def p(self, *parts):
        return os.path.join(self.base, *parts)

    def refused(self, params, job_dir=None):
        with self.assertRaises(JobError) as cm:
            check_job_paths(self.job if job_dir is None else job_dir, params)
        self.assertEqual(cm.exception.kind, "invalid_input")
        return str(cm.exception)

    def test_normal_job_is_accepted(self):
        check_job_paths(self.job, {
            "model_dir": self.p("models", "sdxl"),
            "input": os.path.join(self.job, "in_input.png"),
            "input2": os.path.join(self.job, "in_input2.png"),
            "reference0": os.path.join(self.job, "ref.png"),
            "references": [self.p("anim", "ref0.png")],
            "init_image": "",
            "lora": {"path": self.p("models", "lora.safetensors"), "scale": 0.8},
            "preview": {"dir": self.p("models", "taesd"), "every": 4},
            "cutout": {"model": self.p("models", "birefnet.onnx")},
            "controlnet": {"dir": self.p("models", "cn"), "image": self.p("anim", "pose.png")},
            "ip_adapter": {"dir": self.p("models", "ip"), "weight": "ip.safetensors",
                           "encoder": "models/image_encoder", "images": [self.p("anim", "r.png")]},
            "dest_dir": self.p("models", "tiny"),
            "prompt": "../not a path",
            "steps": 4,
        })

    def test_job_folder_must_be_absolute_without_dotdot(self):
        self.refused({}, job_dir="jobs/j1")
        self.refused({}, job_dir=os.path.join(self.base, "jobs", "..", "..", "j1"))
        self.refused({}, job_dir="")

    def test_traversal_and_relative_paths_are_refused(self):
        evil = os.path.join(self.job, "..", "..", "secret.png")
        for params in ({"input": evil}, {"input7": evil}, {"mask": "mask.png"}, {"init_image": evil},
                       {"reference1": "ref.png"}, {"references": [evil]},
                       {"model": "model.onnx"}, {"model_dir": evil}, {"vae_dir": "vae"},
                       {"dest_dir": os.path.join(self.base, "models", "..", "..", "drop")},
                       {"dest_dir": "relative/drop"},
                       {"lora": {"path": evil}}, {"lora_preload": {"path": "l.safetensors"}},
                       {"preview": {"dir": evil}}, {"cutout": {"model": evil}},
                       {"controlnet": {"dir": evil}}, {"controlnet": {"image": "pose.png"}},
                       {"ip_adapter": {"dir": evil}}, {"ip_adapter": {"images": [evil]}},
                       {"ip_adapter": {"weight": "../../x.safetensors"}},
                       {"ip_adapter": {"encoder": self.p("elsewhere")}},
                       {"input": 42}):
            with self.subTest(params=params):
                self.refused(params)

    def test_backslash_traversal_is_refused(self):
        self.refused({"dest_dir": self.base + "\\models\\..\\..\\drop"})


class SubmitRefusesTraversalTests(unittest.TestCase):
    """The protocol answers a bad submit with an error; the worker keeps running."""

    def setUp(self):
        self.w = Worker()
        self.tmp = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.w.close()
        self.tmp.cleanup()

    def test_relative_output_dir_is_rejected_at_submit(self):
        rid = self.w.send("job.submit", {"job": "p1", "task": "model.download", "dir": os.path.join(self.tmp.name, "p1"),
                                         "params": {"dest_dir": "models/evil", "license_accepted": "x", "files": []}})
        msg = self.w.response(rid)
        self.assertIn("error", msg)
        self.assertIn("dest_dir", msg["error"]["message"])
        self.assertFalse(os.path.exists(os.path.join(self.tmp.name, "p1")))

    def test_dotdot_job_dir_is_rejected_and_worker_survives(self):
        rid = self.w.send("job.submit", {"job": "p2", "task": "diag.echo",
                                         "dir": os.path.join(self.tmp.name, "a", "..", "..", "p2"), "params": {}})
        self.assertIn("error", self.w.response(rid))
        ok = self.w.send("job.submit", {"job": "p3", "task": "diag.echo", "dir": os.path.join(self.tmp.name, "p3"),
                                        "params": {}})
        self.assertIn("result", self.w.response(ok))


if __name__ == "__main__":
    unittest.main()
