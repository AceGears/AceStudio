# Known Issues and Limitations

Current as of 0.2.0 (Phase 2a).

## Not yet verified
- **Not yet run on real Windows or with MSVC.** Core, CLI and tests cross-compile for Windows
  with MinGW-w64 and all pass under Wine, which exercises the Win32 file code
  (`MoveFileExW`, `FlushFileBuffers`, `_wfopen`, `SHGetKnownFolderPath`). The Qt app has not
  been built for Windows, and Wine is not a substitute for NTFS/antivirus behaviour on a real
  machine. The CI Windows job is written but has not run.
- **Qt 6.12 not yet compiled against.** Code targets Qt ≥ 6.4 APIs; `jurplel/install-qt-action`
  availability of 6.12 is unverified.
- **High-DPI** behaviour has not been checked at 150%/200% Windows scaling.

## Functional limitations (by design for this phase)
- Phase 2b tools are not implemented yet: gradient, curve, polygon, lasso, magic wand, clone,
  dithering/custom brushes, RotSprite, outline, shading, tiled drawing. See ROADMAP.md.
- Selection is rectangular only and is not part of undo history.
- Pencil writes colours exactly (no alpha blending). Painting a translucent colour replaces
  the pixel rather than blending over it.
- Duplicating group layers is not supported yet (raster layers only).
- Imported PNGs larger than the canvas are cropped to the canvas.
- No colour-mode conversion: indexed/grayscale constrain new paint but existing pixels are not
  converted when the mode changes.
- A full recomposite of very large documents is CPU-bound (1024×1024 × 20 layers ≈ 109 ms on
  the 2-vCPU test VM). Strokes stay fast because only the dirty region is recomposited, but
  undo, frame changes and layer property changes on huge documents will feel slow until GPU
  compositing lands.
- Single-key tool shortcuts are fixed; shortcut customisation is Phase 2b.
- No AI, tileset, MCP, Lua or plugin functionality yet; those menus show one disabled entry
  naming the planned phase.
- Workspace presets are not implemented; layout persistence and Reset Layout are.

## Cosmetic
- The layer action buttons wrap tightly when the Layers dock is narrow.
- Clicking the opacity slider track moves it by a page step rather than jumping to the click
  point (standard Qt behaviour).
- Long file paths in the Properties panel wrap across several lines.
- The Log panel requests the Consolas font; other platforms fall back to a default monospace.
