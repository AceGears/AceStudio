// acestudio-mcp with the official TypeScript SDK client (1.32.2, protocol 2025-11-25).
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";
import { StreamableHTTPClientTransport } from "@modelcontextprotocol/sdk/client/streamableHttp.js";

const [exe, root, url] = process.argv.slice(2);
const failures = [];
const check = (c, what) => { console.log((c ? "  ok   " : "  FAIL ") + what); if (!c) failures.push(what); };

async function run(label, transport) {
  console.log("== " + label);
  const c = new Client({ name: "ts-test", version: "1.0.0" });
  await c.connect(transport);
  check(c.getServerVersion()?.name === "acestudio", "serverInfo");
  const tools = (await c.listTools()).tools;
  check(tools.length >= 15 && tools.every(t => t.inputSchema?.type === "object"), `${tools.length} tools with object schemas`);
  const doc = `ts-${label}.acestudio`;
  let r = await c.callTool({ name: "document_new", arguments: { path: doc, width: 8, height: 8, overwrite: true } });
  check(!r.isError, "document_new");
  r = await c.callTool({ name: "uikit_generate", arguments: { out: `ui-${label}.png`, engines: "godot", overwrite: true } });
  check(!r.isError && r.structuredContent?.files?.length >= 2, "uikit_generate writes files");
  const progress = [];
  r = await c.callTool({ name: "render_frame", arguments: { path: doc } }, undefined, { onprogress: p => progress.push(p) });
  check(r.content.some(b => b.type === "image" && b.mimeType === "image/png"), "render_frame image");
  r = await c.callTool({ name: "app_document", arguments: {} });
  check(r.isError && /not running/.test(r.content[0].text), "app_document reports the app is not running");
  await c.ping();
  check(true, "ping");
  await c.close();
}

await run("stdio", new StdioClientTransport({ command: exe, args: ["--root", root] }));
if (url) await run("http", new StreamableHTTPClientTransport(new URL(url)));
console.log("FAILURES:", failures.length ? failures : "none");
process.exit(failures.length ? 1 : 0);
