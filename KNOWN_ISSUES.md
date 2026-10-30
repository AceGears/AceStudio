# Known Issues and Limitations

Current as of 0.5.0 (Phase 4).

## Not yet verified
- **Windows desktop app only launched so far.** CI builds everything with MSVC and `/W4 /WX`,
  runs all tests on Windows and packages the app; the packaged build opened on a Windows 11 PC,
  but drawing, saving, export and AI on that desktop have not been recorded yet.
- The CI package includes `vc_redist.x64.exe` but does not run it: on a PC without the Visual
  C++ 2015-2022 runtime the app will not start until it is installed (the installer, Phase 8,
  will handle this).
- **CI builds against Qt 6.8.3, not 6.12.** As of 2026-10-29 the aqtinstall package index used by
  CI lists Qt up to 6.11.3, and 6.11/6.12 metadata downloads fail checksum verification; CI tries
  6.12 → 6.11 → 6.8 and reports which it used. Building locally against a Qt 6.12 install from
  the official Qt installer is expected to work (code targets Qt ≥ 6.4) but is unverified.
- **High-DPI** behaviour has not been checked at 150%/200% Windows scaling.

## Functional limitations (by design for this phase)
- Phase 2c tools are not implemented yet: curve, polygon, clone, custom brushes, shading,
  tiled drawing, layer masks/clipping/adjustment layers, .ase palettes. See ROADMAP.md.
- Selections are not part of undo history, and a selection cannot yet be moved as a floating
  piece (transforms apply in place, re-centred on the selection bounds).
- Changing the canvas size clears the selection.
- Layer transforms skip frames where the layer has no pixels; transforming an empty layer
  reports "The layer is empty".
- RotSprite keeps the canvas size, so corners of rotated content can be clipped at the canvas
  edge.
- Pencil writes colours exactly (no alpha blending). Painting a translucent colour replaces
  the pixel rather than blending over it.
- Duplicating group layers is not supported yet (raster layers only).
- Imported PNGs larger than the canvas are cropped to the canvas.
- A full recomposite of very large documents is CPU-bound (1024×1024 × 20 layers ≈ 109 ms on
  the 2-vCPU test VM). Strokes stay fast because only the dirty region is recomposited, but
  undo, frame changes and layer property changes on huge documents will feel slow until GPU
  compositing lands.
- No AI, tileset, MCP, Lua or plugin functionality yet; those menus show one disabled entry
  naming the planned phase.
- Workspace presets are not implemented; layout persistence and Reset Layout are.

## AI limitations (0.5.0)
- **Not yet run on a GPU.** SDXL generation was verified only with a tiny randomly initialised
  model (real Diffusers code path, meaningless images). Speed, quality and VRAM use on a 12 GB
  NVIDIA GPU are unmeasured; the VRAM figures in the Models dialog are estimates and say so.
- **BiRefNet needs a lot of memory on CPU:** BiRefNet Lite exceeded 6 GB RAM at its fixed
  1024×1024 input and was killed on the 7 GB test VM. On a machine without a suitable GPU,
  background removal may fail the same way (the app reports it and recovers).
- **Unverified download details:** the SDXL and Pixel Art XL file lists could not be checked
  against Hugging Face from the build environment, so their hashes are recorded at first
  download rather than pinned. The PyTorch `cu128`/`cpu` index pins are also unchecked. If a
  file or wheel is missing, the download or install fails with the server's message.
- **Python is not bundled yet:** AI features need Python 3.10+ installed (or configured in Set
  Up AI Runtime). Bundling an embeddable Python is planned for packaging (Phase 8). Choosing a
  specific Python installs packages into that environment.
- Generation shows no live preview while it runs; the LoRA strength is fixed at 1.0 in the UI.
- Remove Background adds its layer on the frame that is current when the job finishes.
- Inserting an AI result as a layer is two undo steps (new layer, then pixels).
- Upscaling works on the visible frame only; only ×4 models are catalogued.
- Real-ESRGAN smears small pixel art (expected for that model); use Image › Scale instead.

## Animation limitations (0.4.0)
- Onion skin recomposites the ghost frames on every frame change: about 14 ms for one ghost
  each side at 256×256 with 20 layers, but about 240 ms at 1024×1024 (2-vCPU test VM), so
  playback with onion skin on large HD canvases stutters. Ghosts are not cached yet.
- Tags may overlap; playback, export defaults and the timeline tint use the first tag that
  contains the frame.
- GIF export: at most 256 colours per frame (median-cut reduction beyond 255), 1-bit
  transparency (alpha < 128 becomes transparent) and 10 ms duration steps (minimum 20 ms, as
  browsers treat shorter delays as 100 ms). Use APNG or WebP for exact output.
- Frames can be reordered one at a time; multi-frame selection and cel-level linking are not
  implemented.
- The Preview dock shows the whole canvas; it has no checkerboard behind transparent pixels.
- Export presets are not remembered between exports yet.

## Cosmetic
- The layer action buttons wrap tightly when the Layers dock is narrow.
- Clicking the opacity slider track moves it by a page step rather than jumping to the click
  point (standard Qt behaviour).
- Long file paths in the Properties panel wrap across several lines.
- The Log panel requests the Consolas font; other platforms fall back to a default monospace.
