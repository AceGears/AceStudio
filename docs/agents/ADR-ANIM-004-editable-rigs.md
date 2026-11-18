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
4. **Poses are not stored; they are baked.** A pose is per-bone turns relative to the parent, plus a move of every root. `bakePose(doc, restFrame, frame, pose)` draws the part cels of a *rest frame* into another frame with the part rig's renderer (whole-pixel moves, RotSprite turns from an 8× Scale2x copy, binary alpha, no new colours). Reading from a rest frame means baking never compounds: baking the same pose twice gives the same frame.
8. **Keys are poses on frames (rig version 2).** The rig stores its rest frame and a sorted list of keys, one per frame: a pose and a curve (`linear`, `ease-in`, `ease-out`, `ease-in-out`, `stepped`, or a CSS-style cubic `bezier`). `rigs::poseAt(rig, frame)` holds the first and last key's pose outside the keyed range and between two keys eases every turn and the root move with the earlier key's curve. `rigs::bakeKeys(doc, first, last)` bakes `poseAt` into every frame of the range from the rest frame and skips the rest frame itself, so the parts at rest are never overwritten. Keys move with their frames when frames are inserted, moved or deleted (`Document::remapFrames`); deleting a frame drops its key and undo brings it back. A rig without keys and with rest frame 0 is still written as version 1, so older readers open it.
5. **IK is two-bone and analytic.** A chain names its tip; the tip and its parent are solved so the tip's tail reaches the target. Segment one runs from the upper bone's head to the tip's head (they need not touch), segment two along the tip. `bend` +1 puts the joint clockwise of the line to the target. Limits and locks are applied after solving, and the tip is then aimed from where the joint really is.
6. **Edits are values.** Every edit (`rigs::addBone`, `reparentBone`, `setBoneGeometry`, …) works on a copy and keeps it only if the whole rig validates. `Editor::setRig` replaces the rig as one undo step; consecutive calls with the same merge key (a spin-box drag on one bone) merge. Rigs are small (≤ 256 bones, ≤ 64 chains), so whole-rig snapshots are cheap.
7. **Auto-rig reuses the animator's analysis.** `rigs::autoRig` runs `analyzeSubject` and `buildGaitRig` on the chosen layer, makes a group of part layers above it (the source layer is hidden, not deleted), a `body` root bone, per leg a thigh, shin (when the legs are long enough to have a knee) and foot bone, per arm a bone, and a leg IK chain per leg. Bodies the gait rig cannot cut are tried as a flyer (`buildFlightRig`, the flap animator's wing cut: a body bone and one bone per wing from its shoulder to its tip) or a swimmer (`buildSwimRig`, the swim animator's cut: a body bone from the tail stalk to the head, a tail-fin bone and one bone per other fin) when the analysis reads them so. `AutoRigBody` (`--as legs|wings|fins`, the panel's combo) forces one cut: a dragon reads as legged, so its wings need Wings. The two cuts were extracted from `flightRender` and `swimRigRender` without changing them (the part-rig and sprite-motion tests pass unchanged). Blobs and objects are refused: rig those by hand. The composite of the new layers equals the sprite pixel for pixel (tested on `rogue64`).

## Built

| Piece | Where | Verified |
|---|---|---|
| Rig model, validation, edits, FK, two-bone IK, JSON | `core/include/acestudio/core/Rig.h`, `core/src/Rig.cpp` | `tests/test_rig.cpp`, run locally (zig c++, Catch2 stand-in) and in CI |
| Document and project (schema 4), removing a layer drops its binding | `Document`, `ProjectIO` | project round-trip case in `tests/test_rig.cpp`: CI only (miniz is not in the local toolchain) |
| Undo (`Editor::setRig`, merge keys) | `Editor` | `tests/test_rig.cpp` |
| Auto-rig, bake pose | `core/include/acestudio/core/RigTools.h`, `core/src/RigTools.cpp` | `tests/test_rig.cpp` (composite equality, rest pose exact, no compounding, root move, colours kept) |
| Auto-rig of wings and fins (`AutoRigBody`, `buildFlightRig`, `buildSwimRig`) | `RigTools`, `RigFlight.cpp`, `RigSwim.cpp`, `RigModules.h` | `tests/test_rig.cpp` on `dragon128` (Wings) and `fish57` (Auto and Fins): composite equality, a turned limb redraws only its part; `test_part_rig` and `test_sprite_motion` unchanged; run locally and in CI |
| Keys with curves, `poseAt`, `bakeKeys`, rest frame in the rig, keys following frame edits | `Rig`, `RigTools`, `Document::remapFrames`, `Editor::removeFrame` | `tests/test_rig.cpp` (curves, interpolation, frame insert/move/delete, bake), run locally and in CI |
| CLI `rig show`, `rig auto`, `rig pose`, `rig key`, `rig unkey`, `rig rest`, `rig bake` | `cli/RigCommand.cpp` | `tests/cli_smoke.cmake` (keyed bake equals the plain pose on the last key, rest frame untouched, in-between differs): CI only |
| Rig panel keys (key list, curve, Set Key, Delete Key, Bake Keys, the working pose follows the keys on a frame change, rest frame saved with the rig) | `app/src/panels/RigPanel.*` | CI build and UI audit only |
| Rig panel (bone tree, name, parent, head, length, angle, limits, lock, parts, IK chains, working pose, rest frame, bake, remove), Animation › Auto-Rig Current Layer | `app/src/panels/RigPanel.*`, `MainWindow` | CI build and UI audit only |
| Bone overlay on the canvas (the working pose; selected bone highlighted) | `CanvasWidget::setBones` | CI build only; not looked at on a screen |
| Dragging on the canvas: a bone's tip turns it (an IK chain's tip reaches), a root's joint moves the figure, Ctrl+drag edits rest geometry at the rest pose. The canvas grabs a press only within 7 screen pixels of a joint or tip; other presses go to the tool | `CanvasWidget::setBonesInteractive`, `RigPanel::boneGrabbed/boneDragged` | CI build only; not dragged by hand |
| Parts bent across a joint (`Skin`, `Rig::skins`, `skinLayer`; dropped when the child bone, the binding or the layer goes, or the child is reparented; saved as rig version 3 only when used). Each source point goes to the blend (1 - w) A(p) + w B(p) of the two bones' moves, w a smoothstep across a band either side of the joint along the direction halfway between the bones at rest; drawn by inverse mapping (Newton's method from both bones' inverse points, the point nearer the child winning where it folds) from the 8x Scale2x copy. At rest, or when the bones turn together, the rigid path draws it | `Rig`, `RigTools.cpp` (`renderSkinned`), `Document`, `Editor` (layer undo), `cli/RigCommand.cpp` (`rig skin`), `mcp/AceTools.cpp`, `app/src/panels/RigPanel.cpp` (Bends with) | `tests/test_rig_skin.cpp` (rest and turning together exact; bent 0.5, 1.2 and -0.9 rad: one piece, no holes, pixel count kept within 80-130%, only the sprite's colours, the upper arm unmoved, the end at the forearm's tail; validation, edits, JSON), run locally; `tests/cli_smoke.cmake` run locally with a placeholder GIF writer; a striped test arm looked at bent from -46 to 92 degrees. The MCP action and the panel's Bends with box are compiled by CI only (the panel also runs in the UI audit) |

## Not built (next stages)

- A curve editor: the panel offers the named curves and Bezier with its default points; custom Bezier points are set with `rig key --curve bezier:x1,y1,x2,y2`. Keys are not shown on the Timeline strip.
- Arms that the silhouette analysis does not find (it looks only beside the legs) are found only
  when marked by hand: `AutoRigOptions::arms` / `rig auto --arms` (built 2026-11-17, tested in
  `tests/test_rig_motion.cpp` and the CLI smoke test; no app UI to draw the boxes yet). A rig that
  has both legs and wings is not built (Wings leaves the legs in the body).
- A swimmer's body bending: auto-rig makes it one rigid part (the swim animator bends it column by column, which a saved rig cannot yet express).
- Skinning across more than two bones, painted weights or a mesh. (Built on 2026-11-18: a part
  bends across the joint with one child of its bone, `Rig::skins`, rig version 3; see below.)
- (Built on 2026-11-15: Lua `doc:rig()`, `set_rig`, `auto_rig`, `pose_rig`, `key_rig`, `unkey_rig`, `bake_rig`, and the MCP tools `rig_show` and `rig_edit`, which run the CLI. Lua is tested locally and in CI; the MCP tools are compiled, not called by a test.)
- (Built on 2026-11-17, CLI only: `rig motion` keys a BVH/glTF/preset motion onto a saved rig; see
  ADR-ANIM-002 section 3.3. Motion Studio still builds its own rig per clip, and the app has no
  button for it yet. On 2026-11-18 Lua `doc:rig_motion` and the MCP tool `rig_motion` were added,
  with every option of the command; Lua is tested in `tests/test_script.cpp`, the MCP tool is
  compiled, not called by a test.)

## Consequences

- Files with a rig need AceStudio with schema 4; files without one are unchanged.
- Auto-rig adds a group and a handful of layers; undo removes them again in one step.
- Baking writes ordinary cels, so everything downstream (export, engine export, QA) is unchanged.
- A rig cut on one frame applies to that frame's parts. Other frames of the part layers are empty until a pose is baked into them.
