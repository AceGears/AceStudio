# Known Issues and Limitations

Current as of 0.6.0. Everything that could be fixed in code for Phases 1–5 has been fixed and
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
- **Python:** the AI features need a 64-bit Python 3.10–3.14 installed. ONNX Runtime publishes no
  Windows wheels for 3.15 yet, and none for 32-bit, so a 32-bit Python is skipped or refused.
  Bundling an embeddable Python is part of Phase 8 packaging.
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
  On the 2-vCPU test VM, a full 1024×1024 × 20-layer recomposite takes about 63 ms with mixed
  blend modes and 42 ms with normal layers. That is roughly 15–20 ms on an 8-core PC. Strokes
  recomposite only their dirty region. A GPU compositor would have to reproduce every blend
  mode, mask, clip and adjustment exactly; these numbers do not justify that cost. Phase 8
  profiles again on real hardware.
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
  ghost frame once (about 240 ms for one ghost each side at 1024² × 20 layers on the VM).

## Phase 6 asset tools (design limits)
- Terrain levels are layers: each level is drawn over the one below it, so painting grass
  straight onto water shows a grass edge over water with no sand between. Paint the level
  below first where a margin is wanted (the Random map nests levels this way).
- Up to three terrains over a base, and textures of up to 4×4 tiles. Engines without per-cell
  variants (most autotile rule systems) use phase variant 0 or need the documented
  `x mod phases, y mod phases` rule.
- AI background removal (slicer, parallax) uses BiRefNet, which picks out the main objects in
  an image; on a sheet of many small sprites it may miss some. Picking the background colour
  by clicking is the exact method when the background is one colour.
- The atlas packer rotates only by 90° clockwise (TexturePacker's convention), never flips.

## Planned in later phases
- MCP, Lua and plugin features, and Godot/Tiled/Unity exporters for tilesets, atlases and UI
  kits (Phase 7). Phase 6 writes TexturePacker-style JSON and its own tileset/parallax JSON.
- BiRefNet on CUDA spends part of its 2.6 s on CPU pre/post-processing. FP16 weights, I/O binding
  and GPU-side resizing are to be measured in the Phase 8 profiling pass.
