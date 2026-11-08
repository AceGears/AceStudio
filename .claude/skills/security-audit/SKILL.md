---
name: security-audit
description: Run a privacy and security audit of AceStudio — threat model, local servers and IPC, network egress and telemetry, credential handling, model/dependency supply chain, deserialization, file import, process launching, MCP roots, Lua/plugin sandbox, and secret scanning — producing evidence-backed findings with severity and owners. Use for audits and before releases.
---

# Security & Privacy Audit

## 1. Inventory and data flow
List every process (app, CLI, MCP, worker), every listening socket, every outbound destination, every file location written, every credential. Draw the data flow in `SECURITY.md`.

## 2. Static review (source available parts)
Grep the worker and any source for:
```
bind|listen|host=|0.0.0.0|HTTPServer|uvicorn|flask|aiohttp
requests\.|urllib|http\.client|socket\.|urlopen
pickle|torch\.load|weights_only|yaml\.load|eval\(|exec\(|trust_remote_code
subprocess|os\.system|shell=True|Popen
open\(|os\.path\.join|\.\.|extractall|zipfile|tarfile
token|api_key|secret|password|Authorization
```
For each hit: is input validated? is the path confined? is the server authenticated? Origin/Host checked (DNS rebinding)? Timeouts set? TLS verified?

## 3. Binary review (no source)
- Strings scan for URLs/hosts and telemetry SDK names:
  `python -I -c "import re,sys;d=open(sys.argv[1],'rb').read();print('\n'.join(sorted({m.decode() for m in re.findall(rb'https?://[\x21-\x7e]{4,200}',d)})))" AceStudio.exe`
- Look for known analytics/crash SDKs (sentry, segment, mixpanel, amplitude, appcenter, google-analytics, firebase).
- Inspect the import table for networking DLLs (`Qt6Network`, `winhttp`, `wininet`).

## 4. Runtime observation
Start the app/worker and list sockets and connections:
```powershell
Get-NetTCPConnection | Where-Object { (Get-Process -Id $_.OwningProcess -ErrorAction SilentlyContinue).ProcessName -match 'AceStudio|python|acestudio' } | Select-Object LocalAddress,LocalPort,RemoteAddress,RemotePort,State,OwningProcess
```
Any listener on `0.0.0.0`/`::` = finding. Any remote address during normal offline use = finding unless documented (e.g. explicit model download).

## 5. Supply chain
Pinned versions in `requirements/*.txt`; hashes present?; `pip-audit -r <file>` if available (do not install tools without approval); model downloads over HTTPS with SHA-256 verification; safetensors only.

## 6. Privacy requirements check
No account · no license key · no ad/analytics trackers · no unnecessary telemetry · no silent uploads · Cloud Mode opt-in and inputs-only · logs free of secrets and unnecessary personal paths.

## 7. Findings format → `docs/security/<date>-audit.md`
`SEC-###  Severity  Component  Evidence  Scenario  Fix  Owner  Verification`
Never claim the app "is secure"; state what was checked and what was not.
