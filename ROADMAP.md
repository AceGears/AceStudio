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

## Phase 3 — Animation · ✅ (delivered in 0.4.0; real-Windows use pending as for Phase 1)

| Criterion | Status |
|---|---|
| Frame insert / delete / duplicate / reorder (drag, Alt+←/→, context menu) with exact undo; durations travel with frames | ✅ unit-tested; drag and undo driven in the app |
| Per-frame durations; set all durations at once | ✅ |
| Tags: name, range, colour, forward/reverse/ping-pong, repeat count; validated and undoable; shown tinted in the timeline | ✅ tag created through the dialog in the app |
| Playback honours the tag under the playhead (direction, repeats) and per-frame durations | ✅ `AnimationPlayer` unit-tested; driven in the app |
| Onion skin: frames before/after (0–8), opacity, tint, wrap; persisted in settings | ✅ unit-tested; checked visually |
| Preview dock (floatable) looping independently of the editing playhead, zoom 1–32× | ✅ |
| Sprite sheet export: horizontal, vertical, grid, packed; padding/spacing; trim; merge duplicates; power-of-two; scale; Aseprite JSON hash/array | ✅ every frame region compared to the composite; 2× trimmed grid exported from the app and checked |
| GIF, APNG, WebP export verified by re-decoding | ✅ exact round trip (APNG/WebP lossless incl. partial alpha; GIF exact for ≤255 colours); GIF also checked with Pillow |
| PNG sequence export | ✅ |
| CLI `export` for all formats | ✅ cli_smoke on Linux and the Windows build (Wine) |

## Phase 4 — AI Infrastructure · 🟡 (infrastructure done; GPU verification pending)

| Criterion | Status |
|---|---|
| Supervised Python worker with crash recovery, tested by killing it mid-job | ✅ SIGKILL mid-job → job retried on a fresh worker and succeeds (unit test); real out-of-memory kill in the app → retried once, then failed with a memory hint, editor unaffected |
| Queue, progress, cancellation (cooperative, and forced for stuck jobs) | ✅ unit-tested against the real worker; queue UI in AI Studio |
| Model manager with licence display and approval-gated downloads | ✅ licence dialog with mandatory acceptance; resumable downloads verified by SHA-256; real 224 MB and 18 MB downloads from the official releases |
| Runtime setup (private Python environment, pinned packages, no system changes) | ✅ from the app and the CLI on Linux; Windows (`onnx-windows`, `diffusion-cuda`) not yet run |
| Text-to-image and image-to-image with SDXL on a 12 GB NVIDIA GPU | 🟡 code path verified end to end with real Diffusers 0.42 on a tiny random SDXL (seeds, img2img, LoRA hook, cancel, pixel-art post-process); real SDXL weights and GPU not available here |
| BiRefNet background removal | 🟡 implemented; BiRefNet Lite needs more than 6 GB RAM on CPU and was OOM-killed on the 7 GB test VM. Must be verified on a GPU machine |
| Real-ESRGAN upscaling | ✅ official x4plus and anime weights run on CPU (64→256 px in 6 s) |
| Nearest-neighbour upscaling | ✅ Image › Scale (core, since 0.3.0) |
| Benchmarks recorded from real runs | 🟡 CPU numbers recorded (TESTING.md); GPU benchmarks need the target machine |

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
