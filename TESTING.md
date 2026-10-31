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
| `test_subprocess` | unit | stdin/stdout/stderr lines, UTF-8, exit codes, 5000-line output with unterminated tail, arguments with spaces/quotes/backslashes (Windows quoting round-trips through the real C runtime), environment, working directory, kill, destructor kill, missing programs |
| `test_ai_worker` | integration | the real Python worker: handshake, progress, ordering, queued cancel, cooperative and forced cancel, crash retry then failure with stderr, restart-storm cut-off, external SIGKILL mid-job (job retried and succeeds), image inputs, stop, unstartable worker |
| `test_model_catalog` | unit/integration | shipped catalogue valid and commercial-only, unsafe entries rejected, licence-gated downloads, install state, hash pinning, end-to-end download through the worker from a local HTTP server |
| `ai_worker/tests/test_server.py` | integration (Python) | protocol, busy/next-job race, cancel, failures, stdout protection, crash exit, EOF, downloads with resume, hash mismatch, licence refusal, unsafe paths |
| `ai_worker/tests/test_models_real.py` | integration (Python, optional) | BiRefNet IoU against a known mask; Real-ESRGAN reconstruction; tiny random SDXL through real Diffusers (seeds, cache, pixel post-process with exact palette, img2img, cancel, input errors) |
| `cli_smoke` | end-to-end | every CLI command, JSON output, exit codes, overwrite protection, corrupt-file handling |

## Recorded results

### 2026-10-30 — Phase 4, version 0.5.0

| Environment | Build | Result |
|---|---|---|
| Linux, GCC 13.3, Qt 6.4.2, Python 3.13 | everything | ✅ 125/125 CTest (incl. the real-worker AI tests); Python worker suite 14/14 |
| Windows target via MinGW-w64, `-Werror`, under Wine | core + CLI + tests | ✅ 116 passed, 7 skipped (they need Python, absent in Wine), 1941 assertions; `ai models` / `ai status` find the worker files |
| Real models, CPU (2-vCPU VM, 7 GB RAM) | Python suite with torch 2.15, diffusers 0.42, onnxruntime 1.31 | ✅ Real-ESRGAN x4plus 64→256 px: 6.0 s, PSNR 28.3 dB vs original (bicubic 33.2 dB on this synthetic image; ESRGAN adds texture); ✅ tiny SDXL plumbing 12.8 s; ❌ BiRefNet Lite: OOM-killed above 6 GB RSS at its fixed 1024² input |
| Real downloads | CLI | ✅ Real-ESRGAN anime 17.9 MB in 1.4 s and BiRefNet Lite 224 MB in 12 s from the official GitHub releases, both matching the pinned SHA-256 |
| Runtime setup | CLI and app | ✅ private venv created, `onnx-cpu` profile installed from PyPI, worker restarted with the new packages |

Manual desktop checks (Linux, Xvfb), real input:

| Check | Result |
|---|---|
| AI Studio menu and dock; Upscale ×4 on the knight opens a 128×128 document | ✅ |
| Models dialog: licence, size, state; SDXL licence dialog keeps "Accept and Download" disabled until the box is ticked | ✅ |
| Remove Background with BiRefNet on the 7 GB VM: worker OOM-killed twice (attempt + retry), job Failed with "The system may have run out of memory", worker restarted, editor responsive | ✅ (recovery path); the model itself is unverified here |
| Set Up AI Runtime in a fresh data folder: venv, install, restart, status "AI ready" | ✅ |
| Defects found and fixed: hello blocked for tens of seconds importing torch (now probed after hello); "done" was sent before the worker was free, so the next job was refused as busy; a Diffusers 0.42 API change (VAE tiling); dropped final progress step; AI dock forced the window taller than the screen; long model names widened the dock; Models dialog buttons enabled with no selection; setup text named the wrong destination for a configured Python; log lines stuck in the buffer | ✅ fixed and re-verified |

Windows (GitHub Actions, windows-2022, MSVC, Qt 6.8.3), first fully green run 2026-10-30
(run 40998686282): all 124 CTest tests including the real-worker AI tests, the Python worker
suite on Windows Python 3.14, and the packaged CLI finding its worker files. Three defects only
real Windows exposed, all fixed: MSVC C4996 on `sscanf`/`getenv` (warnings are errors); a
test-fixture bug that took `begin()`/`end()` from two copies of the Python launcher arguments
(harmless on Linux where the list is empty, a crash on Windows where it holds `-3`); and
program/working-directory paths are now normalised to backslashes before `CreateProcessW`
(a defensive change made while diagnosing; not proven to have been needed).

First desktop launch: the CI build started and opened its window on a Windows 11 PC
(NVIDIA GPU, VC++ runtime already installed), copied without the software-OpenGL, DirectX
shader compiler, network and TLS files windeployqt adds, which a Widgets app does not need.

Hands-on checks on that PC (Windows 11, 1456×819 logical desktop, NVIDIA GPU), through
computer use on 2026-10-30 with build 8955520:

| Check | Result |
|---|---|
| Launch; welcome page; Open Project through the native file dialog; knight renders correctly | ✅ |
| Pencil draws, unsaved marker appears; Ctrl+Z twice restores the file and clears the marker | ✅ |
| AI Studio: "AI runtime not set up"; Set Up found Python 3.15.0 through the `py` launcher | ✅ |
| Set Up created the private venv in `%LOCALAPPDATA%\AceStudio\ai\python`; worker started (hello in 0.2 s) and began installing `onnx-windows` | ✅ |
| Cancel Job on the running install: job Cancelled, worker idle and ready | ✅ |
| Install `onnx-windows` on Python 3.15 | ❌ cannot work: onnxruntime-directml has no 3.15 wheels; pip ran for 5+ minutes with no output in the app. Fixed in 53256f7 (version preference, warning, pip output in the log, binary-only installs) |
| Maximised window at 1456×819 | ❌ four stacked right-hand docks needed ~850 px, so the window grew past the screen and hid the timeline; layer buttons and Properties text overlapped. Fixed in 53256f7 (tab groups, smaller minimums; verified at 1456×740 in Xvfb) |
| Properties "File" row | ❌ showed "C:" only; fixed (file name, full path in the tooltip) |

Re-check on the same PC with nightly build c57e5c2 (portable mode, data in `<exe>\data`):

| Check | Result |
|---|---|
| Window fits 1456×819: timeline and Log tabs visible, Layers/AI Studio and Palette/Preview/Properties tab groups | ✅ |
| Set Up AI Runtime with only Python 3.15 installed: warns it is newer than the AI packages support, links python.org, background-removal option disabled; destination shows the portable data folder | ✅ |
| Properties "File" row shows `knight.acestudio` | ✅ |
| Palette buttons with no document open are clipped vertically (cosmetic) | ⚠️ known issue |
| Python 3.14.10 installed by the user: found and accepted, but it was the **32-bit** build (win32 wheels; no ONNX Runtime or PyTorch builds exist) | ❌ fixed: 32-bit detection |
| Build a3c7faf with 64-bit Python 3.14.10 also installed: picked the 64-bit one, rebuilt the AI environment that had been made from the 32-bit one | ✅ |
| Install `onnx-windows` (a3c7faf): pip output streamed into the Log, `win_amd64` wheels, finished in 29 s | ✅ |
| BiRefNet Lite download (224 MB, licence accepted by the user's instruction): SHA-256 verified, state Installed | ✅ |
| Remove background, DirectML (onnxruntime-directml 1.24.4): `RUNTIME_EXCEPTION` in `DmlFusedNode_17_58` | ❌ on this GPU/driver; fixed by falling back (below) |
| Remove background with the fallback: DirectML failed, the retry ran on the CPU; ~16 s for BiRefNet at 1024², result added as a layer | ✅ (CPU) |
| BiRefNet quality on the synthetic `scene.png`: background gradient removed and figure kept, but many flat circles kept as foreground (IoU 0.60 vs the intended mask) | ⚠️ synthetic image is a weak test for a photo-trained model |
| Install `diffusion-cuda` with the old pins: pip took PyPI's CPU-only torch 2.15.0 (126.7 MB) without error | ❌ fixed: `torch==2.14.1+cu126` and a CUDA check after install |
| "checking GPU…" never finished (2.5 h) after the PyTorch install | ❌ fixed: worker protocol stdin moved off fd 0; after the change the probe finished within 30 s of a restart |
| Install `diffusion-cuda` with the fixed pins: torch 2.14.1+cu126 (CUDA 12.6) downloaded from download.pytorch.org and verified; ~4 min including diffusers/transformers | ✅ |
| Device probe: NVIDIA GeForce RTX 4070 Laptop GPU, 8.0 GB | ✅ |
| Real-ESRGAN x4plus download (67 MB, BSD-3, licence accepted on the user's instruction): SHA-256 verified | ✅ |
| Upscale ×4 on CUDA, 512×512 → 2048×2048: 3.1 s for the whole job (model load, tiled inference, 4.3 MB PNG); result opened as a new document; downscaled back it matches the source at 37.6 dB PSNR, edges smooth | ✅ |
| SDXL Base 1.0 + Pixel Art XL file lists checked against the Hugging Face API; URLs pinned to commits 462165984030 / 8bf4a4d9ea28; weight hashes pinned | ✅ |
| SDXL download (6.94 GB, licence accepted on the user's instruction): ~8 min; every pinned SHA-256 matched; Pixel Art XL (170.5 MB) likewise | ✅ |
| SDXL text-to-image with Pixel Art XL on the RTX 4070 Laptop (8 GB, so model CPU offload), 1024×1024, 30 steps: ~50 s for the first job (includes loading 7 GB from disk), ~34 s warm; times read from the job files, ±3 s | ✅ |
| Generated knight sprite is clean at 1024²; the 64×64 pixel-art result (palette-limited, 10 colours) is coarse: 16× reduction, figure drawn small | ⚠️ quality limitation; see KNOWN_ISSUES |
| "Pixel art for this canvas" was unticked whenever the session started on the welcome page, so the first result arrived as a 1024² document | ❌ fixed (b49bd18) |
| Tab in the prompt typed a tab character | ❌ fixed (b49bd18) |
| `diffusion-cuda` with the driver check: NVIDIA driver 621.01 detected → torch 2.14.1+cu130 (1998.9 MB) and onnxruntime-gpu 1.31.1 replacing onnxruntime-directml; ~3 min | ✅ |
| BiRefNet Lite on the CUDA execution provider (no fallback warnings): cold job 6.4 s session + 1.2 s run; warm jobs reuse the cached session: runs of 7.7 s, 2.6 s, 2.6 s (the outlier coincides with the laptop GPU waking); CPU was ~16 s | ✅ |
| Remove BG tab: the Run button is below the fold of a ~80 px tab area on a 819 px screen; needs scrolling | ⚠️ usability |
| Install `onnx-windows`: no pip output for 4+ minutes. Same pip from a console (PIP-CHECK.cmd) reached PyPI in 0.8 s and downloaded numpy in under a second | ❌ fixed: pip inherited the worker's protocol stdin and stalled at startup; it now gets DEVNULL (unit-tested; Windows re-check pending) |

**Not yet verified:** anything on an NVIDIA GPU (SDXL quality and speed, CUDA wheels from the
PyTorch index, DirectML), BiRefNet output quality, the `onnx-windows` and `diffusion-cuda`
profiles, and the SDXL/Pixel Art XL file lists against Hugging Face (not reachable from the
build environment).

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
