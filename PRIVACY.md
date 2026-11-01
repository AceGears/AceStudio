# AceStudio Privacy

> **Draft for legal review.** This describes what the software does today (version 0.9.0),
> checked against the source code. Before release it must be reviewed by a lawyer and the
> publisher's legal name and contact address filled in.

## Summary

AceStudio runs on your computer. It has **no accounts, no analytics, no telemetry, no
advertising, no crash upload and no update check**. Your projects, images, prompts and
settings stay on your machine. The program connects to the internet only when you ask it to
set up or download AI components, and then only to the sites listed below.

## What is stored, and where

All of it is on your computer. Nothing below is sent anywhere.

| Data | Location (Windows) | Purpose |
|---|---|---|
| Settings, recent files, shortcuts, window layout | `%APPDATA%\AceStudio\settings.json` | Remember your preferences |
| Logs | `%APPDATA%\AceStudio\logs` | Help you (and support, if you choose to send them) diagnose problems |
| Recovery files (autosaves of open documents) | `%APPDATA%\AceStudio\recovery` | Restore work after a crash; removed when the document is saved or closed |
| Licence key (name, e-mail, edition, expiry as written in the key) | `%APPDATA%\AceStudio\licence.key` | Check the licence offline |
| Trial start date | `%APPDATA%\AceStudio\trial.json` and the registry value `HKCU\Software\AceGears\AceStudio\TrialStartMs` | Count the 14-day trial |
| Characters (names, prompts, notes, reference images you add) | `%APPDATA%\AceStudio\characters` | Reuse a character across AI generations |
| AI runtime, models, job files, MCP task results | `%LOCALAPPDATA%\AceStudio\ai`, `%APPDATA%\AceStudio\mcp-tasks` | Run local AI and keep results of AI-assistant tasks |
| Your projects and exports | Wherever you save them | — |

In portable mode (a file named `portable` next to `AceStudio.exe`) all of this is kept in a
`data` folder beside the program instead. Uninstalling AceStudio removes the program but keeps
this data so that reinstalling does not lose your work; delete the folders above to remove it.

The licence key is checked with a public key built into AceStudio. Checking it never contacts
a server.

## When AceStudio uses the network

Only on your request, from **AI › Set Up AI** / **AI › Models** (or `acestudio-cli ai setup` /
`ai download`):

| What | Where it connects | What is sent |
|---|---|---|
| Installing the AI runtime (Python packages) | `pypi.org` / `files.pythonhosted.org`, and `download.pytorch.org` for the GPU build | Standard package-download requests made by pip (package names, versions, your IP address, pip's user agent) |
| Downloading a model you accepted | The download address shown in the model's entry: `huggingface.co` or `github.com` | A normal file download with the user agent `AceStudio-ModelManager/1`; an access token only if you entered one for a gated model |

These sites have their own privacy policies. AceStudio sends them no project data, images or
prompts. Image generation, background removal, upscaling and all other AI features run locally;
while they run, AceStudio switches the Hugging Face and ONNX Runtime libraries to offline mode
and turns off their telemetry.

Other ways AceStudio may touch the network, all started by you:

- **AI assistants (MCP).** `acestudio-mcp` talks to the AI client you connect it to (for example
  Claude Code), over a local pipe, or over HTTP bound to `127.0.0.1` only. What the client then
  sends to its own service is governed by that client's privacy terms; AceStudio gives it only
  what its tools return (files inside the folders you allowed, renders, project data).
- **Links** in the program (Help, licence pages, python.org) open in your web browser.

## Children

AceStudio is not directed at children and does not knowingly collect personal data from
anyone; it collects none at all.

## Changes and contact

Changes to this document will be listed in the changelog shipped with each version.
Contact: _[publisher name and address — to be completed before release]_.
