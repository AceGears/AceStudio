"""Offline mode: when AceStudio's "Disable all network access" is on, the
host starts the worker with ACESTUDIO_OFFLINE=1. Every outgoing connection
from this process (and any library it loads) to anything but this computer
is then refused at the socket layer, and the tasks that need the network
(model downloads, runtime installs) refuse to start.

Loopback addresses stay allowed (some libraries talk to themselves locally),
but proxies are switched off, so a proxy running on this computer cannot
relay a connection out.
"""

import ipaddress
import os
import socket

NETWORK_TASKS = ("model.download", "runtime.install")

_installed = False


class NetworkDisabled(OSError):
    pass


def offline():
    return os.environ.get("ACESTUDIO_OFFLINE", "") == "1"


def _local_host(host):
    if host is None:
        return False
    if isinstance(host, bytes):
        host = host.decode("ascii", "replace")
    host = str(host).strip("[]").lower()
    if host in ("localhost", "localhost.localdomain", ""):
        return True
    try:
        return ipaddress.ip_address(host.split("%", 1)[0]).is_loopback
    except ValueError:
        return False   # a name: only resolved through the network


def _check(address):
    # AF_UNIX paths are strings or bytes; inet addresses are tuples.
    if isinstance(address, tuple) and address and not _local_host(address[0]):
        raise NetworkDisabled("Network access is disabled in AceStudio (Settings > Privacy)")


def install():
    """Refuse non-local connections for the rest of this process."""
    global _installed
    if _installed:
        return
    _installed = True
    real_connect = socket.socket.connect
    real_connect_ex = socket.socket.connect_ex
    real_getaddrinfo = socket.getaddrinfo

    def connect(self, address):
        _check(address)
        return real_connect(self, address)

    def connect_ex(self, address):
        _check(address)
        return real_connect_ex(self, address)

    def getaddrinfo(host, *args, **kwargs):
        if not _local_host(host):
            raise NetworkDisabled("Network access is disabled in AceStudio (Settings > Privacy)")
        return real_getaddrinfo(host, *args, **kwargs)

    socket.socket.connect = connect
    socket.socket.connect_ex = connect_ex
    socket.getaddrinfo = getaddrinfo
    # Libraries that would try anyway stay quiet.
    for k in ("HF_HUB_OFFLINE", "TRANSFORMERS_OFFLINE", "HF_DATASETS_OFFLINE", "DIFFUSERS_OFFLINE"):
        os.environ[k] = "1"
    os.environ["PIP_NO_INDEX"] = "1"
    # A proxy on this computer (a security suite, a debugging proxy) would
    # relay a loopback connection to the internet: no proxies at all. Python
    # reads them from the environment and, on Windows, the registry.
    for k in list(os.environ):
        if k.lower() in ("http_proxy", "https_proxy", "all_proxy", "ftp_proxy"):
            del os.environ[k]
    os.environ["NO_PROXY"] = os.environ["no_proxy"] = "*"
    import urllib.request
    urllib.request.getproxies = lambda: {}


def install_if_requested():
    if offline():
        install()
        return True
    return False
