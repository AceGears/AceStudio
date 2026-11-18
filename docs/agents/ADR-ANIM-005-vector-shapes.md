# ADR-ANIM-005: Vector shapes on cels (schema 5)

| | |
|---|---|
| Status | Accepted and built on 2026-11-15 (branch `agents/anim-next`). Core and CLI are tested; the app tools are built by CI and opened by the UI audit, not yet used on a PC. |
| Builds on | ADR-ANIM-003 phase 2 (vector shape layers, dithered gradient fill, stroke styles) |

## Context

The brief asks for vector shapes (a pen with anchors, rectangles, ellipses, auto-close) rasterised pixel-perfectly, dithered gradient fills and stroke styles. AceStudio already has raster Line, Rectangle, Ellipse, Polygon, Curve and Gradient tools (ordered Bayer dithering included), but what they draw cannot be edited afterwards.

## Decisions

1. **Shapes live on a cel, the pixels stay ordinary.** A raster layer's cel can carry a list of shapes (`Document::vectorShapes(layer, frame)`); the cel's pixels are their rasterisation. Compositing, blend modes, masks, export, engine export, QA and every raster tool keep working unchanged. No new layer kind was added: a vector layer kind would have touched every place that switches on `LayerKind`.
2. **One rasteriser, built from the existing exact primitives.** `vectors::rasterize` uses `linePoints`, `curvePoints` (cubic Bezier), `ellipsePoints`, `polygonMask` and `bayer4` from `Raster.h`: no anti-aliasing, whole pixels, pixels replaced (not blended). Fills come first, then the stroke. Strokes are a square pen of 1–32 px stamped along the one-pixel outline; dashes and dots are counted along that outline, so they are the same on every zoom and export. Gradient fills use the same formula as the Gradient tool (`gradientT`, `gradientColor` in `Raster.h`; `drawGradient` now calls them), ordered-dithered by default so no new colours appear.
3. **Editing redraws the cel.** `Editor::setVectorShapes` replaces the shapes and the cel's pixels in one undo step. Hand painting on a vector cel is kept only until the shapes are edited; `Editor::rasterizeVectorShapes` (Layer › Rasterize Vector Shapes, `vector rasterize`) drops the shapes and keeps the pixels. Linked cels are refused (unlink first), so one cel's shapes never silently redraw another frame.
4. **Shapes follow the document.** They move with their frame on insert, move and delete (`Document::remapFrames`), go with their layer, and come back on undo (frame delete and layer delete both snapshot them).
5. **Schema 5 only when used.** `document.json` gains `"vectors": [{layer, frame, shapes}]`; `requiredSchemaVersion` is 5 only when a document has shapes, so other files stay readable by older builds. Every shape is validated on load (kinds, point counts, finite coordinates, width, dashes), and an invalid list makes the file unreadable rather than partly read.

## Built

| Piece | Where | Verified |
|---|---|---|
| Model, validation, rasteriser, outline order, JSON | `core/include/acestudio/core/Vector.h`, `core/src/Vector.cpp` | `tests/test_vector.cpp` (exact rectangle pixels, connected outlines, widths, dashes, dots, Bezier handles, closed and auto-closed fills, dithered gradients use two colours, ellipse symmetry, draw order, JSON round trip, refusals): run locally and in CI |
| Shared gradient formula | `Raster.h` (`gradientT`, `gradientColor`), `Paint.cpp` | the Gradient tool's existing tests in CI |
| Document, undo, frames, layers, rasterise | `Document`, `Editor` | `tests/test_vector.cpp` (undo/redo, insert/delete frames, delete layer, rasterise, linked cel refused): locally and in CI |
| Project file (schema 5) | `ProjectIO` | project case in `tests/test_vector.cpp`: locally (full core build) and in CI |
| CLI `vector show`, `vector add`, `vector clear`, `vector rasterize` | `cli/VectorCommand.cpp` | `tests/cli_smoke.cmake` (its vector steps also replayed locally against a local CLI build) |
| App: "Keep as vector shape" for Line, Rectangle, Ellipse, Polygon (auto-closed) and Curve (its control points become the anchor handles); stroke style and fill in the Tools panel; Layer › Rasterize Vector Shapes | `ToolState`, `ToolsPanel`, `DocumentView`, `MainWindow` | CI build and UI audit only; not drawn with by hand |

## Not built

- Editing a shape after it is drawn: selecting it, dragging anchors and handles, changing its colours. Shapes are added from the tools and removed by `vector clear` (or undo); the CLI and the file hold the full model.
- A pen tool that adds any number of Bezier anchors in one path (the Curve tool makes one segment with handles; the Polygon tool makes straight multi-point paths).
- (Built on 2026-11-15: Lua `doc:vector_shapes`, `set_vector_shapes`, `rasterize_vectors` and the MCP tools `vector_show`, `vector_edit`.)

## Consequences

- Files with shapes need schema 5; files without them are unchanged.
- A cel with shapes is redrawn whole on each edit; pixels painted on it by hand are lost at the next shape edit unless it is rasterised first (the app says so once per document view).
