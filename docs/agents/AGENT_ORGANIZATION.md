# AceStudio Agent Organization

28 project subagents live in `.claude/agents/`. Claude Code version this was written for: **2.1.319**. Frontmatter fields used: `name`, `description`, `tools`, `model`, `color`. All of them are long-standing supported fields.

## How to run
- **Lead as main session:** `claude --agent acestudio-chief-architect`. Only the main session can spawn subagents (subagents cannot spawn subagents).
- From any normal session you can call a specialist by name, e.g. *"use the security engineer agent to audit ai_worker"*.
- To make the chief architect the default for this folder, add `"agent": "acestudio-chief-architect"` to `.claude/settings.json`. It is not set by default.
- Project agents load from the `.claude/agents/` folder of the directory Claude Code is started in. To use them on the AceStudio source tree, start Claude Code in that tree or copy `.claude/` there (see `README.md`).

## Roster and ownership
Paths in the *Owns* column below are the generic module names the agent prompts use. In this repository they map to real files as follows (authoritative):

| Generic path | Real files |
|---|---|
| `app/shell/`, `app/platform/` | `app/src/main.cpp`, `SingleInstance.*`, `AppContext.*`, `core/src/Subprocess.cpp`, `core/src/AiWorker.cpp` (supervision), `core/src/Recovery.cpp` |
| `app/core/`, `app/services/` | `core/src/Document.cpp`, `Editor.cpp`, `UndoStack.cpp`, `Settings.cpp`, `ResourceProfile.cpp`, `Log.cpp`, `app/src/ai/AiService.*` |
| `app/ui/` | `app/src/{MainWindow,Theme,Shortcuts,Accessibility,*Dialog*}.*`, `app/src/{pages,panels,ui}/` |
| `app/render/` | `app/src/CanvasWidget.*`, `DocumentView.*`, `panels/PreviewPanel.*`, `core/src/{Raster,BlendMode,Transform,Image}.cpp` |
| `app/project/`, `app/assets/` | `core/src/{ProjectIO,AssetFiles,FileUtil,PngCodec,AnimCodec}.cpp`, `docs/project-format.md` |
| `app/anim/rig/` | `core/src/PixelRig.*`, `Pose.cpp`, `app/src/ai/PoseEditor.*` |
| `app/anim/creatures/*` | `core/src/SpriteMotion.cpp`, `Animation.cpp`, `AnimRouter.cpp`, `KeyPoses.cpp`, `Character.cpp` (dragon, aquatic and locomotion specialists coordinate per function) |
| `app/fx/` | `core/src/Effects.cpp` |
| `tools/anim_qa/`, `app/anim/repair/` | `core/src/AnimationQA.cpp`, `.claude/skills/animation-quality/scripts/` |
| `app/world/` | `core/src/{Autotile,WorldMap,AssetGen}.cpp`, `app/src/pages/WorldBuilderPage.*`, `app/src/assets/` |
| `app/export/` | `core/src/{Export,EngineExport,Atlas}.cpp`, `tests/engine/` |
| `mcp/` | `mcp/`, `app/src/scripting/AppBridgeServer.*`, `core/src/AppBridge.cpp` |
| CLI / Lua | `cli/`, `core/src/Script.cpp`, `app/src/scripting/ScriptDialog.*`, `docs/lua-api.md` |
| worker | `ai_worker/acestudio_worker/**`, `ai_worker/catalog/`, `ai_worker/requirements/`, `core/src/ModelCatalog.cpp` |
| `tools/bench/` | `bench/`, `app/src/ai/{Benchmark,SystemMonitor,PerformanceStrip}.*` |
| installer / build | `CMakeLists.txt`, `CMakePresets.json`, `cmake/`, `packaging/`, `.github/workflows/` |

| # | Agent file | Division | Model | Write access | Owns |
|---|---|---|---|---|---|
| 01 | `acestudio-chief-architect` | Leadership | opus | full + Agent | CLAUDE.md, ARCHITECTURE, DECISIONS, ROADMAP, integration glue |
| 02 | `windows-systems-engineer` | Core | inherit | code | `app/shell/`, `app/platform/`, worker supervisor, manifests |
| 03 | `backend-architect` | Core | inherit | code | `app/core/`, `app/services/`, job system, IPC schema, settings store |
| 04 | `ui-ux-director` | Core | inherit | code | `app/ui/`, `styles/`, DESIGN_SYSTEM |
| 05 | `graphics-engineer` | Core | inherit | code | `app/render/`, shaders, preview, render goldens |
| 06 | `asset-pipeline-engineer` | Core | inherit | code | `app/project/`, `app/assets/`, PROJECT_FORMAT, migrations |
| 07 | `ai-research-engineer` | AI | opus | **docs only** | AI_MODEL_STRATEGY, `docs/evals/`, `tests/ai_eval/` |
| 08 | `inference-optimizer` | AI | inherit | code | `ai_worker/**` runtime, catalog, requirements |
| 09 | `generative-art-engineer` | AI | inherit | code | recipe/post-process parts of `tasks/generate.py`, `imaging.py` |
| 10 | `reference-consistency-engineer` | AI | inherit | code | worker reference/style modules, `tools/identity/` |
| 11 | `cloud-gpu-engineer` | AI | inherit | code (no paid actions) | `cloud/`, CLOUD_ARCHITECTURE |
| 12 | `animation-director` | Animation | opus | **docs only** + Agent | ANIMATION_STANDARDS, `docs/reviews/animation/` |
| 13 | `rigging-engineer` | Animation | inherit | code | `app/anim/rig/`, rig templates |
| 14 | `dragon-animation-specialist` | Animation | inherit | code | `app/anim/creatures/flying/` |
| 15 | `aquatic-animation-specialist` | Animation | inherit | code | `app/anim/creatures/aquatic/` |
| 16 | `character-motion-specialist` | Animation | inherit | code | `app/anim/creatures/locomotion/` |
| 17 | `environmental-effects-specialist` | Animation | inherit | code | `app/fx/` |
| 18 | `temporal-consistency-engineer` | Animation | inherit | code | `tools/anim_qa/`, animation-quality scripts, `app/anim/repair/` |
| 19 | `pixel-art-director` | Game assets | inherit | **docs only** | PIXEL_ART_STANDARDS, `docs/reviews/pixel-art/` |
| 20 | `world-generation-engineer` | Game assets | inherit | code | `app/world/`, worker tileset tasks |
| 21 | `sprite-export-engineer` | Game assets | inherit | code | `app/export/`, `tests/engines/` |
| 22 | `mcp-integration-engineer` | Integration | inherit | code | `mcp/`, MCP_INTEGRATION |
| 23 | `scripting-plugin-engineer` | Integration | inherit | code | CLI, Lua scripting, plugin loader, CLI/SCRIPTING docs |
| 24 | `cybersecurity-engineer` | Sec/Perf/Release | opus | **docs + security tests** | SECURITY, PRIVACY, `docs/security/`, `tests/security/` |
| 25 | `performance-engineer` | Sec/Perf/Release | inherit | code (by contract) | `tools/bench/`, resource modes, PERFORMANCE_BASELINE |
| 26 | `qa-engineer` | Sec/Perf/Release | inherit | **tests only** | `tests/**`, TESTING, `docs/reviews/qa/` |
| 27 | `release-engineer` | Sec/Perf/Release | inherit | code | build scripts, `installer/`, RELEASE notes |
| 28 | `documentation-engineer` | Sec/Perf/Release | inherit | docs | `docs/**`, README |

Read-only and review agents (07, 12, 19, 24, 26) have no `Edit` tool, or are limited to docs/tests by their instructions. No agent has unrestricted authority over cloud resources, publishing, or deletion. Those actions always go back to the user.

## Reviewers with veto
| Domain | Reviewer | Veto on |
|---|---|---|
| Motion | `animation-director` | any animation output |
| Pixel art | `pixel-art-director` | any pixel-art output |
| Security | `cybersecurity-engineer` | network, IPC, MCP, plugins, credentials, downloads |
| Gates | `qa-engineer` | merge of critical changes, release candidates |

## Routing (minimum sets)
| Problem | Activate (in order) |
|---|---|
| Dragon wings flat/broken | animation-director → rigging-engineer → dragon-animation-specialist → graphics-engineer (z-order) → temporal-consistency-engineer → qa-engineer |
| Fire looks artificial | environmental-effects-specialist → pixel-art-director → graphics-engineer → performance-engineer → qa-engineer |
| Fish swimming/turning | animation-director → rigging-engineer → aquatic-animation-specialist (+ reference-consistency-engineer for views) → temporal-consistency-engineer → qa-engineer |
| Walk distortion | animation-director → rigging-engineer → character-motion-specialist → temporal-consistency-engineer → qa-engineer |
| Generation slow | performance-engineer (profile) → inference-optimizer ∥ ai-research-engineer → cloud-gpu-engineer (if remote) → qa-engineer |
| Premium UI | ui-ux-director → windows-systems-engineer → graphics-engineer → qa-engineer |
| Security audit | cybersecurity-engineer → backend-architect, cloud-gpu-engineer, mcp-integration-engineer (fix owners) → qa-engineer |
| Export broken | sprite-export-engineer → pixel-art-director → qa-engineer |
| Data loss/corrupt project | asset-pipeline-engineer → backend-architect → qa-engineer |
| Release | qa-engineer → cybersecurity-engineer → release-engineer → documentation-engineer |

`∥` = may run in parallel (independent, read-only, or disjoint files). Everything else is serialized. The rig core must land before species work.

## Collaboration rules
1. One owner per file per task. Changes outside your ownership go to the owner as a request.
2. Interface changes are announced in a handoff before implementation (`backend-architect` for services/IPC, `rigging-engineer` for rig API, `asset-pipeline-engineer` for format).
3. Before a substantial merge: interfaces compatible → tests run → perf impact checked → security impact checked → conflicts resolved → docs updated.
4. Check `KNOWN_ISSUES.md`, `ARCHITECTURE.md §5 (ADRs)` and prior audits before starting, so nobody repeats a failed approach.
5. Hardware: at most one GPU-heavy job at a time on the local PC. Release models after tests. Never run paid cloud resources without approval.

## Handoffs
Every task contract and every report uses this structure:
```
Objective:
Owner / reviewers:
Files (owned / read-only):
Constraints:
Findings:
Changes made:
Tests executed (command → result):
Not verified / skipped (why):
Known issues:
Next steps:
```

## Escalation ladder
Specialist → owning reviewer (animation/pixel/security/QA) → `acestudio-chief-architect` → **user**. The user decides on paid services, publishing, deleting data, removing features, incompatible format changes, and accepting failed gates.
