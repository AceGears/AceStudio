"""model.download transport rules: allowed URL schemes and credentials on redirects."""

import http.server
import os
import socketserver
import sys
import tempfile
import threading
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

from acestudio_worker.jobs import JobError  # noqa: E402
from acestudio_worker.tasks import model_download as md  # noqa: E402

PAYLOAD = b"model-bytes" * 1000


class _Ctx:
    def check_cancel(self):
        pass

    def progress(self, *a, **k):
        pass


def _serve(handler_cls):
    httpd = socketserver.ThreadingTCPServer(("127.0.0.1", 0), handler_cls)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd


class DownloadSecurityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        seen = cls.seen = {}

        class Target(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_GET(self):
                seen["auth"] = self.headers.get("Authorization")
                seen["host"] = self.headers.get("Host")
                self.send_response(200)
                self.send_header("Content-Length", str(len(PAYLOAD)))
                self.end_headers()
                self.wfile.write(PAYLOAD)

        cls.target = _serve(Target)
        tport = cls.target.server_address[1]

        class Redirector(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_GET(self):
                # /cross -> different host name (localhost), /same -> same host (127.0.0.1),
                # /file -> file:// (must be refused)
                loc = {"/cross": "http://localhost:%d/m" % tport,
                       "/same": "http://127.0.0.1:%d/m" % tport,
                       "/file": "file:///C:/Windows/win.ini"}[self.path]
                self.send_response(302)
                self.send_header("Location", loc)
                self.send_header("Content-Length", "0")
                self.end_headers()

        cls.redir = _serve(Redirector)
        cls.rport = cls.redir.server_address[1]

    @classmethod
    def tearDownClass(cls):
        for s in (cls.target, cls.redir):
            s.shutdown()
            s.server_close()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.part = os.path.join(self.tmp.name, "m.part")
        self.seen.clear()

    def tearDown(self):
        self.tmp.cleanup()

    def fetch(self, url, headers=None):
        md._download(_Ctx(), url, self.part, 0, headers or {}, 0, 0)

    def test_disallowed_schemes_are_refused(self):
        for url in ("file:///C:/Windows/win.ini", "ftp://example.com/m.bin",
                    "http://example.com/m.bin", "https:///nohost"):
            with self.subTest(url=url):
                with self.assertRaises(JobError) as cm:
                    self.fetch(url)
                self.assertIn("https", str(cm.exception))
                self.assertFalse(os.path.exists(self.part))

    def test_https_and_loopback_http_are_allowed(self):
        md._check_url("https://huggingface.co/x/resolve/abc/m.safetensors")
        md._check_url("http://127.0.0.1:8000/m.bin")
        md._check_url("http://localhost/m.bin")

    def test_credentials_dropped_on_cross_host_redirect(self):
        self.fetch("http://127.0.0.1:%d/cross" % self.rport, {"Authorization": "Bearer test-token-xyz"})
        self.assertTrue(self.seen["host"].startswith("localhost"))
        self.assertIsNone(self.seen["auth"])
        with open(self.part, "rb") as f:
            self.assertEqual(f.read(), PAYLOAD)

    def test_credentials_kept_on_same_host_redirect(self):
        self.fetch("http://127.0.0.1:%d/same" % self.rport, {"Authorization": "Bearer test-token-xyz"})
        self.assertEqual(self.seen["auth"], "Bearer test-token-xyz")

    def test_redirect_to_file_url_is_refused(self):
        with self.assertRaises(JobError):
            self.fetch("http://127.0.0.1:%d/file" % self.rport)


if __name__ == "__main__":
    unittest.main()
