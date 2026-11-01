"""Drives acestudio-mcp with the official MCP Python SDK (mcp 2.3.0) in every
connect mode, over stdio and (optionally) Streamable HTTP.

usage: py_client.py MCP_EXE ROOT [HTTP_URL]
"""
import asyncio, base64, json, os, sys
from mcp.client.client import Client
from mcp.client.stdio import StdioServerParameters

exe, root = sys.argv[1], sys.argv[2]
url = sys.argv[3] if len(sys.argv) > 3 else None
failures = []

def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)

async def exercise(label, target, mode):
    print(f"== {label} mode={mode}")
    async with Client(target, mode=mode) as c:
        era = "modern" if getattr(c.session, "discover_result", None) else "legacy"
        print("   era:", era, "version:", getattr(c.session, "protocol_version", None) or "")
        if mode == "auto" or mode.startswith("2026"):
            check(era == "modern", "negotiated the modern era")
        if mode == "legacy":
            check(era == "legacy", "used the initialize handshake")
        tools = await c.list_tools()
        names = [t.name for t in tools.tools]
        check(names == sorted(names) and len(names) >= 15, f"{len(names)} tools in a deterministic order")
        doc = f"{label}-{mode}.acestudio".replace("/", "_")
        r = await c.call_tool("document_new", {"path": doc, "width": 16, "height": 12, "frames": 3, "overwrite": True})
        check(not r.is_error and r.structured_content["frames"] == 3, "document_new")
        r = await c.call_tool("render_frame", {"path": doc, "frame": 2})
        imgs = [b for b in r.content if b.type == "image"]
        check(not r.is_error and len(imgs) == 1 and base64.b64decode(imgs[0].data)[:4] == b"\x89PNG", "render_frame returns a PNG")
        r = await c.call_tool("run_lua", {"code": "local d = ace.open(arg[1]); print(d.frames, d.width)", "args": [doc]})
        check(not r.is_error and r.content[0].text.strip() == "3\t16", "run_lua reads the project")
        r = await c.call_tool("document_info", {"path": "../outside.acestudio"})
        check(r.is_error and "outside" in r.content[0].text, "paths outside the root are refused")
        r = await c.call_tool("document_new", {"path": doc, "width": 16, "height": 12})
        check(r.is_error, "existing files are not overwritten")
        r = await c.call_tool("document_new", {"path": doc})
        check(r.is_error and "missing required argument" in r.content[0].text, "argument errors are tool errors")
        try:
            await c.call_tool("no_such_tool", {})
            check(False, "unknown tool is a protocol error")
        except Exception as e:
            check("Unknown tool" in str(e), "unknown tool is a protocol error")
        if era == "legacy":
            await c.send_ping()
            check(True, "ping (legacy)")

async def main():
    params = StdioServerParameters(command=exe, args=["--root", root])
    for mode in ("auto", "2026-07-28", "legacy"):
        await exercise("stdio", params, mode)
    if url:
        for mode in ("auto", "2026-07-28", "legacy"):
            await exercise("http", url, mode)
    print("FAILURES:", failures if failures else "none")
    sys.exit(1 if failures else 0)

asyncio.run(main())
