# Known Issues and Limitations

Current as of 0.2.0 (Phase 2a).

## Not yet verified
- **Windows desktop app not yet used interactively.** CI builds everything with MSVC and
  `/W4 /WX`, runs all tests on Windows and packages the app with `windeployqt`, but nobody has
  yet launched and drawn with the packaged app on a Windows desktop.
- **CI builds against Qt 6.8.3, not 6.12.** As of 2026-10-29 the aqtinstall package index used by
  CI lists Qt up to 6.11.3, and 6.11/6.12 metadata downloads fail checksum verification; CI tries
  6.12 → 6.11 → 6.8 and reports which it used. Building locally against a Qt 6.12 install from
  the official Qt installer is expected to work (code targets Qt ≥ 6.4) but is unverified.
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
