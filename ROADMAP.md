# Roadmap

Each phase is complete only when every acceptance criterion passes an executed test (unit,
integration or recorded manual check on Windows). Status is updated as work lands.

Legend: ✅ done and tested · 🟡 partially done · ⬜ not started · 🔒 blocked

## Phase 1 — Architecture & Foundation · ✅

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
| CI: Linux core job and Windows MSVC + Qt job, green on GitHub Actions | ✅ Windows built against Qt 6.12.0 (since 2026-10-31) |
| Windows package runs without the VC++ redistributable | ✅ CRT DLLs shipped app-local; CI fails if any shipped binary imports a runtime DLL that is not packaged (a clean-PC launch is not yet tried) |
| Core/CLI/tests build for Windows and pass (MinGW cross-compile, run under Wine) | ✅ |
| App builds with MSVC on Windows, tests pass, packaged with windeployqt | ✅ CI |
| App launched and used interactively on a Windows desktop | ✅ Windows 11 PC, 2026-10-30/31: open, draw, undo, import, layers, AI setup, downloads, generation, background removal and upscaling driven through the UI (TESTING.md) |
| High-DPI check at 150%/200% scaling | ✅ 150% and 200% via `QT_SCALE_FACTOR` (Xvfb): icons crisp, no clipped or overlapping controls; the Windows PC runs it at its own scaling (TESTING.md) |

## Phase 2 — Core Editor · ✅

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

### 2c — delivered in 0.5.1
| Criterion | Status |
|---|---|
| Curve (cubic Bézier) and polygon tools with live preview | ✅ unit-tested; driven in the app |
| Clone stamp (aligned source, Alt-click) | ✅ unit-tested; driven in the app |
| Custom brushes: create from selection, import/export PNG, optional tip colours | ✅ unit-tested; created from a selection in the app |
| Inks: simple, blend (src-over), shading along the palette ramp (once per pixel per stroke) | ✅ unit-tested |
| Tiled drawing (wrap H/V/both) and seamless 3×3 preview | ✅ unit-tested; preview checked in the app |
| Layer masks (add/from selection/edit/disable/apply/delete), clipping masks, adjustment layers (5 types) with live preview; merge bakes them | ✅ compositor and undo unit-tested; every action driven in the app under AddressSanitizer |
| Selection in undo history, carried by canvas transforms; Move tool with Alt-copy and arrow nudge | ✅ unit-tested; driven in the app |
| Adobe .ase palettes (RGB/CMYK/LAB/Gray read, RGB write) | ✅ unit-tested with hand-built files |
| Project schema 2 (backwards compatible: schema 1 written when no new feature is used) | ✅ round-trip tests |
| Group duplication (whole subtree) | ✅ unit-tested |
| Large-canvas compositing | ✅ multi-threaded bands with identical output, 1024²×20 layers: 115 → 63 ms on 2 vCPUs (42 ms with normal blending); CPU kept by design (KNOWN_ISSUES) |
| Free Transform with handles (move, scale, rotate; one undo step; keeps off-canvas pixels) | ✅ core unit-tested; scale, rotate and undo driven in the app |
| Levels and Curves adjustment layers; mask-editing tint; Fit Canvas to Content; workspaces | ✅ unit-tested where core; curve drag, live preview, undo, tint and workspace save/apply driven in the app |
| Painting keeps cel pixels beyond the canvas | ✅ unit-tested (stroke, undo, redo) |

## Phase 3 — Animation · ✅ (delivered in 0.4.0; extended in 0.5.1)

| Criterion | Status |
|---|---|
| Frame insert / delete / duplicate / reorder (drag, Alt+←/→, context menu) with exact undo; durations travel with frames | ✅ unit-tested; drag and undo driven in the app |
| Multi-frame selection: duplicate, delete, move, drag and durations for a block, one undo step each; delete, durations and linking for selections with gaps | ✅ unit-tested; Shift/Ctrl-click, drag and undo driven in the app (0.5.1) |
| Linked cels (shared pixels across frames) kept in sync through painting, undo, frame and layer operations; saved once per group | ✅ unit-tested incl. file round trip; linked by context menu and painted through in the app |
| Per-frame durations; set all durations at once | ✅ |
| Tags: name, range, colour, forward/reverse/ping-pong, repeat count; validated and undoable; shown tinted in the timeline | ✅ tag created through the dialog in the app |
| Playback honours the tag under the playhead (direction, repeats) and per-frame durations | ✅ `AnimationPlayer` unit-tested; driven in the app |
| Onion skin: frames before/after (0–8), opacity, tint, wrap; persisted in settings; ghosts cached | ✅ unit-tested; checked visually |
| Preview dock (floatable) looping independently of the editing playhead, zoom 1–32×, checkerboard | ✅ |
| Sprite sheet export: horizontal, vertical, grid, packed; padding/spacing; trim; merge duplicates; power-of-two; scale; Aseprite JSON hash/array | ✅ every frame region compared to the composite; 2× trimmed grid exported from the app and checked |
| GIF, APNG, WebP export verified by re-decoding | ✅ exact round trip (APNG/WebP lossless incl. partial alpha; GIF exact for ≤255 colours); GIF also checked with Pillow |
| PNG sequence export | ✅ |
| CLI `export` for all formats | ✅ cli_smoke on Linux and the Windows build (Wine) |
| Export options remembered per kind | ✅ (0.5.1) |

## Phase 4 — AI Infrastructure · ✅ (verified on a Windows 11 PC with an RTX 4070 Laptop GPU, 2026-10-31)

| Criterion | Status |
|---|---|
| Supervised Python worker with crash recovery, tested by killing it mid-job | ✅ SIGKILL mid-job → job retried on a fresh worker and succeeds (unit test); real out-of-memory kill in the app → retried once, then failed with a memory hint, editor unaffected |
| Queue, progress, cancellation (cooperative, and forced for stuck jobs) | ✅ unit-tested against the real worker; queue UI in AI Studio |
| Model manager with licence display and approval-gated downloads | ✅ licence dialog with mandatory acceptance; resumable downloads verified by SHA-256; real 224 MB and 18 MB downloads from the official releases |
| Runtime setup (private Python environment, pinned packages, no system changes) | ✅ from the app and the CLI on Linux; Windows CI runs the real-worker supervision tests and the Python worker suite on Windows Python 3.14; installed from the app on a Windows 11 PC with an RTX 4070 Laptop GPU: `onnx-windows` in 29 s, `diffusion-cuda` (torch 2.14.1+cu126) in ~4 min |
| Text-to-image and image-to-image with SDXL on an NVIDIA GPU | ✅ real SDXL Base 1.0 + Pixel Art XL on an 8 GB RTX 4070 Laptop (model offload): ~34 s per 1024² image at 30 steps warm, ~50 s cold; pixel-art post-process to the canvas and palette. Plumbing (seeds, img2img, LoRA, cancel) also covered by the tiny-SDXL test. 12 GB cards not measured |
| BiRefNet background removal | ✅ CUDA provider on the RTX 4070 Laptop: 2.6 s per warm job (session cached), 7.6 s cold; DirectML failure falls back automatically (CPU ~16 s) |
| Real-ESRGAN upscaling | ✅ CPU (64→256 px in 6 s) and CUDA on the RTX 4070 Laptop: 512→2048 px in 3.1 s per job including model load |
| Nearest-neighbour upscaling | ✅ Image › Scale (core, since 0.3.0) |
| AI results: one undo step, placed on the frame the job started from; status shows where it ran and how long it took; LoRA strength and subject framing | ✅ (0.5.1) worker tests for framing and strength validation |
| Live generation preview (optional TAESD-XL), all-frame upscaling, CPU memory pre-check | ✅ preview through real Diffusers with a tiny random model; multi-frame upscale with a stub model; memory check fails in 5 s on a 6 GB container instead of being killed |
| Benchmarks recorded from real runs | ✅ CPU and RTX 4070 Laptop numbers for every model (TESTING.md); other GPUs are Phase 9's GPU matrix |

## Phase 5 — Advanced AI Generation · ✅ (verified with the real models on the RTX 4070 Laptop PC, 2026-10-31)

| Criterion | Status |
|---|---|
| Character profiles (prompt, negative, palette, up to 4 references, size, seed, strengths, facing) saved and reloaded | ✅ `.acechar` zip format with validation and round-trip tests; created, edited and saved in the Characters dialog on Windows |
| Editable pose skeleton and procedural cycles | ✅ 18-point skeleton; idle, walk, run and jump cycles; pose editor with presets, mirror and joint drag; unit tests for cycles, mirroring, framing and JSON |
| OpenPose ControlNet conditioning | ✅ xinsir SDXL OpenPose (Apache-2.0, 2.50 GB, SHA-256 verified) on the RTX 4070 Laptop: 8-frame walk, all frames follow the skeleton; ~48 s per finished frame including background removal (FP8 weight storage on <10 GB GPUs, see CHANGELOG); plumbing test with a tiny ControlNet shows the pose image changes the output |
| IP-Adapter reference conditioning | ✅ h94 IP-Adapter Plus SDXL + ViT-H encoder (Apache-2.0, 3.38 GB, SHA-256 verified): a 4-frame idle with one reference scored 95% consistency and kept the character's look (the walk without references scored 89%, but it moves more, so this is not a controlled comparison) |
| Pipeline generate → evaluate → correct → commit | ✅ on Windows: walk, run, idle and jump. Regenerate Flagged and Auto-Correct ran on real frames; Auto-Correct leaves frames it cannot fix without clipping, and those can then be regenerated. Commit: blank document from frame 1, otherwise appended with its own tag; one undo step |
| Automated consistency checks (palette drift, silhouette, ground line, height, colour mix) | ✅ Frames are judged against their own poses (ground, height, shape relative to how much the poses change). Also checked: facing, a palette-free colour profile and identity against the reference images. Calibrated on 21 real frames from the test PC; unit tests for each check |
| Review pass (2026-10-31) | ✅ cancel crash fixed. Cancel during loads, slow steps and mid-step verified. Run and jump on SDXL, FP8 vs Full measured, single instance, live preview in the dialog, duplicate poses share a job. See TESTING.md |
| Live generation preview | ✅ real TAESD-XL (MIT, 9.8 MB): a preview every 5 steps, ~0.9 s per step with previews on |

## Phase 6 — Environment & Asset Tools · ✅ (verified with real SDXL on the RTX 4070 Laptop PC, 2026-10-31)

| Criterion | Status |
|---|---|
| Tileset generator | ✅ Assets › Tileset Generator: terrain and base textures from the document, a file or SDXL with circular padding (tileable on both axes, verified by VAE torus equivariance and on the PC). Real run: "lush green grass" 72 s cold and "brown dirt ground" 41 s → both report "tiles seamlessly", 47-tile blob set with 0 seams in 1036 pairs that can touch |
| 4/8-neighbour, bitmask and Wang autotiling | ✅ Cardinal16 (4-bit), Blob47 (8-neighbour bitmask, 47 tiles) and Wang16 (corner/dual grid); exhaustive unit tests of masks and placement for every neighbourhood |
| Automated seam tests | ✅ `checkSeams` compares every legal pair of touching tiles, including the base fill; generated sets pass by construction (tested for all schemes, sizes and styles), broken sets are caught; Assets › Check Tileset Seams on any document |
| Terrain sandbox | ✅ paint/erase cells, brush size, map size, random terrain, live autotiling, map export |
| Parallax backgrounds | ✅ layers with scroll factor, position and repeat; animated preview; checks for seams (also outline steps in silhouettes), gaps and off-view layers; Make Seamless (dithered); Clear Sky; SDXL layers with horizontal wrap (43 s each on the PC); PNG + JSON export |
| UI kit generator with 9-slice metadata | ✅ 19 themed elements with exact 9-slice borders (unit-tested to be uniform), colours from the document palette, stretch test, atlas + JSON export |
| Smart slicer | ✅ connected sprites with merge distance and noise filter, or a detected / manual grid; JSON, PNG files, or a new animation document |
| Deterministic atlas packer | ✅ MaxRects, byte-identical output across runs, trim, duplicates, extrusion, padding, border, power of two, several pages, TexturePacker-compatible JSON; squarer pages preferred within 25% of the minimum area |

## Phase 7 — Developer Integrations · ⬜
CLI generation/export commands; Lua 5.4 API (sandboxed); MCP server (spec 2026-07-28 with
fallback) passing conformance tests; config templates for Claude Code, Cursor, VS Code, Codex;
Godot 4.x and Unity import verified in real engine projects.

## Phase 8 — Commercial Polish · ⬜
Performance profiling; accessibility; Inno Setup installer; licensing module; full docs;
security review; regression suite.

## Phase 9 — Release Candidate · ⬜
Clean-machine install tests; GPU matrix; recovery tests; export verification across engines.
