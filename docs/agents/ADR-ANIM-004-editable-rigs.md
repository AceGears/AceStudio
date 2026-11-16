# ADR-ANIM-004: Editable rigs saved in the project (schema 4)

| | |
|---|---|
| Status | Accepted and built on 2026-11-15 (branch `agents/rig`). Core and CLI are tested; the app panel and canvas overlay are verified by CI and the UI audit only (Qt is not in the local toolchain), not yet used on a PC. |
| Builds on | ADR-ANIM-001 (part rig, section 4 proposed schema 4), ADR-ANIM-003 (phase 3, "auto and manual rigging") |
| Replaces | ADR-ANIM-001 section 4's file layout (`rig/rig.json` plus part PNGs). See decision 1. |

## Context

The part rig of ADR-ANIM-001 is rebuilt from the sprite every time a clip is made and thrown away afterwards. Users cannot see it, fix a bad cut, move a joint, add a bone for a tail or a held sword, or keep the rig with the project. ADR-ANIM-003 lists saved, editable rigs as the next stage of phase 3.

## Decisions

1. **Parts are the document's own raster layers.** A rig is a bone tree plus a map *layer → bone* and IK chains. It holds no pixels. ADR-ANIM-001 proposed separate part PNGs (`rig/parts/P<id>.png`, `_hidden.png`) in the archive; that would have been a second, invisible copy of the art that the drawing tools cannot reach and exporters would have to learn. With parts as layers:
   - parts are painted, fixed, reordered and hidden with the normal tools, and the hidden fill behind a part is ordinary pixels on its layer;
   - stacking order is layer order (no separate z);
   - export, onion skin, tags and linked cels need no change;
   - the archive gets no new entries.
2. **Stored as `"rig"` in `document.json`.** Schema 4 is required only when a document has a rig (`requiredSchemaVersion`), so files without one stay readable by older builds. The rig object has its own `version` (1); a newer one is refused with an "update AceStudio" message, never half read.
3. **Rest geometry is absolute.** Each bone stores its head, length and direction in canvas pixels at rest. Reparenting never moves a bone. Angles are radians, clockwise on screen from +x (y points down), the convention the part rig already uses.
4. **Poses are not stored; they are baked.** A pose is per-bone turns relative to the parent, plus a move of every root. `bakePose(doc, restFrame, frame, pose)` draws the part cels of a *rest frame* into another frame with the part rig's renderer (whole-pixel moves, RotSprite turns from an 8× Scale2x copy, binary alpha, no new colours). Reading from a rest frame means baking never compounds: baking the same pose twice gives the same frame. Keyframed bone tracks are a later stage (ADR-ANIM-003 phase 8).
5. **IK is two-bone and analytic.** A chain names its tip; the tip and its parent are solved so the tip's tail reaches the target. Segment one runs from the upper bone's head to the tip's head (they need not touch), segment two along the tip. `bend` +1 puts the joint clockwise of the line to the target. Limits and locks are applied after solving, and the tip is then aimed from where the joint really is.
6. **Edits are values.** Every edit (`rigs::addBone`, `reparentBone`, `setBoneGeometry`, …) works on a copy and keeps it only if the whole rig validates. `Editor::setRig` replaces the rig as one undo step; consecutive calls with the same merge key (a spin-box drag on one bone) merge. Rigs are small (≤ 256 bones, ≤ 64 chains), so whole-rig snapshots are cheap.
7. **Auto-rig reuses the animator's analysis.** `rigs::autoRig` runs `analyzeSubject` and `buildGaitRig` on the chosen layer, makes a group of part layers above it (the source layer is hidden, not deleted), a `body` root bone, per leg a thigh, shin (when the legs are long enough to have a knee) and foot bone, per arm a bone, and a leg IK chain per leg. Bodies the gait rig cannot cut (blobs, flyers, swimmers, objects) are refused with a message: rig those by hand. The composite of the new layers equals the sprite pixel for pixel (tested on `rogue64`).

## Built

| Piece | Where | Verified |
|---|---|---|
| Rig model, validation, edits, FK, two-bone IK, JSON | `core/include/acestudio/core/Rig.h`, `core/src/Rig.cpp` | `tests/test_rig.cpp`, run locally (zig c++, Catch2 stand-in) and in CI |
| Document and project (schema 4), removing a layer drops its binding | `Document`, `ProjectIO` | project round-trip case in `tests/test_rig.cpp`: CI only (miniz is not in the local toolchain) |
| Undo (`Editor::setRig`, merge keys) | `Editor` | `tests/test_rig.cpp` |
| Auto-rig, bake pose | `core/include/acestudio/core/RigTools.h`, `core/src/RigTools.cpp` | `tests/test_rig.cpp` (composite equality, rest pose exact, no compounding, root move, colours kept) |
| CLI `rig show`, `rig auto`, `rig pose` | `cli/RigCommand.cpp` | `tests/cli_smoke.cmake`: CI only |
| Rig panel (bone tree, name, parent, head, length, angle, limits, lock, parts, IK chains, working pose, rest frame, bake, remove), Animation › Auto-Rig Current Layer | `app/src/panels/RigPanel.*`, `MainWindow` | CI build and UI audit only |
| Bone overlay on the canvas (the working pose; selected bone highlighted) | `CanvasWidget::setBones` | CI build only; not looked at on a screen |

## Not built (next stages)

- Dragging bones and IK targets on the canvas. Bones are edited with the panel's fields; the canvas only shows them.
- Stored poses or keyframed bone tracks with curves (ADR-ANIM-003 phase 8). A pose lives in the panel until it is baked.
- Auto-rig of flyers and swimmers (their modules in ADR-ANIM-001 cut wings and tails, but do not yet produce bones for them), and of arms that the silhouette analysis does not find.
- Skinning (rows, columns, mesh) of a part across two bones: every part is rigid on one bone.
- Lua, MCP and AI-assistant access to rigs.
- Retargeting BVH/glTF motion onto a saved rig (Motion Studio still builds its own rig per clip).

## Consequences

- Files with a rig need AceStudio with schema 4; files without one are unchanged.
- Auto-rig adds a group and a handful of layers; undo removes them again in one step.
- Baking writes ordinary cels, so everything downstream (export, engine export, QA) is unchanged.
- A rig cut on one frame applies to that frame's parts. Other frames of the part layers are empty until a pose is baked into them.
