---
name: windows-release
description: Build, package, install-test, upgrade-test and uninstall-test an AceStudio Windows release (exe installer + portable zip + checksums) in a clean environment, and record evidence against the release gates. Use when preparing any release candidate. Never publishes without user approval.
---

# Windows Release

## 1. Preconditions
- `qa-engineer` gate verdicts for the candidate; `cybersecurity-engineer` sign-off; release notes from `documentation-engineer` listing only verified changes.
- Version bumped in the single version source; changelog entry.

## 2. Build (record exact commands and tool versions)
1. Configure + build Release (MSVC x64).
2. `windeployqt --release --no-translations <AceStudio.exe>` (verify the plugin folders: platforms, styles, imageformats, iconengines, tls, networkinformation, generic).
3. Bundle the worker (`ai_worker/`) without `__pycache__`; runtime install path documented.
4. Embed version resource + official AceStudio icon.
5. Build installer (per-user, no admin by default) and portable zip (`portable` marker file).
6. `Get-FileHash -Algorithm SHA256` for every artifact → `SHA256SUMS.txt`.

## 3. Clean-machine tests (Windows Sandbox: `WindowsSandbox.exe` with a `.wsb` mapping the artifacts read-only)
| Gate | Pass condition |
|---|---|
| Install | completes, no admin prompt (per-user), shortcut + file association created |
| Launch | main window < 2 s, no missing-DLL errors |
| Core workflow | open sample project → animate preview → export sheet + JSON |
| Upgrade | install over previous version: settings, recent projects, recovery and models preserved |
| Uninstall | program files removed; `%APPDATA%\AceStudio`, user projects and models preserved unless opted in |
| Leftovers | no running processes, no orphaned `python.exe`, no stray services/scheduled tasks |
Record each result with screenshots/log excerpts in `docs/releases/<version>.md`.

## 4. Publish (only with explicit user approval)
Ask in chat with the artifact list, sizes and checksums. Do not upload, tag, or create GitHub releases otherwise.
