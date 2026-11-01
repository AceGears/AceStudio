# AceStudio Architecture

Status: 0.8.1 (Phase 7: developer integrations). This document records the structure that exists today and the
decisions (ADRs) that constrain future phases. Planned components are labelled **planned**.

## 1. Shape of the system

```
                    ┌────────────────────────────────────────────┐
                    │                acestudio_core              │  C++20, no Qt
                    │  Image · Color · BlendMode · Palette       │
                    │  Document (layers, frames, cels, tags)     │
                    │  ProjectIO (.acestudio) · PngCodec         │
                    │  Editor · UndoStack · Raster · Paint       │
                    │  Recovery · Settings · Log · FileUtil      │
                    └───────▲──────────────▲──────────────▲──────┘
                            │              │              │
          ┌─────────────────┴──┐   ┌───────┴───────┐  ┌───┴─────────────────┐
          │ AceStudio (Qt 6    │   │ acestudio-cli │◄─┤ acestudio-mcp       │
          │ Widgets desktop)   │   │ (headless)    │  │ (MCP, Phase 7)      │
          └─────────┬────▲─────┘   └───────────────┘  └──────────┬──────────┘
                    │    └──── local bridge (named pipe) ────────┘
                    │ local IPC (planned, Phase 4)
          ┌─────────▼────────────────────────────────┐
          │ AI worker process (Phase 4)               │  Python: PyTorch/Diffusers,
          │ separate process; a crash cannot take     │  ONNX Runtime
          │ down the editor                            │
          └───────────────────────────────────────────┘
```

Every front end (desktop, CLI, MCP, Lua) drives the **same core services**; there is no
duplicate implementation of document logic, file formats or export in any front end.

## 2. Repository layout

| Path | Contents |
|---|---|
| `core/` | `acestudio_core` static library. Public headers in `core/include/acestudio/core/`. Must never include Qt. |
| `app/` | Qt 6 Widgets desktop application. `app/src/panels/` holds dock panels. |
| `cli/` | `acestudio-cli` executable. |
| `mcp/` | `acestudio-mcp`: protocol layer and tools (Qt-free library `acestudio_mcp_proto`), stdio/HTTP transports and the app bridge client (`main.cpp`, Qt Core/Network). |
| `tests/` | Catch2 unit tests and CTest-driven CLI end-to-end test. `tests/engine/`: Godot and Unity import checks. `tests/mcp/`: MCP conformance and SDK client checks. |
| `cmake/` | Dependency pins, warning flags, Qt license guard. |
| `docs/` | Format specs and research notes (`docs/research/`). |
| `.github/workflows/` | CI (Linux core build + Windows full build). |

## 3. Core module contracts

- **Result / Status** (`Result.h`): every fallible operation returns `Result<T>` or `Status`
  (`[[nodiscard]]`). No exceptions cross module boundaries; JSON parse exceptions are caught
  inside ProjectIO and converted to `CorruptData`.
- **Image**: dense straight-alpha RGBA8, row-major. Dimension cap 16384 per side to bound memory
  from untrusted files.
- **BlendMode**: W3C separable compositing (normal, multiply, screen, overlay, darken, lighten,
  add, subtract, difference). Opaque normal pixels take an exact fast path so pixel art is never
  altered by float rounding.
- **Document**: canvas, color mode, pixel aspect, color profile, palette, layers (bottom→top,
  groups via `parent`), frames (durations 1–65535 ms), cels keyed by (layer, frame) with
  offsets, animation tags, and a namespaced `extensions` JSON object for AI provenance,
  export presets, tileset metadata and project preferences. Mutators preserve invariants;
  `validate()` re-checks everything after loading untrusted data.
- **ProjectIO**: see `docs/project-format.md`. Save = serialize → verify by deserializing →
  atomic write with `.bak` of the previous file. Load = CRC-checked ZIP extraction with
  per-entry and total size limits → schema check → structural validation.
- **FileUtil::writeFileAtomic**: write temp sibling → flush to disk (`FlushFileBuffers` /
  `fsync`) → optional backup copy → replace (`MoveFileExW(REPLACE_EXISTING|WRITE_THROUGH)` /
  `rename` + directory fsync). Readers never observe a partial file.
- **Settings**: every key has a registered default and type; wrong-typed writes are rejected;
  corrupt files are quarantined (renamed `*.corrupt-<timestamp>`) and reported; files from a
  newer schema are refused rather than overwritten.
- **Log**: thread-safe, rotating file sink (5 × 5 MB default), optional stderr, listener hook
  used by the in-app Log panel.

## 3a. Editing model (Phase 2)

- **Editor** owns the `Document` and an `UndoStack`. Every user, script or (future) MCP edit
  goes through an Editor method, which validates, applies and records a `Command`. Commands
  must restore the exact prior state on undo; `tests/test_editor.cpp` enforces this by
  comparing full snapshots (metadata JSON + every cel's pixels and position) after undo and redo.
- Structural commands capture precisely what they need (e.g. delete-layer keeps the whole
  subtree with stack indices and cels). Property changes on the same layer and field can merge
  (slider drags) but never across the saved ("clean") point.
- **Grouped steps**: `UndoStack::mark()` / `squash(mark, label)` fold the commands recorded
  since a mark into one `CompositeCommand` (undo runs the parts in reverse). Multi-step edits
  ("expand canvas + insert image", frame-block moves) use it through `Editor::grouped`. That
  helper mutes change notifications while the steps run, rolls back on failure and notifies
  once.
- **PaintSession** edits one cel live so the canvas can show strokes in progress. On start it
  normalises the cel to canvas size (keeping the exact original for undo); on commit it diffs
  against the original and records one `PaintCommand` holding only the changed rectangle
  (before/after patches). Constraints are applied per pixel: selection, alpha lock, colour mode
  (indexed → nearest opaque palette colour, grayscale → Rec.709 luma) and mirroring.
- **Raster** contains the pure algorithms (Bresenham, rectangles, ellipse inside-test for exact
  symmetry, pixel-perfect corner detection, scanline flood fill with an explicit stack, masks).
- **Compositing**: groups are rendered in isolation then blended with their own mode/opacity.
  Normal blending uses exact integer arithmetic; other modes use the float W3C formulas.
  `compositeRegion` lets the canvas recomposite only a stroke's dirty rectangle. Since 0.5.1:
  - Clipped layers use the alpha of the base layer below them; a hidden base hides them.
  - Masks multiply a layer's alpha (red channel, white outside the mask cel).
  - Adjustment layers transform the buffer accumulated so far, weighted by opacity × mask ×
    base alpha.
  - Areas of 256² px or more are split into horizontal bands composited on worker threads. The
    compositor is per-pixel over a read-only `Document`, so the output is identical to a
    single pass (tested).
  - Cels may extend past the canvas; transforms work on the union of the cel and the result,
    so off-canvas pixels are never dropped.
- **Project schema 2** adds masks (`masks/L<id>_F<n>.png`), clipping and adjustment fields
  and the tile mode. `requiredSchemaVersion()` still writes schema 1 when none of these is
  used, so older builds keep opening such files.
- **Recovery**: `RecoveryStore` writes `<id>.acestudio` + `<id>.json` (writer pid, original
  path) into `<data dir>/recovery`. Files whose writer process is no longer running are
  offered for recovery at startup. Saving or closing a document removes its files.

## 4. Desktop application

- Qt Widgets with `QDockWidget` docking (dock, float, resize, hide, restore; layout persisted
  in settings; Window › Reset Layout).
- `AppContext` owns settings, data directory and recent files. `MainWindow` owns tabs and
  docks. Each tab is a `DocumentView` (owns a `Document`) with a `CanvasWidget`.
- `CanvasWidget` displays the flattened frame with nearest-neighbour sampling at all zoom
  levels, integer zoom steps ≥ 100%, checkerboard transparency, and a pixel grid. Zoom anchors
  at the cursor; pan with middle-drag, Space+drag or Shift/Alt+wheel.
- `ToolState` holds the active tool, colours and brush options shared by all documents.
  `DocumentView` turns canvas input (`toolPressed/Moved/Released` in pixel coordinates) into
  `PaintSession` operations and keeps a cached composite updated by dirty region.
- An autosave timer (default 120 s, `general.autosaveIntervalSec`) writes recovery copies of
  modified documents that are not mid-stroke.
- Menus for features in later phases exist only as disabled entries naming the phase
  (execution rule 1: nothing fake, unavailable features clearly marked).

## 5. Architecture decision records

### ADR-001 — C++20 + Qt 6 Widgets, Qt under LGPLv3 with dynamic linking
- **Decision:** C++20 for core and app; Qt 6 (target 6.12 LTS) Widgets for the shell.
- **Why Widgets over QML for the shell:** dock-heavy creative tools (Krita-style) are mature in
  Widgets; `QDockWidget` covers dock/float/restore without extra dependencies. QML remains
  available (LGPL) for individual panels later if it proves better.
- **Licensing:** Qt is linked dynamically and its LGPL obligations are met by shipping Qt DLLs,
  license texts and relinking ability. `cmake/QtLicenseGuard.cmake` fails the build if a
  GPL-only module (Graphs, Charts, Quick 3D, Virtual Keyboard, Lottie, Quick Timeline, Canvas
  Painter, HTTP Server, Network Authorization) is linked or if Qt is static.
- **Considered:** Rust core (rejected: bridge complexity for little gain at this stage);
  Qt Advanced Docking System (LGPL-2.1, deferred until QDockWidget proves insufficient).

### ADR-002 — Qt-free core shared by every front end
- The core library cannot include Qt. This keeps CLI/MCP/tests light, allows headless servers,
  and guarantees one implementation of every format.

### ADR-003 — Project format: ZIP container with JSON + PNG cels
- Human-inspectable, diffable metadata; PNG keeps cels lossless and compact; ZIP CRC-32 gives
  integrity checking for free; partial reads are possible later. Versioned by
  `manifest.schemaVersion`; newer versions are refused with a clear message, older versions
  will be migrated in code.

### ADR-004 — AI inference in a separate worker process (implemented in 0.5.0)
- Python worker (`ai_worker/acestudio_worker`; PyTorch + Diffusers for SDXL, ONNX Runtime for
  BiRefNet, PyTorch or ONNX for Real-ESRGAN) launched and supervised by the Qt-free core
  (`Subprocess`, `WorkerSupervisor`), so the app and `acestudio-cli ai` share one
  implementation. Protocol: newline-delimited JSON-RPC 2.0 over stdin/stdout
  (`docs/ai-worker-protocol.md`), one job at a time, file-based image exchange in per-job
  folders. A model crash or out-of-memory kill restarts the worker, never the editor; the
  policy (retry once, hard cancel after 10 s, stop after repeated crashes) is in the protocol
  document and covered by tests that kill the real worker.
- The worker's core is standard-library Python, so it starts and reports missing packages
  before any AI package is installed. Packages are installed per profile
  (`ai_worker/requirements/*.txt`, pinned) into a private virtual environment in the
  non-roaming user data folder (`%LOCALAPPDATA%\AceStudio\ai`); models live there too
  (`ai.modelDirectory` overrides).
- Models come from `ai_worker/catalog/models.json` (licence, size, SHA-256 per file). A
  download requires a recorded licence approval; files are verified before being renamed into
  place, and hashes the catalogue could not pin are recorded at first download.
- Default model set and exclusions: `docs/research/ai-models.md`. No model is downloaded
  without explicit user approval.

- **Phase 5 (0.6.0)** adds three Qt-free core modules:
  - `Pose`: an 18-point skeleton in normalised coordinates, the conditioning renderer,
    procedural cycles, and `poseFramingBox`, one crop box shared by all frames.
  - `AnimationQA`: per-frame scores against frame 1 (ground line, height, palette, colour
    mix, silhouette IoU aligned at the bottom centre) and `correctFrames`.
  - `Character`: `.acechar` zip profiles.

  The app's `AnimationDialog` sends one `image.generate` job per frame with `controlnet`,
  `ip_adapter` and `pixel.box` parameters, and commits through `Editor::addAnimationLayer`
  (one undo step). On GPUs under 10 GB the worker stores the UNet and ControlNet weights in
  FP8, because Diffusers keeps the ControlNet resident outside the model-offload chain.
- Job folders are pruned at start-up after 7 days (`pruneJobFolders`).
- **Phase 6 (0.7.0)** adds three Qt-free core modules:
  - `Atlas`: MaxRects packing (best short side fit, fixed ordering, so output is
    byte-identical), trim, duplicates, extrusion, pages, TexturePacker-style JSON.
  - `Autotile`: terrain samples (3×3 coverage per tile), Cardinal16 / Blob47 / Wang16
    placement, a generator that interpolates coverage with tile-periodic noise (so legal
    neighbours match by construction), `checkSeams` and `checkWrap`.
  - `AssetTools`: sprite and grid detection, 9-slice detection and rendering, the UI kit
    generator, and parallax rendering, checks and `makeSeamlessX`.

  The app's `assets/` dialogs are thin views over these; `AiImageJob` wraps one SDXL job with
  the worker's `tileable` mode (circular padding patched into the UNet and VAE convolutions).

### ADR-005 — MCP server (implemented in 0.8.0)
- There is no official C++ MCP SDK, so `acestudio-mcp` implements JSON-RPC itself on nlohmann/json.
- **Dual-era.** It speaks 2026-07-28 (stateless: per-request `_meta` envelope,
  `server/discover`) and the legacy `initialize` handshake (2025-11-25 and earlier).
  - On stdio, the client's first request locks the era of the connection, as in the
    reference SDKs.
  - Over HTTP, each modern request stands alone, and legacy clients get a session per
    `initialize`.
- **Tools run `acestudio-cli --json` as a subprocess.**
  - The CLI is the single implementation; MCP adds argument checks, root-folder limits
    and image content.
  - A crash in a tool cannot take the server down, and cancellation kills the child.
- **The live app bridge is a per-user local socket.**
  - It is a named pipe on Windows, owned by the user.
  - Scripts run on a copy of the document on a worker thread and are applied with
    `Editor::replaceDocument` as one undo step.
  - The result is dropped if the user edited the document meanwhile.
- **Verification.**
  - Catch2 tests of the protocol layer.
  - The official conformance suite (0.2.0-alpha.12), every scenario, no baseline.
- **Input from the client.** Tools ask through `InputContext`: in the legacy era it sends a
  server-to-client request and waits; in 2026-07-28 it records the question, the server
  returns `input_required` with an HMAC-signed `requestState` (bound to the method, name,
  arguments and a 10-minute expiry), and the retry replays the handler with the answers.
- **Tasks.** With the tasks extension, task-capable tools run on a detached thread behind a
  `TaskStore`; `ask()` then parks the task as `input_required` until `tasks/update` brings
  the answers. A tool may gather input first and hand back `asyncWork`, which becomes the
  task (MRTR, then task).
- **Durable tasks.** Every change is also written to `<task dir>/<taskId>.json`. The owner
  refreshes `<instance>.alive` every second and picks up `<taskId>.cancel` and
  `<taskId>.update-*.json` requests left by other processes. A task file whose owner has gone
  quiet for 10 s reads as failed. A listen stream with `tasks` gets `notifications/tasks`.
  - The Python (dual-era) and TypeScript SDK clients over both transports.
  - CI runs them against the packaged Windows build.

### ADR-006 — Packaging: Inno Setup per-user installer with a bundled Python (implemented in 0.9.0)
- **Why Inno Setup.** Free for commercial use; WiX binaries now carry a maintenance-fee EULA
  above US$10k revenue. MSIX may follow for the Store and enterprises.
- **Per-user by default** (`PrivilegesRequired=lowest`, override allowed): no UAC prompt, and
  the `.acestudio` association goes to HKCU. Installs into `{autopf}\AceStudio`
  (`%LOCALAPPDATA%\Programs\AceStudio` for a per-user install).
- **Contents.** The program, Qt (dynamic, LGPL), the app-local VC++ runtime, the AI worker
  source, `licences\`, `docs\` and `EULA.txt`. CI checks that every DLL an executable imports is
  present.
- **Python.** The 3.12 embeddable package (python.org's SHA-256 checked in CI) and a pinned
  pip wheel. On first AI setup the embeddable Python is copied to the user's AI folder, its
  `._pth` removed so it behaves like a normal install, and pip unpacked into it (a wheel is a
  zip; running pip from the wheel failed on the runner). Users no longer need to install
  Python; a system Python is used only if the bundled one is missing.
- **Models are never bundled**: each is an optional download after its licence is accepted.
- **Uninstall keeps user data** (settings, recovery files, licence, models).
- **CI test.** A silent per-user install, the files and association, the CLI from the install
  folder, `ai setup` with the bundled Python, then a silent uninstall that must remove the
  program and keep the data.

### ADR-007 — Scripting: Lua 5.4.9 + sol2 3.3.0 (implemented in 0.8.0)
- **Build.** Lua is compiled as C++ (`SOL_USING_CXX_LUA`), so Lua errors unwind C++ frames
  safely.
- **Sandbox.**
  - Available: the base, string, table, math, utf8 and coroutine libraries. Removed:
    `dofile`, `loadfile` and `string.dump`. `load` takes text chunks only.
  - Only `os.time`, `os.clock`, `os.date` and `os.difftime` remain of `os`; `io`, `package`
    and `debug` are gone.
- **Limits.**
  - Memory is limited through the Lua allocator, and the same budget counts the pixels of
    every image the script holds and the documents it creates or grows (a `Charge` per image;
    a full GC runs before a native allocation is refused).
  - Time and cancel are checked by a count hook. Once a stop is set it is sticky: the
    sandbox's `pcall`, `xpcall` and `coroutine.resume` re-raise it, so scripts cannot
    catch it.
- **Files.** Scripts reach files only through the `ace` API, and every path is resolved
  (`weakly_canonical`) and checked against the allowed folders.
- **Undo.** Edits to the host document go through its `Editor`, so a script is one undo
  step, rolled back on error.

### ADR-008 — Licensing: offline Ed25519 keys and a 14-day trial (0.9.0)
- **Keys** are `ACESTUDIO-1.<base64url JSON>.<base64url signature>`. The JSON holds the
  licensee, e-mail, edition, issue date and optional expiry; the signature is Ed25519
  (Monocypher 4.0.3, checked against the RFC 8032 vectors). Verification is offline: only the
  public key is in the program.
- **Key pairs.** The release public key is a CMake variable (`ACESTUDIO_LICENCE_PUBLIC_KEY`,
  from a CI variable). Without it, builds use a development key whose seed is public; the
  licence dialog and `acestudio-cli licence` (`developmentKey`) say so, and CI warns when a
  nightly is built that way. The private key lives only with the publisher
  (`tools/licence`, never shipped).
- **Trial.** 14 days of everything. The start date is kept in `trial.json` and in the
  registry; the earliest wins, so deleting one does not restart the trial. A test data folder
  (`ACESTUDIO_DATA_DIR`) keeps the registry out.
- **After the trial, read-only** rather than locked out: files open and render; saving,
  exporting and write commands (app, CLI exit 7, Lua, MCP) are refused with a clear message.
  Autosave and crash recovery keep working, so no work is ever lost to licensing.
- **Not DRM.** A determined user can patch a binary; the aim is honest users' convenience and
  a clear line, without activation servers or telemetry.

### ADR-009 — Accessibility is tested, not reviewed (0.9.0)
- Every control gets an accessible name. Most are derived automatically when a window is
  shown (from its label, form row, grid cell, tool tip or placeholder); the rest are set
  explicitly.
- `AceStudio --ui-audit` opens every window by triggering every menu command, and fails if any
  visible control is unnamed or cannot be reached with Tab (`ui_audit` test; it runs wherever
  the app is built, on the Windows CI runner and on the Linux development machine).
- High-contrast theme (and Windows high-contrast detection), text size 100–200%, and
  rebindable shortcuts for every command.

### ADR-010 — Untrusted input: own decoders where fuzzing found problems (0.9.0)
- Every parser of untrusted input has a libFuzzer target (`tests/fuzz`): project, PNG, GIF,
  APNG, WebP, palette, licence key, Lua and MCP messages. Findings become regression inputs.
- Decoders check the total decoded size (1 GB for an animation) before allocating.
- GIF is decoded by AceStudio's own bounds-checked decoder: fuzzing found a use-after-free in
  stb_image's GIF loader. stb_image is compiled for PNG only. See SECURITY.md.

### ADR-011 — Regression suite: old files, golden renders, performance budgets (0.9.0)
- **Old projects.** `tests/data/compat` holds projects saved by 0.1.0, 0.5.0 and 0.8.1 and their
  renders, written by those versions built from git. The current build must render them
  identically (one documented exception: group compositing changed on purpose in 0.2.0).
- **Golden renders** of every blend mode, adjustment, mask, clipping, onion skin and the packed
  sprite sheet with its JSON. Updating them is a deliberate act
  (`ACESTUDIO_UPDATE_GOLDEN=1`), explained in the commit.
- **Performance budgets.** `acestudio_bench --budget bench/perf_budgets.json` runs as the
  `perf_budget` test in optimised builds. The ceilings are several times the measured times,
  so they catch real regressions rather than runner noise.

## 6. Error-handling rules

1. No silent failures: every error reaches the user (dialog/status) and the log.
2. A failed save leaves the previous file untouched (atomic replace + verification).
3. Untrusted input (project files, settings, future plugins/models) is size-limited and
   validated before use.
4. Destructive actions require confirmation or keep a recovery copy (`.bak`, quarantine).
