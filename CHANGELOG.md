# Changelog

All notable changes are documented here. Format follows Keep a Changelog; versions follow SemVer.

## [0.5.0] — 2026-10-30 — Phase 4 AI infrastructure (unreleased)

### Added
- **AI Studio** dock and menu: Generate (SDXL text-to-image / image-to-image, optional Pixel
  Art XL LoRA, pixel-art mode that downsamples to the canvas and limits to the document
  palette), Remove Background (BiRefNet, optional hard edges), Upscale ×4 (Real-ESRGAN). Results
  arrive as new layers or new documents; existing pixels are never overwritten.
- Job queue with progress, cancel and error details.
- **Set Up AI Runtime**: creates a private Python environment and installs pinned package
  profiles (ONNX for background removal; PyTorch + Diffusers with CUDA or CPU).
- **Models** dialog: licence, size and install state of every catalogued model; downloads
  require accepting the licence, resume after interruption and are verified with SHA-256.
- `acestudio-cli ai status | setup | models | download | remove-bg | upscale | generate`.
- Core: `Subprocess` (Windows job objects, POSIX pipes), `WorkerSupervisor` (handshake,
  queue, cancel, crash restart policy), `ModelCatalog`/`ModelStore`, `userLocalDataDir`,
  `executableDir`.
- Python worker (`ai_worker/`): protocol server, tasks, own RRDBNet implementation for the
  official Real-ESRGAN weights, test suite.

### Added (verified on Windows with an RTX 4070 Laptop GPU)
- NVIDIA driver detection; with R580+ drivers the CUDA profile installs PyTorch 2.14.1+cu130 and
  swaps ONNX Runtime to onnxruntime-gpu 1.31.1, so BiRefNet runs on CUDA (2.6 s warm vs ~16 s
  on the CPU). Older drivers keep PyTorch cu126.
- ONNX sessions are cached in the worker; each ONNX job logs its provider and session/run
  times; every job's wall time is logged when it finishes.
- SDXL Base 1.0 and Pixel Art XL files pinned to Hugging Face commits with verified sizes and
  SHA-256 for the weights.

### Fixed
- "Pixel art for this canvas" was unticked whenever a session started without a document.
- Tab in the prompt box typed a tab character instead of moving to the next field.
- Windows: the AI worker's device check ("checking GPU…") could hang indefinitely. Any query
  of the protocol stdin handle from another thread (library imports on the probe thread)
  waits behind the main thread's blocking read. The worker now reads the protocol from a
  private descriptor and points fd 0 at the null device.
- The CUDA runtime profile silently installed the CPU-only PyTorch from PyPI (no torch
  2.15.0 build exists on the cu128 index). Pinned to `torch==2.14.1+cu126` (the local label
  forces the CUDA build or a clear failure), and the install now checks that the imported
  torch reports a CUDA version.
- Log panel follows new lines even when it was hidden behind another tab when they arrived.
- AI Studio: the Generate / Remove Background / Upscale buttons sit below the tab pages instead
  of inside their scroll areas, so they stay visible on short screens.
- Windows: AI runtime installs hung with no output. pip inherited the worker's protocol stdin,
  which the worker's main thread is blocked reading, and stalled at startup. pip now gets an
  empty stdin.
- ONNX models (BiRefNet, ONNX upscalers) fall back when the GPU provider fails at run time:
  fewer graph fusions first, then the CPU; the working choice is remembered for the worker's
  lifetime and logged as a warning. DirectML sessions now use the settings its documentation
  requires (no memory pattern, sequential execution).
- 32-bit Python is detected: Set Up skips it in favour of a 64-bit interpreter and, if it is the
  only one, says the 64-bit build is needed instead of starting an install that cannot succeed.
- Log lines below warning level could stay in the buffer indefinitely; the log now flushes on
  warnings and at least once a second.

## [0.4.0] — 2026-10-29 — Phase 3 animation (unreleased)

### Added
- Timeline: drag frames to reorder (or Alt+← / Alt+→, or the right-click menu); set the
  duration of one frame or all frames; tag ranges tinted in their colour; Tags… button.
- Animation › Tags (Ctrl+Alt+T): add, edit and remove tags (name, range, colour, direction,
  repeat). All tag edits are undoable.
- Playback follows the tag under the playhead: forward, reverse, ping-pong, finite repeats.
- Onion skin (F3) with configurable frames before/after, opacity, red/blue tint and wrap
  (Animation › Onion Skin Settings…), remembered across sessions.
- Preview dock: loops the tag at the playhead, all frames or a chosen tag at 1–32× zoom while
  you keep editing.
- File › Export Animation: Sprite Sheet (layouts, padding, spacing, trim, merge duplicates,
  power-of-two, scale, Aseprite JSON hash/array), Animated GIF, Animated PNG, Animated WebP,
  PNG Sequence. Asks before replacing any existing file.
- CLI `export` command for every format (`--format sheet|gif|apng|webp|png`).
- Core: `AnimationPlayer`, `renderOnionSkin`, `buildSpriteSheet`, `spriteSheetJson`, GIF
  encoder (own LZW) and decoder, APNG encoder/decoder, WebP via libwebp 1.6.0,
  `writeSpriteSheet`, `writeAnimation`, `exportPngSequence`.

## [0.3.0] — 2026-10-29 — Phase 2b (unreleased)

### Added
- Image menu: canvas size with anchor, crop to selection, flip/rotate canvas; flip, rotate 90°,
  RotSprite rotation (any angle) and nearest-neighbour scale for the current layer, honouring
  the selection and optionally all frames; outline generation.
- Tools: lasso select (Q), magic wand (W), gradient (D) — linear/radial, ordered-dither or
  smooth; brush dither density.
- Selections are now arbitrary masks with add/subtract/intersect (Shift/Alt/Shift+Alt) and
  Invert Selection (Ctrl+Shift+I); outline drawn along pixel edges.
- Palette Load/Save (.gpl, .pal, .hex, .txt) and "From Image" (median cut).
- Colour mode conversion: RGBA, indexed (nearest or ordered dither), grayscale.
- Edit › Keyboard Shortcuts: every menu action can be rebound; conflicts are refused;
  overrides persist in settings. Startup logs any clashing shortcuts.

### Fixed
- Shift+letter shortcuts could trigger the plain-letter tool (Shift+G opened the paint bucket);
  gradient moved to D and layer flips to Ctrl+Alt+H / Ctrl+Alt+V.
- Windows: files open in another program could not be read (found by the first Windows CI run).

## [0.2.0] — 2026-10-29 — Phase 2a core editor (unreleased)

### Added
- Undo/redo for every edit (`UndoStack`, `Editor`): exact round-trip, slider-drag merging,
  500-step / 512 MiB limits, clean-state tracking for the modified indicator.
- Drawing (`Raster`, `Paint`): pencil with pixel-perfect mode, eraser, line, rectangle and
  ellipse (outline/filled, Shift = square/circle), paint bucket (contiguous/global, tolerance),
  eyedropper (Alt for temporary), rectangle selection, brush size/shape, mirror painting,
  alpha lock, indexed/grayscale colour constraints. Each stroke is one undo step storing only
  the changed rectangle.
- Layer editing: add, group, rename, reorder, duplicate, merge down, delete, visibility,
  opacity, blend mode, lock, transparency lock.
- Frame editing: insert, duplicate, delete, duration.
- Palette editing and colour picking (left = primary, right = secondary).
- PNG import (new project or new layer) and current-frame PNG export.
- Autosave to recovery files and crash-recovery prompt on next launch.
- Tools dock, Edit/Layer/Tools menus, single-key tool shortcuts, `X` swap colours, `[`/`]` brush size.
- `acestudio_bench` micro-benchmarks.

### Changed
- Groups now composite in isolation (W3C group semantics) instead of multiplying opacity.
- Integer fast path for normal blending; canvas recomposites only the dirty region while painting.

### Fixed
- Welcome page version text was nearly invisible in the dark theme.

## [0.1.0] — 2026-10-29 — Phase 1 foundation (unreleased)

### Added
- `acestudio_core` (Qt-free): RGBA8 images, W3C blend modes, palettes (≤256), document model
  with layers, groups, frames, cels, animation tags and extension metadata; flattening.
- `.acestudio` project format v1: ZIP + JSON + PNG cels, CRC verification, size limits,
  schema versioning, verify-before-write, atomic save with `.bak` backup.
- Atomic file writes with flush-to-disk on Windows and POSIX.
- Typed, versioned settings with corrupt-file quarantine and newer-schema protection.
- Rotating, thread-safe logger with live listener.
- `acestudio-cli`: `version`, `new`, `info`, `validate`, `render`; `--json` output; documented
  exit codes; never overwrites without `--force`.
- Qt 6 desktop shell: welcome page, multi-document tabs, dockable Layers/Properties/Palette/
  Timeline/Log panels, dark/light themes, pixel-exact canvas with integer zoom, grid,
  cursor-anchored zoom and panning, frame playback honouring per-frame durations, recent
  projects, persisted layout, drag-and-drop open, unsaved-changes prompts.
- Build: CMake ≥ 3.25, pinned dependencies, strict warnings, CMake presets, Qt license guard.
- Tests: 39 Catch2 unit tests + 1 CLI end-to-end test.
- CI workflow for Linux (core) and Windows (MSVC + Qt 6.12).
- Docs: architecture/ADRs, project format spec, research notes, license audit, roadmap.
