"""PreToolUse hook: block Write/Edit calls that would put credentials into files.

Reads the hook JSON from stdin. Exit 0 = allow, exit 2 = block (stderr is shown to Claude).
Stdlib only so it runs on any Python 3.8+.
"""
import json
import re
import sys

PATTERNS = [
    ("private key block", re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH |DSA |PGP )?PRIVATE KEY-----")),
    ("AWS access key id", re.compile(r"\bAKIA[0-9A-Z]{16}\b")),
    ("GitHub token", re.compile(r"\bgh[pousr]_[A-Za-z0-9]{36,}\b")),
    ("OpenAI/Anthropic-style key", re.compile(r"\bsk-(?:ant-)?[A-Za-z0-9_\-]{32,}\b")),
    ("Hugging Face token", re.compile(r"\bhf_[A-Za-z0-9]{30,}\b")),
    ("Slack token", re.compile(r"\bxox[abprs]-[A-Za-z0-9-]{10,}\b")),
    ("Google API key", re.compile(r"\bAIza[0-9A-Za-z_\-]{35}\b")),
    (
        "hardcoded secret assignment",
        re.compile(
            r"(?i)\b(?:api[_-]?key|secret|password|passwd|access[_-]?token|auth[_-]?token)\b"
            r"\s*[:=]\s*[\"'][^\"'\s]{16,}[\"']"
        ),
    ),
]

ALLOW_MARKER = "secret-guard: allow-test-fixture"


def main() -> int:
    try:
        payload = json.load(sys.stdin)
    except (json.JSONDecodeError, ValueError):
        return 0  # not our input; never block on hook parse failure
    tool_input = payload.get("tool_input") or {}
    text = "\n".join(
        str(tool_input.get(k, "")) for k in ("content", "new_string") if tool_input.get(k)
    )
    if not text or ALLOW_MARKER in text:
        return 0
    hits = [name for name, rx in PATTERNS if rx.search(text)]
    if hits:
        path = tool_input.get("file_path", "<unknown>")
        sys.stderr.write(
            f"secret_guard blocked write to {path}: looks like it contains {', '.join(hits)}. "
            "Store credentials in Windows Credential Manager / environment, never in files. "
            f"For deliberate fake test fixtures add the comment '{ALLOW_MARKER}'.\n"
        )
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
