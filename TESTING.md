# Testing

## Running the tests and benchmarks

```sh
cmake --preset core-only && cmake --build --preset core-only && ctest --preset core-only
```

Benchmarks: `build/<preset>/bench/acestudio_bench` (build in Release for meaningful numbers).
On Windows use the `windows-release` preset. Tests never touch real user data: they use unique
temporary directories, and `ACESTUDIO_DATA_DIR` redirects the app's settings and logs.

## Suites

| Suite | Kind | Covers |
|---|---|---|
| `test_color_image` | unit | hex parsing, luma, image limits, bounds safety, fill/copy/blit clipping, palette capacity and lookups |
| `test_blend` | unit | all 9 blend modes against reference values, opacity, transparent backdrops, exact opaque fast path |
| `test_document` | unit | layer/group hierarchy, cel rules, frame insert/duplicate/remove with cel and tag shifting, compositing with offsets/visibility/opacity/groups, corruption detection (cycles, duplicate IDs) |
| `test_png` | unit | lossless RGBA round trip, garbage rejection, file I/O |
| `test_project_io` | unit/integration | lossless save/load, `.bak` creation, no temp-file leftovers, truncated archive, CRC failure on damaged pixel data, non-ZIP input, newer-schema refusal, refusal to save an invalid document |
| `test_fileutil` | unit | atomic write create/replace/backup, size limits, timestamps, data-dir override |
| `test_settings` | unit | defaults, type enforcement, persistence, corrupt-file quarantine, unknown/mistyped values skipped, newer schema not overwritten |
| `test_log` | unit | level filtering, file sink, listener, rotation |
| `test_undo` | unit | push/undo/redo, failed commands not recorded, redo-branch discard, clean point, merging, step and memory limits |
| `test_editor` | unit | every Editor operation (layers, frames, palette) is checked to undo to the exact prior state and redo to the exact post state twice; invalid ops leave no history; slider merging; isolated group compositing |
| `test_paint` | unit | Bresenham, rect/ellipse symmetry, pixel-perfect, flood fill (incl. 1M-pixel region without stack overflow), live painting + single undo step, cancel/no-op, offset-cel restore, brush size/shape/mirror, selection, alpha lock, lock/hidden/group refusal, indexed/grayscale constraints, shape preview, fill, eyedropper |
| `test_recovery` | unit | recovery write/load/remove, live-writer exclusion, unsafe ids, bad metadata, process liveness |
| `test_transform` | unit | flips/rotations as exact permutations, nearest scaling round trip, Scale2x behaviour, RotSprite palette preservation/area/right-angle equivalence, outline modes, polygon masks, selection ops, Bayer matrix, gradients, dither brush |
| `test_palette_io` | unit | .gpl/.pal/.hex/.txt round trips, real-world variants (CRLF, comments, alpha), malformed and oversized input rejected, files on disk, extraction (exact and median cut), colour-mode conversion with undo |
| `test_animation` | unit | tag cycles for every direction, repeats, `AnimationPlayer` looping/finite tags/all-frames, frame move with exact undo, tag validation and undo, onion-skin fade/tint/wrap |
| `test_export` | unit/integration | frame ranges and playback order, every sheet layout placing frames exactly, trim/merge/packed/power-of-two, Aseprite JSON (hash/array, tag remapping), GIF/APNG/WebP round trips, noisy-frame LZW resets, >255-colour reduction bound, corrupt input, PNG sequence and file writers refusing to overwrite |
| `cli_smoke` | end-to-end | every CLI command, JSON output, exit codes, overwrite protection, corrupt-file handling |

## Recorded results

### 2026-10-29 — Phase 3, version 0.4.0

| Environment | Build | Result |
|---|---|---|
| Linux, GCC 13.3, Qt 6.4.2, Release, `-Werror` | everything | ✅ zero warnings; 106/106 CTest |
| Windows target via MinGW-w64, `-Werror`, under Wine | core + CLI + tests | ✅ 105/105 test cases, 1823 assertions; cli_smoke (incl. all export formats) passed |
| Independent decoder | GIF from the app | ✅ Pillow: 4 frames, durations 100/120/120/120 ms, loop 0 |
| GitHub Actions (Windows MSVC + Linux) | everything | see commit status on GitHub |

Benchmarks (2-vCPU Xeon VM, Release, median ms):

| Benchmark | 256×256 | 1024×1024 |
|---|---|---|
| Full composite, 20 layers | 6.5 | 110.3 |
| Onion skin, 1 ghost each side (populated frames) | 14.2 | 242.7 |

Manual desktop checks (Linux, Xvfb 1600×1000, Qt 6.4.2), real mouse/keyboard input on the
knight sample:

| Check | Result |
|---|---|
| F3 shows tinted ghosts of frames 1 and 3 under frame 2 | ✅ |
| Dragging frame 1 past frame 3 moves it with its 100 ms duration; Ctrl+Z restores the order | ✅ |
| Tags dialog: add "walk" frames 2–3 ping-pong; timeline lists both tags | ✅ |
| Preview dock loops while the canvas stays on another frame | ✅ |
| Export Sprite Sheet: grid, 2×, trim → 128×104 PNG + JSON with both tags and trimmed source rects | ✅ |
| Export GIF twice: second time asks "Replace existing files?" with Cancel as default | ✅ |
| Enter / play button: playback starts and stops on the current frame | ✅ |
| Defects found and fixed during this check: literal "&" in the export dialog's File label; near-invisible summary text; every new tag got the same colour | ✅ fixed and re-verified |

### 2026-10-29 — Phase 2b, version 0.3.0

| Environment | Build | Result |
|---|---|---|
| Linux, GCC 13.3, Qt 6.4.2, Release, `-Werror`, clean build | everything | ✅ zero warnings; 90/90 CTest |
| Windows target via MinGW-w64, `-Werror`, under Wine | core + CLI + tests + bench | ✅ 89/89 test cases, 1518 assertions |
| GitHub Actions (Windows MSVC + Linux) | everything | see commit status on GitHub |

Manual desktop checks (Linux, Xvfb), real mouse/keyboard input:

| Check | Result |
|---|---|
| RotSprite 45° on the knight's body layer: clean edges, no new colours; undo restores | ✅ |
| Dithered gradient red→blue on a new layer (Bayer pattern) | ✅ (after fixing the Shift+G shortcut clash this check exposed) |
| Lasso selection outline; magic wand Alt+click subtracts a region | ✅ |
| Edit menu shows labelled undo ("Undo Gradient") and new items | ✅ |
| Keyboard Shortcuts dialog: filter, conflict (Ctrl+J twice) refused, Ctrl+G saved to settings and immediately creates a group | ✅ |

### 2026-10-29 — Phase 2a, version 0.2.0

| Environment | Build | Result |
|---|---|---|
| **GitHub Actions `windows-2022`, MSVC 2022, Qt 6.8.3, Release, `/W4 /WX`** | core + CLI + tests + bench + app, `windeployqt` staging | ✅ zero warnings; 71/71 CTest; `AceStudio-windows-x64` artifact produced (run 40944993023) |
| GitHub Actions `ubuntu-24.04`, GCC, `-Werror` | core + CLI + tests | ✅ 71/71 |
| Linux, GCC 13.3, Qt 6.4.2, Release, `-Werror`, clean build | core + CLI + tests + bench + app | ✅ zero warnings; 70/70 CTest (69 test cases + cli_smoke) |
| Windows target via MinGW-w64, `-Werror`, run under Wine 64 | core + CLI + tests + bench | ✅ zero warnings; 69/69 test cases, 1192 assertions; cli_smoke passed |

Manual desktop checks (Linux, Xvfb, Qt 6.4.2), driven with real mouse/keyboard events:

| Check | Result |
|---|---|
| Pick colour from palette, pencil stroke, rectangle, bucket fill inside the outline, ellipse | ✅ screenshot reviewed |
| Undo removes the last shape; redo restores it; save writes exactly what is on screen (CLI render compared) | ✅ |
| Tab shows modified dot and Properties shows "Unsaved changes" after edits; "Saved" after save | ✅ |
| New layer above current, 3px brush stroke on it, opacity change reflected in slider and layer list; two undos revert opacity then stroke | ✅ |
| Crash recovery: draw → autosave (5 s interval) → `kill -9` → relaunch → prompt → Recover restores the stroke under the original file path as unsaved; recovery file re-stamped with the new process; saving deletes the recovery files | ✅ |

Benchmarks (`acestudio_bench`, Release, 2-vCPU Intel Xeon 2.8 GHz VM, CPU only — not the
target Windows/GPU machine):

| Benchmark | Before optimisation | After |
|---|---|---|
| Full composite 256×256, 20 layers | 17.74 ms | 9.24 ms |
| Composite 32×32 dirty region of 256×256 | — | 0.16 ms |
| Stroke 200 px + commit, 256×256 | 0.17 ms | 0.34 ms |
| Undo + redo of a paint step, 256×256 | 0.01 ms | 0.01 ms |
| Full composite 1024×1024, 20 layers | 369.48 ms | 108.97 ms |
| Composite 32×32 dirty region of 1024×1024 | — | 0.10 ms |
| Stroke 200 px + commit, 1024×1024 | 2.46 ms | 2.41 ms |

Bugs found by the first real-Windows CI runs and fixed:
- MSVC C4996 on `getenv`/`_wfopen` (now `GetEnvironmentVariableW` and shared `_wfsopen`).
- MSVC C4245 signed/unsigned miniz constant in a test.
- **Reading a file that another handle had open failed on Windows** (`_wfopen_s` opens
  exclusively). Wine did not reproduce this. Regression test added in `test_fileutil`.

Not yet measured: real on-screen frame rate on Windows, high-DPI rendering cost. The Windows
app has been compiled, packaged and unit-tested on Windows, but not yet launched and used
interactively on a Windows desktop.

### 2026-10-29 — Phase 1, version 0.1.0

| Environment | Build | Result |
|---|---|---|
| Linux, GCC 13.3, Qt 6.4.2, RelWithDebInfo | core + CLI + tests + app | ✅ 40/40 CTest (39 unit test cases + cli_smoke) |
| Linux, GCC 13.3, Release, `-Werror` (CI config), clean build | core + CLI + tests + app | ✅ builds with zero warnings; 40/40 |
| Windows target via MinGW-w64 GCC (posix), `-Werror`, run under Wine 64 | core + CLI + tests (no app) | ✅ zero warnings; 39/39 test cases, 584 assertions; cli_smoke passed |
| Cross-platform format check | — | ✅ Windows-built CLI writes a project the Linux build validates; Linux-written project read by Windows build; frame renders pixel-identical |

**Not yet tested:** MSVC compilation, Qt 6.12, the desktop app on real Windows, high-DPI
scaling, and the GitHub Actions workflow itself. Wine is not real Windows: NTFS semantics,
antivirus file locking and `MoveFileExW` behaviour under load still need verification on a
Windows 10/11 machine.

### Manual desktop checks (Linux, Xvfb 1600×1000, Qt 6.4.2)

| Check | Result |
|---|---|
| App launches with a project path argument and opens it | ✅ |
| Canvas fits to window at integer zoom (2200% for 32×32), nearest-neighbour, grid visible | ✅ screenshot reviewed |
| Multiply layer at 43% opacity composites correctly (matches CLI render) | ✅ |
| Layers panel shows group nesting, blend mode and opacity | ✅ |
| Timeline: Enter starts playback, frames advance, Enter stops on the current frame | ✅ |
| Ctrl+Q exits cleanly; settings.json written with recent project and window layout; log written | ✅ |
| Defects found and fixed during this check: canvas fitted before final window size; transport icons blank on platforms without stock media icons; truncated frame label | ✅ fixed and re-verified |
