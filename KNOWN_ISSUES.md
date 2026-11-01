# Known Issues and Limitations

Current as of 0.10.0. Everything that could be fixed in code for Phases 1–8 has been fixed and
tested; what remains below needs hardware we have not had, is a limit of a third-party
component or file format, or is a deliberate design choice. Nothing is listed as done that has
not been run.

## Needs hardware or machines not yet available (Phase 9 test matrix)
- **Clean Windows install:** the package ships the Visual C++ runtime DLLs next to the
  executables, and CI fails if any shipped binary imports a runtime DLL that is not packaged.
  Starting it on a Windows install that has never had the redistributable has not been tried.
- **Per-monitor DPI:** 150% and 200% scaling were checked (Linux, `QT_SCALE_FACTOR`), and the
  test PC runs the app at its own scaling. Dragging the window between monitors with different
  scaling has not been tried.
- **GPUs other than the test PC's:** everything AI was verified on one RTX 4070 Laptop (8 GB,
  driver 621.01).
  - SDXL at 1024², 30 steps, with model CPU offload: ~34 s warm, 50–56 s cold.
  - Animation frames (SDXL + pose ControlNet, optional IP-Adapter, FP8 weight storage, 28
    steps, background removal included): 46–56 s per frame warm, ~73–84 s for the first.
  - BiRefNet on CUDA: 2.6 s warm.
  - Real-ESRGAN 512→2048: 3.1 s.

  12 GB+ NVIDIA cards (SDXL resident), AMD/Intel GPUs on DirectML, NVIDIA drivers older than
  R580 (PyTorch cu126 path) and the CPU-only PyTorch profile have not been installed on a
  Windows PC. The Models dialog's VRAM figures are estimates and say so.
- **Real-display frame rate** on Windows is not measured, only compositing times (see below).

## Limits of third-party components and formats
- **ONNX Runtime DirectML:** onnxruntime-directml 1.24.4 fails inside a fused node when running
  BiRefNet on the test PC. AceStudio falls back to fewer fusions, then to the CPU (~16 s per
  image). NVIDIA GPUs with driver R580+ use CUDA instead.
- **BiRefNet on the CPU needs about 6.5 GB of free RAM.** The ONNX export has a fixed 1024×1024
  input; a smaller input fails inside the graph. AceStudio checks free memory, including
  container limits, before a CPU run. If too little is free, the job fails within seconds and
  says how much is needed, instead of the worker being killed. The 12 GB figure for BiRefNet
  General is an estimate.
- **Python:** the installer bundles Python 3.12, so nothing needs installing. Without it (a
  build from source, or a deleted `python` folder) AceStudio looks for a 64-bit Python
  3.10–3.14; ONNX Runtime publishes no Windows wheels for 3.15 yet, and none for 32-bit. An
  AI environment already made from another supported Python is kept, not rebuilt.
- **Small canvases and SDXL:** generation runs near 1024² and is reduced with nearest neighbour.
  At 64×64 that is a 16× reduction, twice Pixel Art XL's ~8× pixel size, so results are coarse;
  128×128 fits best. "Frame the subject" helps when the background is plain.
- **FP8 weight storage (Precision: Auto)** is used for pose-controlled generation on GPUs under
  10 GB, so the UNet and ControlNet fit in VRAM.
  - Speed, measured on the 8 GB test PC at 10 steps: FP8 ~20 s per frame, Full 505 s, because
    full precision spills into shared memory.
  - Quality: with the same seed and pose the two give different images (PSNR 13 dB). Both are
    clean knights of similar quality, so FP8 changes which image a seed gives without
    visibly degrading it.
  - Seeds do not carry over between precision modes.
- **SDXL ignores some prompt words.** It sometimes adds a floor, a floor shadow or scenery even
  when asked for a plain background. AceStudio now asks for "isolated on a plain white
  background" and adds floor and scenery terms to the negative prompt. A floor drawn beneath
  an airborne character is removed before the checks. A floor touching the feet stays and
  counts as part of the figure.
- **Real-ESRGAN** smears small pixel art; that is how the model works. Use Image › Scale for
  pixel art.
- **GIF export:**
  - At most 256 colours per frame (median-cut reduction beyond 255).
  - 1-bit transparency: alpha below 128 becomes transparent.
  - Durations in 10 ms steps, minimum 20 ms; browsers treat shorter delays as 100 ms.

  Use APNG or WebP for exact output.

## Design choices (documented behaviour, not defects)
- **Compositing runs on the CPU.** Large areas are split across threads with identical output.
  On the 2-vCPU test VM, a full 1024×1024 × 20-layer recomposite with mixed blend modes takes
  34–44 ms in 0.9.0 (58–68 ms before the blend loops were specialised per mode). Strokes
  recomposite only their dirty region (0.07 ms for 32×32). A GPU compositor would have to
  reproduce every blend mode, mask, clip and adjustment exactly; these numbers do not justify
  that cost. `bench/perf_budgets.json` fails CI if they grow several-fold.
- **Live generation preview** needs the optional 10 MB TAESD-XL model (MIT; Models dialog).
  Without it, generation shows progress only. Generate Animation shows the preview on the
  frame being generated.
- **What the animation checks can and cannot see:**
  - Each frame is checked against its own pose: ground line and height (so a jump may leave
    the ground), facing, shape continuity with its neighbours (relaxed by as much as the poses
    themselves differ), and colour profile.
  - The colour profile is compared with frame 1 and with the character's reference images.
    It uses 12 hues × 3 lightness levels and 4 greys. On real frames of one character it
    measured ≤ 0.31, and ≥ 0.41 for other subjects and recoloured copies; the thresholds are
    0.36 and 0.38.
  - It is a colour check, not face or identity recognition: a different knight in the same
    colours passes. Reference images (IP-Adapter) are what keep the identity; the check
    catches drift.
  - Between very different poses (a crouch and the top of a jump overlap 12%) the shape check
    allows almost any shape. There, only the colour and identity checks protect against a
    wrong drawing.
- **Cancelling generation:**
  - Between steps (FP8, ~1 s each) a cancel takes effect at the next step.
  - Loading SDXL and importing PyTorch cannot be interrupted. A cancel then waits for the load
    to finish (up to 3 minutes), so the model stays loaded. Measured: it cancelled about 40 s
    later and the next job skipped the load.
  - Full-precision steps on an 8 GB GPU take 25–45 s and are waited for the same way.
- **Identical poses share one job.** A jump's rise and fall use the same poses, so a 6-frame
  jump runs 4 SDXL jobs; the repeated frames are pixel-identical.
- **One AceStudio per data folder.** Opening a file while AceStudio runs sends it to the open
  window. `--new-instance` starts another window anyway; two windows then share settings, and
  the last one closed wins.
- **Committing an animation** fills a blank document from frame 1. Into a document with
  content, it appends the frames after the last one with their own tag. To place frames
  elsewhere, move the block in the timeline afterwards.
- **AI job folders** (inputs, raw 1024² outputs, previews) are kept for 7 days, then removed at
  start-up.
- **Free Transform** scales about the box centre. Shift keeps proportions and dragging past the
  centre flips. The preview uses fast nearest-neighbour rotation; the applied result uses
  RotSprite.
- **Linked cels** link pixel cels only; layer masks stay per frame. Duplicating frames creates
  independent copies, and duplicating a layer keeps its links.
- **Frame selections with gaps** can be deleted, re-timed and linked. Moving, dragging and
  duplicating need a continuous block, and the timeline says so.
- **Tags may overlap.** Playback, export defaults and the timeline tint use the first tag that
  contains the frame.
- **Layer transforms skip frames where the layer has no pixels.** Transforming an empty layer
  reports "The layer is empty".
- Onion-skin ghosts are cached. The first display after a structural change composites each
  ghost frame once (about 100 ms for one ghost each side at 1024² × 20 layers on the VM).
- **Saving large projects** PNG-compresses every cel, about 10 ms per 1024² cel per core
  (miniz deflates the whole cel even when it is mostly empty). Cels are compressed on all
  cores: 160 such cels take 1.3 s to save and 0.5 s to load on the 2-vCPU VM. Autosave writes
  to recovery files in the background.

## Phase 6 asset tools (design limits)
- Up to three terrains over a base, and textures of up to 4×4 tiles. Engines without per-cell
  variants (most autotile rule systems) use phase variant 0 or need the documented
  `x mod phases, y mod phases` rule. With Margins off in the sandbox, a level painted straight
  onto a much lower one shows its edge with no level in between (Margins is on by default).
- AI background removal (slicer, parallax) is BiRefNet followed by `refineCutout`, which gives
  hard pixel-art edges, brings back dropped parts that differ from the local background and
  opens background gaps the mask filled in. A sprite part with the same colour as the
  background next to it cannot be told apart from it.
- Clear Sky "Sky against ground" needs ground at the bottom of the layer. A scene whose bottom
  is mist that fades into the sky (one PC castle layer) keeps its sky; the dialog then says how
  little it cleared and suggests the AI cut-out, which handled that layer.
- Texture results depend on the prompt: SDXL with Pixel Art XL drew "lush green grass" as bushes
  on pale stone in three PC runs, while "dense short green grass lawn, solid green" gave a plain
  grass texture, so that is now the default.
- The atlas packer rotates only by 90° clockwise (TexturePacker's convention), never flips.

## Phase 7 integrations (design limits)

### MCP server
- No SSE stream resumption (`Last-Event-ID`). It is optional in 2025-11-25 and removed in
  2026-07-28. A dropped HTTP stream cancels its tool call (tasks keep running; poll them).
- A task cannot continue in another process: if the server that runs it is killed, the task
  reads as failed (its result is kept when it finished first).
- Each tool call starts `acestudio-cli`, so the CLI and the server never disagree. That costs
  a median of 2.8 ms per `info` call on the Linux build machine; Windows was not timed
  separately.
- Scripts from `app_run_lua` run on a copy, and their result is not applied if the user edits
  the document meanwhile; the tool then says so.
- Client setups: Claude Code 2.1.316 was run and connects to the server; Codex 0.166.0 reads
  the setup, but a Codex session (which needs a sign-in) was not started. Cursor and VS Code
  were not run; their setups follow their documentation (October 2026).

### Lua
- No network, processes or native modules. Files and `require`d modules only through the
  `ace` API and inside the allowed folders.
- In the app, Run Script works on a copy of the document; its result is dropped if you edit
  the document while it runs (run it again to apply it).

### Engine export
- Unity: the tiles use phase variant 0 only, and Tile Palettes are not generated. The optional
  Rule Tiles need 2D Tilemap Extras and cover Blob47 and Cardinal16 only; Wang16 uses
  AceStudio's dual grid. AceStudio's tile scripts use only Unity's built-in Tilemap module;
  they were checked in a project that also had Tilemap Extras installed.
- Tiled: sprite sheets get a `.tsx` with tile animations only when untrimmed and on a grid
  (horizontal, vertical or grid layout). The `.tsx` files were checked by tests, not loaded in
  Tiled itself.
- Godot keeps an isolated single cell empty in corner (Wang) mode. That is Godot's own
  behaviour, reproduced in `tests/engine`.

## Phase 8 commercial polish (limits)

### Licensing
- Enforcement is client-side and offline. Deleting both the trial file and its registry value
  restarts the trial, and a patched binary skips the check (SECURITY.md).
- Builds without the release public key (`ACESTUDIO_LICENCE_PUBLIC_KEY`) accept keys made with
  the public development key. The licence dialog and CI say so. The release key pair has not
  been generated yet; the publisher must do it (`tools/licence/README.md`).
- There is no online activation, seat counting or revocation list; an expiry date in the key
  is the only way to end a licence.

### Installer and distribution
- The installer and executables are **not code-signed**. Seen on the test PC: SmartScreen
  blocks the installer ("Windows protected your PC") until the user picks More info › Run
  anyway. A code-signing certificate is required before release.
- The interactive install (wizard pages) was exercised on the test PC only up to the EULA
  page; the full install, AI setup and uninstall were tested silently in CI.
- Per-user install only by default; an all-users install is possible from the command line
  (`/ALLUSERS`) but was not tested.
- Nightly installers are attached to a GitHub release, public while the repository is.
- The EULA (`packaging/windows/EULA.txt`) and PRIVACY.md are drafts for legal review.

### Accessibility
- The UI audit checks that every control has an accessible name and can be reached with Tab,
  on every window the menus open. It does not prove the result is pleasant with a screen
  reader: NVDA and Narrator were not run.
- The canvas itself is a picture; drawing needs a pointer. Everything else (menus, panels,
  dialogs, timeline, layers) works from the keyboard.

### Regression suite
- Projects from 0.1.0 render differently where groups have content: 0.2.0 changed group
  compositing on purpose (isolated groups). The compat test allows exactly that.
- Performance budgets are ceilings several times the measured times, chosen so slow CI
  runners do not fail; they catch large regressions, not small ones.

## 0.10.0 premium studio: unresolved

These are the issues still open after the 0.10.0 work. Each was found by running the feature,
or is a case that has not been run yet.

- **FLUX.2 [klein] on the test PC** (RTX 4070 Laptop, 8 GB):
  - Speed: 3.6–3.8 s per step at 1024² and 2.1–2.4 s at 512², with a peak of 4.4 GB.
  - Verified: HD generation, the Character Pack's front and back views, and style matching
    (goblin 87 %, sword 71 %).
  - The pixel-art LoRA gave pure noise at adapter weight 1.0, because it is an rsLoRA with
    alpha 128. It now runs at 0.05; that fix has not been re-run on the PC.
  - The 7×7 bestiary works on the PC: 49 whole creatures in 192 s, made as six 3×3 sheets.
    Several brutes still look alike.
  - Not yet measured on the PC:
    - the grid-versus-staged benchmark;
    - Pet to Pixel.
  - At Medium, a style's reference image leaks its subject: a goblin took the knight's
    armour and red cape.
- **Animate Sprite moves parts rigidly.** It cannot draw new poses (an arm swing, a turn).
  - A side-view character whose legs are drawn together is classified as an object, so it
    rocks rather than walks. This was the case for a 29×57 armoured knight. Set Body to Two
    legs to swing the lower half.
  - **Wing flap** folds everything above a line. On a generated HD dragon (test PC), the
    default span folded the head and neck with the wings. A span of 38–100 % kept the head
    still and flapped the near wing; the far wing, behind the head, stays still.
  - Selecting the wings first (the selection becomes the wing mask) gives exact control.
  - Sprites larger than 320 px are previewed from a reduced copy; the checks shown are the
    preview's. Opening or exporting renders at full size, which takes a few seconds for an
    HD sprite with many actions.
  - Generating flap keyframes with AI is not built.
- **Front/back views for animation** come from AI (Character Pack). Animate Sprite on a front
  view walks with the legs side by side, which reads as a march rather than a walk cycle.
- **GameMaker export** follows GameMaker's `_stripN` import convention. It has not been
  imported into GameMaker, which is not available here.
- **Phaser export** was checked in Phaser 3.90.0 and 4.2.1 (headless Chromium) for loading,
  animation frames, durations and looping. It was not checked in a shipped Phaser game.
- **World Builder.**
  - Maps are exported to Tiled `.tmx`; there is no direct Godot TileMapLayer or Unity
    Tilemap scene export yet. The `.tmx` was checked structurally (unit tests, Python TOML
    and XML parsing; a PC-made level re-rendered from the TMX alone matches the preview) and has
    not been opened in Tiled itself.
  - The map canvas re-renders the whole map on every edit. That is fine for typical levels
    (a few thousand cells) and slow for maps near the 4096×4096 limit.
  - The Terrain tool paints blob-47 and cardinal-16 sheets from the Tileset Generator (it
    reads the sheet's `.json`). Wang-16 (dual-grid) sheets are not supported in the map yet.
- **Claude Code over MCP (Test F)** has not run on the test PC: this session cannot type into
  a terminal there. The MCP tools are covered by CI.
- **Uninstall (Test G)** ran on the test PC: projects, settings and models were kept, and the reinstall
  worked. The new prompt to close a running AceStudio (`AppMutex`) has not been seen on the PC yet.
- **`connect claude --global`** prints the `claude mcp add` command instead of editing
  `~/.claude.json`, because Claude Code rewrites that file itself while running.
- **The VS Code and Claude Desktop user-wide paths** are the documented defaults (`%APPDATA%`).
  Portable or Insiders installs keep their settings elsewhere.
- **Acceptance tests A–G:** see `docs/devlog/2026-11.md` for what was run and with what
  result. Anything needing klein on the GPU is still open.

## Planned in later phases
- Plugin API (Phase 8 or later). Profiling of BiRefNet on CUDA: part of its 2.6 s is CPU
  pre/post-processing. FP16 weights, I/O binding and GPU-side resizing are to be measured in
  the Phase 8 profiling pass.
