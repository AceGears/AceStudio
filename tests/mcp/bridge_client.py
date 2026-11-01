"""Checks the live bridge: acestudio-mcp <-> a running AceStudio with a document open.

usage: bridge_client.py MCP_EXE ROOT

Start AceStudio with a 16x16, 2-frame project first (CI starts it offscreen).
Checks app_document (info + image), app_run_lua (a change in one undo step,
file access limits) and app_open_file.
"""
import asyncio, base64, sys, time
from mcp.client.client import Client
from mcp.client.stdio import StdioServerParameters

exe, root = sys.argv[1], sys.argv[2]
failures = []

def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)

async def main():
    async with Client(StdioServerParameters(command=exe, args=["--root", root])) as c:
        deadline = time.time() + 60
        while True:   # the app may still be starting
            r = await c.call_tool("app_document", {})
            if not r.is_error or time.time() > deadline:
                break
            await asyncio.sleep(1)
        check(not r.is_error, "app_document reaches the running app")
        if r.is_error:
            print("   ", r.content[0].text)
            return
        d = r.structured_content
        check(d["open"] and d["width"] == 16 and d["frames"] == 2, f"document info {d.get('width')}x{d.get('height')}, {d.get('frames')} frames")
        imgs = [b for b in r.content if b.type == "image"]
        check(len(imgs) == 1 and base64.b64decode(imgs[0].data)[:4] == b"\x89PNG", "current frame as a PNG")
        r = await c.call_tool("app_run_lua", {"code": "local d = ace.document(); d:add_tag('bridge', 0, 1); "
                                                      "d:set_cel(0, 0, ace.image(16, 16, '#00ff00')); print(d.frames)"})
        check(not r.is_error and r.content[0].text.strip() == "2", "app_run_lua runs on the open document")
        check(r.structured_content and r.structured_content["documentChanged"], "the change was applied")
        r = await c.call_tool("app_document", {})
        check(any(t["name"] == "bridge" for t in r.structured_content["tags"]), "the app shows the script's tag")
        r = await c.call_tool("app_run_lua", {"code": "ace.image(1, 1):save('../../acestudio_escape.png')"})
        check(r.is_error and "outside the allowed folders" in r.content[0].text, "the script cannot write outside its folders")
        r = await c.call_tool("app_run_lua", {"code": "local d = ace.document(); d:add_frame(); error('stop')"})
        check(r.is_error and "stop" in r.content[0].text, "a failing script reports its error")
        r = await c.call_tool("app_document", {})
        check(r.structured_content["frames"] == 2, "a failing script changes nothing")
        r = await c.call_tool("render_frame", {"path": "bridge.acestudio"})
        r = await c.call_tool("app_open_file", {"path": "bridge.acestudio"})
        check(not r.is_error, "app_open_file")

asyncio.run(main())
print("FAILURES:", failures if failures else "none")
sys.exit(1 if failures else 0)
