# Known Issues and Limitations

Current as of 0.5.1 (Phases 1–4 complete). Items here are known; nothing is listed as done
that has not been run.

## Not yet verified
- **High-DPI on Windows:** the UI was checked at 150% and 200% with `QT_SCALE_FACTOR` on Linux
  (crisp icons, no overlapping or clipped controls). The packaged app has been used on the
  test PC at its own display scaling, but per-monitor DPI changes (dragging the window between
  monitors with different scaling) have not been tried.
- **PCs without the Visual C++ runtime:** the package now ships the runtime DLLs next to the
  executables, and CI checks that every runtime DLL any binary imports is present. Starting
  the app on a clean Windows install without the redistributable has not been tried.
- **Real-display frame rate** on Windows is not measured (only compositing times; see below).

## Editor limitations
- No interactive free-transform handles (scale/rotate by dragging). The Move tool drags and
  nudges selections. Flip, rotate, RotSprite and scale are in the Image menu and apply to the
  selection or layer.
- Layer transforms skip frames where the layer has no pixels; transforming an empty layer
  reports "The layer is empty".
- Compositing runs on the CPU. Large areas are split across threads, giving identical output.
  On the 2-vCPU test VM, a full 1024×1024 × 20-layer composite takes 67 ms (115 ms before
  threading), and the time drops further with more cores. Strokes recomposite only the dirty region. GPU
  compositing is not implemented.
- Pixels outside the canvas (from Keep Canvas Size, RotSprite Expand to fit or canvas
  resizes) stay on the layer but are not shown or exported until the canvas grows. There is
  no "fit canvas to content" command yet.
- Masks are edited with the same brushes as pixels; there is no mask-only overlay colour (the
  layer shows through the mask as it composites).
- Adjustment layers offer Brightness/Contrast, Hue/Saturation, Invert, Posterize and Threshold
  only (no Curves/Levels).
- Tileset, MCP, Lua and plugin features are not implemented yet (Phases 6–7); their menus
  show a disabled entry naming the planned phase.
- Workspace presets are not implemented; layout persistence and Reset Layout are.

## Animation limitations
- **Linked cels** (one image shared by several frames) are not implemented. Duplicated frames
  are independent copies.
- Multi-frame selection in the timeline is one contiguous block (a Ctrl-click with gaps
  selects the whole span).
- Onion-skin ghosts are cached. The first display after a structural change still composites
  every ghost frame: about 240 ms for one ghost each side at 1024×1024 with 20 layers on the
  2-vCPU VM.
- Tags may overlap. Playback, export defaults and the timeline tint use the first tag that
  contains the frame.
- GIF export: at most 256 colours per frame (median-cut reduction beyond 255), 1-bit
  transparency (alpha < 128 becomes transparent) and 10 ms duration steps (minimum 20 ms, as
  browsers treat shorter delays as 100 ms). Use APNG or WebP for exact output.

## AI limitations
- **GPU coverage:** everything was verified on one PC, an RTX 4070 Laptop (8 GB, driver 621.01).
  - SDXL at 1024², 30 steps, with model CPU offload: ~34 s warm, 50–56 s cold.
  - BiRefNet on CUDA: 2.6 s warm.
  - Real-ESRGAN 512→2048: 3.1 s.

  12 GB+ NVIDIA GPUs, which keep SDXL resident, and AMD/Intel GPUs on DirectML have not been tried.
  The Models dialog's VRAM figures are estimates and say so.
- **BiRefNet needs a lot of memory on CPU:** BiRefNet Lite exceeded 6 GB RAM at its fixed
  1024×1024 input and was killed on a 7 GB VM. Without a suitable GPU, background removal may
  fail the same way (the app reports it and recovers).
- **ONNX on DirectML:** onnxruntime-directml 1.24.4 failed inside a fused node when running
  BiRefNet on the test PC. AceStudio falls back to fewer fusions and then the CPU (~16 s per
  image). NVIDIA GPUs with driver R580+ use CUDA instead.
- **BiRefNet on CUDA** spends much of its 2.6 s on CPU pre/post-processing and host↔device
  copies. I/O binding and GPU-side resizing are left for the Phase 8 profiling pass.
- **NVIDIA drivers older than R580** get PyTorch cu126 and keep ONNX models on DirectML or the CPU
  (current onnxruntime-gpu builds need CUDA 12.8+ or 13). This path and the CPU-only PyTorch
  profile have not been installed on a Windows PC yet; the cu130 profile has.
- **Python:**
  - The AI features need a 64-bit Python 3.10–3.14 installed. ONNX Runtime publishes no Windows
    wheels for 3.15 yet, and no 32-bit wheels exist, so a 32-bit Python is skipped or refused.
  - Python is not bundled; an embeddable Python is planned for packaging (Phase 8).
  - Choosing a specific Python installs packages into that environment.
- **Small canvases:** generation runs near 1024² and is reduced with nearest neighbour.
  - At 64×64 that is a 16× reduction, twice Pixel Art XL's natural ~8× pixel size.
  - "Frame the subject" crops to the subject first, which helps when the background is plain;
    busy backgrounds are left uncropped.
  - 128×128 canvases fit the LoRA best.
- Generation shows no live preview while it runs.
- Upscaling works on the visible frame only; only ×4 models are catalogued. Real-ESRGAN smears
  small pixel art (expected for that model); use Image › Scale instead.
- SDXL Base 1.0 has pinned SHA-256 hashes for its 4 weight files only. Its other 14 small files
  (configs, tokenizers) are pinned to the Hugging Face commit and checked by size; their hashes
  are recorded on first download.

## Cosmetic
- The Layers panel's Merge button stays enabled when there is nothing to merge into. It reports
  why instead of acting; the Layer menu item is disabled correctly.
- On a short screen (about 820 px tall) the Palette dock needs scrolling to reach its buttons.
