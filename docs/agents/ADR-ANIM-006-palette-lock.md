# ADR-ANIM-006: Project palette lock

| | |
|---|---|
| Status | Accepted and built on 2026-11-15 (branch `agents/anim-next`). Core tested locally and in CI; CLI and app built and checked by CI only. |
| Builds on | ADR-ANIM-003 phase 10 (style consistency: "palette lock across a project; style reference for AI steps") |

## Context

Indexed colour mode already limits painting to the palette, but an RGBA project (the default, and what most sprite work uses because of translucent pixels) has nothing that keeps its colours together: smooth gradients, pasted images, generated in-betweens and motion clips, and imported frames all bring new colours, and the palette itself can drift while it is edited.

## Decisions

1. **A lock, not a colour mode.** `Document::paletteLocked()` works in any colour mode and keeps alpha as it is. It is stored as the `document.json` extension `"paletteLock": true`, so it needs no new schema and older builds keep (and ignore) it.
2. **Locked means the palette cannot change.** `setPaletteColor`, `addPaletteColor`, `removePaletteColor` and `replacePalette` (load, from image) are refused, and so is converting to grayscale. Unlocking is one undoable step.
3. **New pixels snap at the doors.** While locked, every way pixels enter a document through the Editor snaps their colour to the nearest opaque palette colour (squared RGB distance, alpha kept): painting (`PaintSession::constrain`, so every brush, shape, fill and gradient tool), `setCelImage`, image layers (paste, import), animation layers (in-betweens, motion clips, generated frames) and vector-shape rasterisation.
4. **Existing pixels change only when asked.** Locking does not touch what is already drawn. `Editor::conformToPalette` snaps every cel in one undo step; the app offers it when the lock is turned on and pixels are off the palette, and `palette check` (exit 1) reports them for scripts and CI.
5. **The style reference stays the style profile.** For AI generation, the existing style profiles (StyleProfile.h, "Lock Game Style") are the style reference: their palette, outline, shading and light are applied and then checked against the result. Results brought into a locked project are snapped to the project palette by decision 3.

## Built

| Piece | Where | Verified |
|---|---|---|
| `snapToPalette` (colour, image), `countOffPalette` | `Palette.h/.cpp` | `tests/test_palette_lock.cpp`: locally and in CI |
| Lock flag, `offPalettePixels` | `Document` | same |
| `setPaletteLocked`, `conformToPalette`, refusals, snapping in `setCelImage`, image and animation layers and vector shapes | `Editor` | same (undo, refusals, every door, conform counts and undo) |
| Painting snaps | `Paint.cpp` | paint case in `tests/test_palette_lock.cpp`: locally (full core build) and in CI |
| Saved and reloaded, schema unchanged | `ProjectIO` (extensions) | project case in the same file: locally (full core build) and in CI |
| CLI `palette show`, `check`, `lock`, `unlock`, `conform` | `cli/PaletteCommand.cpp` | `tests/cli_smoke.cmake` (its palette steps also replayed locally against a local CLI build) |
| Palette panel: Lock palette (offers to conform), Conform, edit buttons disabled while locked | `app/src/panels/PalettePanel.*` | CI build only; not used on a PC |

## Not built

- Snapping inside commands that compute new colours from existing ones without going through those doors: Merge Down with blend modes, transforms with smooth resampling, effects. They can leave the palette; `palette check` finds the pixels and Conform fixes them.
- A per-layer or per-tag palette; the lock is for the whole project.
- (Built: Lua `doc:palette_locked`, `lock_palette`, `off_palette`, `conform_palette` and the MCP tools `palette_show`, `palette_edit`.)
