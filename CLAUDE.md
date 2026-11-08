# CLAUDE.md — AceStudio

## Project identity
- **AceStudio**: AI-assisted game asset creation and 2D animation studio for Windows 10/11 x64. Personal use only, with no account, licence key, trial or activation (licensing was removed in `3488cab`, ADR-008 withdrawn).
- **Stack:** C++20 + Qt 6 Widgets (LGPLv3, dynamic linking); a Qt-free `core/` shared by the app, `cli/` and `mcp/`; Lua 5.4 + sol2 scripting; a Python AI worker (`ai_worker/`, JSON-RPC over stdio); an Inno Setup per-user installer with bundled Python. See `ARCHITECTURE.md`.
- **Hardware (measured 2026-11-07):** RTX 4070 Laptop GPU, **8 GB VRAM**, Core Ultra 9 185H, 31.5 GB RAM. Budget AI work for ≤ 7 GB VRAM peak.
- **Version:** 0.10.0 (`CMakeLists.txt`).

## Layout and ownership
| Path | What | Owner agent |
|---|---|---|
| `core/src`, `core/include/acestudio/core` | document, raster, animation (`Animation`, `SpriteMotion`, `PixelRig`, `KeyPoses`, `AnimRouter`, `AnimationQA`), effects, export, autotile, project IO, settings, worker client | per module, see `docs/agents/AGENT_ORGANIZATION.md` |
| `app/src` | Qt GUI (pages, panels, ai, assets, scripting bridge, ui) | ui-ux-director, windows-systems-engineer |
| `cli/`, `mcp/` | CLI and MCP server | scripting-plugin-engineer, mcp-integration-engineer |
| `ai_worker/` | Python worker, catalog, requirements, tests | inference-optimizer (+ generative-art, reference-consistency) |
| `tests/`, `bench/` | Unit, golden, fuzz, engine, MCP tests; performance budgets | qa-engineer, performance-engineer |
| `packaging/`, `.github/workflows` | installer, Python bundling, CI (builds the nightly pre-release) | release-engineer |

## Build and test
- Core + CLI + tests (no Qt): `cmake --preset core-only && cmake --build --preset core-only && ctest --preset core-only`
- Full Windows app: `cmake --preset windows-release && cmake --build --preset windows-release && ctest --preset windows-release` (needs MSVC, Ninja and Qt 6.8+).
- Worker tests: `python -m unittest discover -s ai_worker/tests`
- **This PC has no CMake, MSVC or Qt installed.** C++ changes are verified by CI (`.github/workflows/ci.yml`, Windows job, which publishes a `nightly-<sha>` pre-release). Say "CI-verified" or "not built locally". Never "tested" without a run.

## Engineering standards
Correctness → security/data integrity → stability → animation/art quality → performance → UX polish → maintainability.
- Inspect before changing; smallest root-cause fix; preserve working features; one implementation shared by GUI, CLI, Lua and MCP.
- Explicit errors (`ARCHITECTURE.md` §6), cancellation and progress for long jobs, nothing slow on the UI thread.
- New dependencies need a licence check (`LICENSE_AUDIT.md`) and an ADR in `ARCHITECTURE.md` §5.
- **No AI slop:** no placeholders, fake features, demo outputs presented as real, dead buttons, or "done" without evidence.
- Keep `CHANGELOG.md`, `KNOWN_ISSUES.md`, `ROADMAP.md`, `SECURITY.md`, `PRIVACY.md` and `TESTING.md` accurate. No secrets anywhere.

## Agent organization
28 agents in `.claude/agents/`, with skills in `.claude/skills/`. Roster, routing and handoff format are in `docs/agents/AGENT_ORGANIZATION.md`.
- Lead: `claude --agent acestudio-chief-architect` (only the main session can spawn subagents).
- Activate the minimum set of specialists. Serialize dependent work.
- Veto reviewers: animation-director, pixel-art-director, cybersecurity-engineer, qa-engineer.
- Ask the user before pushing, publishing, paid cloud use, deleting user data, removing features, or making incompatible format changes.

## Animation and AI standards
Animation: see `docs/agents/ANIMATION_STANDARDS.md` (anatomy constant, parts attached, articulated not warped, depth-correct, turns not flips, seamless loops, palette and grid preserved). Check with the `animation-quality` skill; `core/src/AnimationQA.cpp` is the in-app counterpart.
AI: see `docs/agents/AI_MODEL_STRATEGY.md`. Best model per task, licences verified, safetensors + SHA-256, ≤ 7 GB VRAM, no silent quality-for-speed trades, Cloud Mode opt-in only.

## Evidence
Report *implemented* vs *verified*, and list failed and skipped checks. Latest audit: `docs/agents/AUDIT_2026-11-07.md`. Its animation verdicts were made on outputs from builds older than `fa23f09`/`0f15b26` and must be re-checked on `main`.
