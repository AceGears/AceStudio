import os
import socket
import subprocess
import sys
import textwrap
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def run(code, offline):
    env = dict(os.environ)
    env["PYTHONPATH"] = ROOT
    env["ACESTUDIO_OFFLINE"] = "1" if offline else "0"
    return subprocess.run([sys.executable, "-c", textwrap.dedent(code)], env=env, capture_output=True, text=True, timeout=60)


class NetGuardTests(unittest.TestCase):
    def test_offline_refuses_remote_and_allows_loopback(self):
        code = """
            import socket, threading
            from acestudio_worker import netguard
            assert netguard.install_if_requested()
            # A remote address is refused before any packet is sent.
            s = socket.socket()
            try:
                s.connect(("93.184.216.34", 80)); print("REMOTE-CONNECTED")
            except netguard.NetworkDisabled:
                print("REMOTE-REFUSED")
            # So is resolving a name.
            try:
                socket.getaddrinfo("example.com", 443); print("DNS-OK")
            except netguard.NetworkDisabled:
                print("DNS-REFUSED")
            # urllib goes through the same sockets.
            import urllib.request
            try:
                urllib.request.urlopen("https://example.com", timeout=5); print("URL-OK")
            except Exception as e:
                print("URL-REFUSED" if "disabled" in str(e) else "URL-OTHER " + repr(e))
            # This computer is allowed.
            srv = socket.socket(); srv.bind(("127.0.0.1", 0)); srv.listen(1)
            c = socket.socket(); c.connect(("127.0.0.1", srv.getsockname()[1])); print("LOOPBACK-OK")
            print(socket.getaddrinfo("localhost", 80)[0][0] is not None and "LOCALHOST-OK")
        """
        r = run(code, True)
        self.assertEqual(r.returncode, 0, r.stderr)
        for want in ("REMOTE-REFUSED", "DNS-REFUSED", "URL-REFUSED", "LOOPBACK-OK", "LOCALHOST-OK"):
            self.assertIn(want, r.stdout)

    def test_online_installs_nothing(self):
        r = run("from acestudio_worker import netguard; import socket; print(netguard.install_if_requested(), socket.getaddrinfo.__module__)", False)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("False socket", r.stdout)


if __name__ == "__main__":
    unittest.main()
