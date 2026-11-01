# acestudio-mcp protocol tests

The protocol layer has unit tests in `tests/test_mcp.cpp` (they run with CTest).
The checks below run the real server against official MCP tooling. CI runs them
on the packaged Windows build.

## Tools

The tool versions are pinned:

- `@modelcontextprotocol/conformance` 0.2.0-alpha.12: covers 2025-06-18, 2025-11-25 and 2026-07-28, plus the tasks extension
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

# Conformance suite (Streamable HTTP, both eras). --conformance-fixtures adds
# the suite's fixture tools, prompts and resources (test_simple_text, greet,
# slow_compute, test://static-text, ...).
acestudio-mcp --root /tmp/mcproot --http 39200 --conformance-fixtures &
npx conformance server --url http://127.0.0.1:39200/mcp --suite all
npx conformance server --url http://127.0.0.1:39200/mcp --suite all --spec-version 2025-06-18   # and 2025-11-25, 2026-07-28

# Python SDK, auto / 2026-07-28 / legacy modes, over stdio and HTTP
.venv/bin/python py_client.py <path to acestudio-mcp> /tmp/mcproot http://127.0.0.1:39200/mcp

# TypeScript SDK, over stdio and HTTP
node ts_client.mjs <path to acestudio-mcp> /tmp/mcproot http://127.0.0.1:39200/mcp
```

## No baseline

Every scenario must pass; there is no list of expected failures. With 0.2.0-alpha.12:
all scenarios 228/228, 2026-07-28 150/150, 2025-11-25 80/80, 2025-06-18 56/56.
The optional `tasks-status-notifications` scenario has no checks to run, because the server
does not send `notifications/tasks` (clients poll `tasks/get`).
