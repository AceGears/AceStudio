"""AceStudio AI worker.

Runs as a child process of AceStudio and speaks newline-delimited JSON-RPC 2.0
on stdin/stdout. The core of the worker uses only the Python standard library
so it can start (and report what is missing) before any AI packages are
installed; each task imports its heavy dependencies lazily.
"""

PROTOCOL_VERSION = 1
__version__ = "0.6.0"
