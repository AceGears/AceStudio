"""Checks the live bridge: acestudio-mcp <-> a running AceStudio with a document open.

usage: bridge_client.py MCP_EXE ROOT

Start AceStudio with a 16x16, 2-frame project first (CI starts it offscreen).
Checks app_document (info + image), app_run_lua (a change in one undo step,
file access limits) and app_open_file.
"""
import asyncio, base64, sys, time, warnings
import mcp_types as types
from mcp.client.client import Client
from mcp.client.stdio import StdioServerParameters

warnings.simplefilter("ignore")

exe, root = sys.argv[1], sys.argv[2]
failures = []

def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)

updates = []

async def on_message(message):
    if isinstance(message, types.ServerNotification) or hasattr(message, "root"):
        n = getattr(message, "root", message)
        if getattr(n, "method", "") == "notifications/resources/updated":
            updates.append(n.params.uri)

async def main():
    for m in ("2026-07-28", "legacy"):
        await run(m)

async def run(mode):
    global current_mode
    print("== mode", mode)
    async with Client(StdioServerParameters(command=exe, args=["--root", root]), mode=mode, message_handler=on_message) as c:
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
        check(d["open"] and d["width"] == 16 and d["frames"] >= 2, f"document info {d.get('width')}x{d.get('height')}, {d.get('frames')} frames")
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
        check(r.structured_content["frames"] == d["frames"], "a failing script changes nothing")
        r = await c.call_tool("app_run_lua", {"code": "print('partial'); error('late failure')"})
        check(r.is_error and "partial" in r.content[0].text and "late failure" in r.content[0].text,
              "a failing script's output comes back with its error")
        r = await c.call_tool("app_documents", {})
        docs = r.structured_content["documents"] if not r.is_error else []
        check(len(docs) >= 1 and docs[0]["width"] == 16 and any(d["current"] for d in docs), f"app_documents lists {len(docs)} open document(s)")
        r = await c.call_tool("app_document", {"document": 0})
        check(not r.is_error and r.structured_content["width"] == 16, "app_document by tab index")
        r = await c.call_tool("app_document", {"document": 99})
        check(r.is_error and "no open document 99" in r.content[0].text.lower(), "a missing tab is an error")
        r = await c.call_tool("app_open_file", {"path": "bridge.acestudio"})
        check(not r.is_error, "app_open_file")
        rr = await c.read_resource("acestudio://app/document/frame.png")
        check(base64.b64decode(rr.contents[0].blob)[:4] == b"\x89PNG", "acestudio://app/document/frame.png")

        # Modern subscription: a change in the app reaches the listen stream.
        if mode != "legacy":
            got = asyncio.Event()
            async def listen():
                async with c.listen(resource_subscriptions=["acestudio://app/document"]) as sub:
                    ready.set()
                    async for event in sub:
                        if getattr(event, "uri", "") == "acestudio://app/document":
                            got.set()
                            return
            ready = asyncio.Event()
            task = asyncio.create_task(listen())
            await asyncio.wait_for(ready.wait(), 10)
            await asyncio.sleep(1.5)   # the watcher connects to the app
            # The change comes from another acestudio-mcp process, so only the
            # app's watch channel can tell this server about it (as with edits
            # made by the user in the app).
            async with Client(StdioServerParameters(command=exe, args=["--root", root]), mode="legacy") as other:
                await other.call_tool("app_run_lua", {"code": "ace.document():add_tag('sub', 0, 0)"})
            try:
                await asyncio.wait_for(got.wait(), 10)
                check(True, "subscriptions/listen reports a change made elsewhere (app watch)")
            except asyncio.TimeoutError:
                check(False, "subscriptions/listen reports a change made elsewhere (app watch)")
            task.cancel()
        else:
            await c.subscribe_resource("acestudio://app/document")
            await asyncio.sleep(1.5)
            updates.clear()
            async with Client(StdioServerParameters(command=exe, args=["--root", root]), mode="2026-07-28") as other:
                await other.call_tool("app_run_lua", {"code": "ace.document():add_tag('legacy_sub', 0, 0)"})
            for _ in range(50):
                if "acestudio://app/document" in [str(u) for u in updates]:
                    break
                await asyncio.sleep(0.2)
            check("acestudio://app/document" in [str(u) for u in updates], "resources/subscribe (legacy) reports a change made elsewhere (app watch)")

asyncio.run(main())
print("FAILURES:", failures if failures else "none")
sys.exit(1 if failures else 0)
