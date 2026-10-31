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
    steps, background removal included): 46–49 s per frame warm, ~73 s for the first.
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
- **FP8 weight storage** is used automatically for pose-controlled generation on GPUs under
  10 GB, so the UNet and ControlNet fit in VRAM. Without it, a frame took ~14 minutes on an
  8 GB card. No visible difference was seen in the 64×64 results, but none was measured either.
  `weights: "full"` in the job parameters turns it off.
- **Cancelling while models load:** loading SDXL cannot be interrupted. A cancel during loading
  restarts the worker after 10 s, and the next job loads the models again.
- **SDXL often draws several objects** ("sprite sheet" style) even when asked for one. The
  animation pipeline crops every frame to its pose framing box. For single images, describe one
  object and use "Frame the subject".
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
  Without it, generation shows progress only. The Generate Animation dialog shows each frame
  when it finishes, not a live preview.
- **Animation consistency scores** compare every frame with frame 1 for ground line, height,
  palette, colour mix and silhouette. They catch jumps in shape and colour, not identity: two
  different knights in similar armour score as consistent. Reference images (IP-Adapter) are
  what keep the identity.
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

## Phase 5 items not yet run with real models
- **Auto-Correct** (ground line, height, palette remap) is covered by core tests. None of the
  real runs produced a frame that only needed correcting; flagged frames needed regenerating.
- **Run and jump cycles** have only been generated with the tiny test model, not SDXL.

## Planned in later phases
- Tileset, MCP, Lua and plugin features (Phases 6–7). Their menus show a disabled entry naming
  the phase.
- BiRefNet on CUDA spends part of its 2.6 s on CPU pre/post-processing. FP16 weights, I/O binding
  and GPU-side resizing are to be measured in the Phase 8 profiling pass.
