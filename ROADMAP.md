# Roadmap

Each phase is complete only when every acceptance criterion passes an executed test (unit,
integration or recorded manual check on Windows). Status is updated as work lands.

Legend: ✅ done and tested · 🟡 partially done · ⬜ not started · 🔒 blocked

## Phase 1 — Architecture & Foundation · 🟡 (done except real-Windows verification)

| Criterion | Status |
|---|---|
| Research summary and ADRs written (`docs/research/`, `ARCHITECTURE.md`) | ✅ |
| License audit and SBOM (`LICENSE_AUDIT.md`), GPL-only Qt modules blocked at configure time | ✅ |
| CMake build; core compiles without Qt; strict warnings clean (GCC) | ✅ |
| Logging with rotation and in-app viewer | ✅ |
| Typed, versioned settings with corruption quarantine | ✅ |
| Versioned `.acestudio` format, atomic save with backup, corruption detection | ✅ |
| Qt desktop shell: docks, menus, themes, welcome page, recent files, layout persistence | ✅ (Linux/Xvfb) |
| `acestudio-cli` (version/new/info/validate/render) with JSON output and exit codes | ✅ |
| CI: Linux core job and Windows MSVC + Qt job, green on GitHub Actions | ✅ (Windows built against Qt 6.8.3; see KNOWN_ISSUES) |
| Core/CLI/tests build for Windows and pass (MinGW cross-compile, run under Wine) | ✅ |
| App builds with MSVC on Windows, tests pass, packaged with windeployqt | ✅ CI |
| App launched and used interactively on a Windows desktop | ⬜ needs a Windows machine |
| High-DPI check at 150%/200% scaling on Windows | ⬜ |

## Phase 2 — Core Editor · 🟡 (2a and 2b done; 2c remaining)

### 2a — delivered in 0.2.0
| Criterion | Status |
|---|---|
| Pencil, eraser, line, rectangle, ellipse (outline/filled), paint bucket (contiguous/global, tolerance), eyedropper (merged/layer), rectangle select, hand | ✅ unit-tested and driven in the running app |
| Pixel-perfect freehand, brush size 1–64, square/circle brush, mirror L↔R / T↕B / both | ✅ |
| Selection clipping, alpha lock, layer lock, hidden-layer protection | ✅ |
| Indexed (nearest palette colour) and grayscale constraints while painting | ✅ |
| Undo/redo for every document mutation, exact round-trip tested; merge of slider drags; memory/step limits; clean-state tracking | ✅ |
| Layer UI: add, group, rename, reorder, duplicate, merge down, delete, visibility, opacity, blend, locks | ✅ |
| Frame UI: insert, duplicate, delete, duration | ✅ |
| Palette: pick primary/secondary, edit, add, remove | ✅ |
| PNG import (as project or layer) and frame export | ✅ |
| Autosave to recovery files + recovery prompt after a crash | ✅ tested by `kill -9` of the running app |
| Isolated group compositing (W3C semantics) | ✅ |
| Canvas performance: 256×256, 20 layers | ✅ full composite 9.2 ms; stroke updates recomposite only the dirty region (~0.2 ms). Measured on a 2-vCPU Xeon VM, CPU only. Real-display FPS on Windows not yet measured |

### 2b — delivered in 0.3.0
| Criterion | Status |
|---|---|
| Canvas size (anchor), crop to selection, flip/rotate canvas — exact undo | ✅ |
| Layer flip/rotate 90°/scale (nearest)/RotSprite at any angle, selection-aware, current or all frames | ✅ RotSprite verified to add no colours |
| Outline generation (outside/inside, 4/8-neighbour) | ✅ |
| Lasso and magic wand; selection add / subtract / intersect (Shift / Alt / Shift+Alt); invert | ✅ driven in the app |
| Gradient tool: linear/radial, ordered-dither (2-colour) or smooth | ✅ |
| Dither-density brush (Bayer 4×4) | ✅ |
| Palette import/export: .gpl, .pal (JASC), .hex, .txt (Paint.NET); palette from image (median cut) | ✅ |
| Colour-mode conversion RGBA / indexed (nearest or ordered dither) / grayscale | ✅ |
| Customisable keyboard shortcuts with conflict detection, persisted in settings | ✅ driven in the app |

### 2c — remaining
Curve and polygon tools; clone tool; custom brushes (create/import/export); shading mode;
tiled drawing and seamless preview; layer masks, clipping masks and adjustment layers; Adobe
.ase palettes; moving/transforming a floating selection interactively; GPU compositing for
large HD canvases.

## Phase 3 — Animation · ⬜
Acceptance: frame insert/delete/duplicate/reorder/drag; per-frame durations; tags with
direction; onion skin (configurable range/opacity); floating preview window; sprite-sheet
export (grid/packed) with Aseprite-compatible JSON; GIF, APNG, WebP export verified by
re-decoding.

## Phase 4 — AI Infrastructure · ⬜
Acceptance: supervised Python worker with crash recovery (tested by killing it mid-job);
model manager with license display and approval-gated downloads; text-to-image and
image-to-image with SDXL on a 12 GB NVIDIA GPU; BiRefNet background removal; Real-ESRGAN and
nearest-neighbour upscaling; queue, progress, cancellation; benchmarks recorded from real runs.

## Phase 5 — Advanced AI Generation · ⬜
Character profiles; IP-Adapter reference conditioning; OpenPose ControlNet with an editable
skeleton; animation generation pipeline (generate → evaluate → correct → commit) with
automated consistency checks (palette drift, silhouette, ground line).

## Phase 6 — Environment & Asset Tools · ⬜
Tileset generator; 4/8-neighbour, bitmask and Wang autotiling with automated seam tests;
terrain sandbox; parallax backgrounds; UI kit generator with 9-slice metadata; smart slicer;
deterministic atlas packer.

## Phase 7 — Developer Integrations · ⬜
CLI generation/export commands; Lua 5.4 API (sandboxed); MCP server (spec 2026-07-28 with
fallback) passing conformance tests; config templates for Claude Code, Cursor, VS Code, Codex;
Godot 4.x and Unity import verified in real engine projects.

## Phase 8 — Commercial Polish · ⬜
Performance profiling; accessibility; Inno Setup installer; licensing module; full docs;
security review; regression suite.

## Phase 9 — Release Candidate · ⬜
Clean-machine install tests; GPU matrix; recovery tests; export verification across engines.
