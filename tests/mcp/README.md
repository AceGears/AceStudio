# acestudio-mcp protocol tests

The protocol layer has unit tests in `tests/test_mcp.cpp` (they run with CTest).
The checks below run the real server against official MCP tooling. CI runs them
on the packaged Windows build.

## Tools

The tool versions are pinned:

- `@modelcontextprotocol/conformance` 0.1.16
- `@modelcontextprotocol/sdk` 1.32.2: the TypeScript client, protocol 2025-11-25
- `mcp` 2.3.0: the Python SDK, a dual-era client that speaks 2026-07-28 and falls back to 2025-11-25

Install them once:

```
cd tests/mcp
npm install
python -m venv .venv
.venv/bin/pip install -r requirements.txt     # Windows: .venv\Scripts\pip
```

## Run

```
# A scratch root folder for the tools to write into
mkdir /tmp/mcproot

# Conformance suite (Streamable HTTP, legacy era). --conformance-fixtures adds
# the suite's fixture tools (test_simple_text, ...) so it can check content
# types, tool errors, progress and log notifications.
acestudio-mcp --root /tmp/mcproot --http 39200 --conformance-fixtures &
npx conformance server --url http://127.0.0.1:39200/mcp --suite all \
    --expected-failures conformance-baseline.yml

# Python SDK, auto / 2026-07-28 / legacy modes, over stdio and HTTP
.venv/bin/python py_client.py <path to acestudio-mcp> /tmp/mcproot http://127.0.0.1:39200/mcp

# TypeScript SDK, over stdio and HTTP
node ts_client.mjs <path to acestudio-mcp> /tmp/mcproot http://127.0.0.1:39200/mcp
```

## Baseline

`conformance-baseline.yml` lists the scenarios this server is expected to fail,
with the reason for each. They test features acestudio-mcp does not offer
(resources, prompts, completions, sampling and elicitation) or SSE resumability.

The suite fails if a scenario not in the baseline fails. It also fails if a
listed scenario starts passing, so the baseline cannot go stale.
