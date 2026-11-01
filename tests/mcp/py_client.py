"""Drives acestudio-mcp with the official MCP Python SDK (mcp 2.3.0) in every
connect mode, over stdio and (optionally) Streamable HTTP.

usage: py_client.py MCP_EXE ROOT [HTTP_URL]
"""
import asyncio, base64, json, os, sys, warnings
import mcp_types as types
from mcp.client.client import Client
from mcp.client.stdio import StdioServerParameters

warnings.simplefilter("ignore")   # deprecation notices for legacy-only calls

exe, root = sys.argv[1], sys.argv[2]
url = sys.argv[3] if len(sys.argv) > 3 else None
failures = []

def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)

class Answer:
    """Elicitation callback: replies to the overwrite question."""
    def __init__(self):
        self.replace = True
        self.asked = []

    async def __call__(self, context, params):
        self.asked.append(params.message)
        return types.ElicitResult(action="accept", content={"replace": self.replace})


async def exercise(label, target, mode):
    print(f"== {label} mode={mode}")
    answer = Answer()
    async with Client(target, mode=mode, elicitation_callback=answer) as c:
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
        answer.replace = False
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

        # Overwrite confirmation through elicitation (2026: input_required, legacy: a request).
        answer.replace = False
        answer.asked.clear()
        r = await c.call_tool("document_new", {"path": doc, "width": 8, "height": 8})
        check(r.is_error and "kept" in r.content[0].text and len(answer.asked) == 1, "asked before replacing; 'no' keeps the file")
        answer.replace = True
        r = await c.call_tool("document_new", {"path": doc, "width": 8, "height": 8})
        check(not r.is_error and len(answer.asked) == 2, "'yes' replaces the file")
        r = await c.call_tool("document_info", {"path": doc})
        check(r.structured_content["width"] == 8, "the replaced file is the new one")

        # Resources, templates, prompts, completions.
        res = await c.list_resources()
        uris = [x.uri for x in res.resources]
        check("acestudio://docs/lua-api" in [str(u) for u in uris], "resources/list has the Lua API reference")
        rr = await c.read_resource("acestudio://docs/lua-api")
        check("ace.image" in rr.contents[0].text, "the Lua API reference reads")
        rr = await c.read_resource("acestudio://app/document")
        check('"open": false' in rr.contents[0].text, "the app document reports the app is not running")
        tl = await c.list_resource_templates()
        check(any("acestudio://project/" in t.uri_template for t in tl.resource_templates), "project templates are listed")
        rr = await c.read_resource(f"acestudio://project/{doc}")
        check('"width": 8' in rr.contents[0].text, "acestudio://project/{path} reads the project")
        rr = await c.read_resource(f"acestudio://project/{doc}/frame/0.png")
        check(base64.b64decode(rr.contents[0].blob)[:4] == b"\x89PNG", "acestudio://project/{path}/frame/{n}.png is a PNG")
        try:
            await c.read_resource("acestudio://project/missing.acestudio")
            check(False, "a missing project is an error")
        except Exception as e:
            check("not found" in str(e).lower(), "a missing project is 'resource not found'")
        pl = await c.list_prompts()
        names = [p.name for p in pl.prompts]
        check({"autotile_terrain", "export_for_engine", "lua_script"} <= set(names), "prompts are listed")
        gp = await c.get_prompt("lua_script", {"task": "draw a red square"})
        check(any(getattr(m.content, "type", "") == "resource" for m in gp.messages), "lua_script embeds the API reference")
        cp = await c.complete(types.ResourceTemplateReference(type="ref/resource", uri="acestudio://project/{+path}"),
                              {"name": "path", "value": label})
        check(doc in cp.completion.values, "completion finds the project file")
        cp = await c.complete(types.PromptReference(type="ref/prompt", name="export_for_engine"), {"name": "engine", "value": "go"})
        check("godot" in cp.completion.values, "completion of engine names")
        if era == "modern":
            async with c.listen(resource_subscriptions=["acestudio://app/document"]) as sub:
                check("acestudio://app/document" in (sub.honored.resource_subscriptions or []), "subscriptions/listen is acknowledged")

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
