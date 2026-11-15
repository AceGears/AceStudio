# ADR-ANIM-002: Motion Studio, 3D skeletal motion on 2D sprites

| | |
|---|---|
| Status | Stage 1 (audit) done. Stage 3 core (BVH import, mapping, camera, retarget onto the part rig) built and tested locally on 2026-11-15. A first Motion Studio window is in the app. Everything else is planned below, not built. |
| Builds on | ADR-ANIM-001 (part rig: parts, hidden pixels, depth, whole-pixel/RotSprite rendering, two-bone IK) |
| Reference | A "Bonebase"-style Aseprite extension: a 3D skeleton beside a 2D rig, with turn, tilt and roll. It was used for the concept only. No code or assets were taken from it. |

## 1. Audit: what AceStudio has (main @ d993b7d)

| Area | State | Where |
|---|---|---|
| Animation engine | Procedural, on the sprite: `motion::animate` routes to species modules on the part rig (gait, flight, swim), with older affine renderers as fallback. Clips are baked frames. | `core/src/SpriteMotion.cpp`, `RigGait/RigFlight/RigSwim.cpp`, `PartRig.cpp` |
| Rig | Parts (own pixels plus inpainted hidden pixels), pivots, per-pose depth, whole-pixel moves, RotSprite turns, column bends, skinning, two-bone IK, depth-pop check. Rigs are rebuilt per clip and never saved (no schema change). | `core/src/PartRig.h` |
| Skeleton data | Only `Pose` (18 OpenPose keypoints, 2D, for ControlNet conditioning). There is no bone tree, no 3D, and no import. | `core/include/acestudio/core/Pose.h` |
| Timeline / layers / project | Frame-and-cel model with tags and per-frame durations. No keyframed tracks or curves. Project schema 3. | `Document.h`, `ProjectIO.cpp`, `app/src/panels/TimelinePanel.cpp` |
| AI pipeline | Python worker over a local protocol (SDXL, ControlNet pose, klein inpainting, BiRefNet). Cloud GPU is designed but opt-in. | `core/AiWorker.*`, `ai_worker/`, `docs/agents/CLOUD_GPU_DESIGN.md` |
| Export | Strips, sheets, GIF/APNG/WebP, Godot `.tres`, Unity `.meta`/`.anim`, all baked. | `Export.cpp`, `EngineExport.cpp` |
| QA | `checkClip` (area, flicker, floating feet, loop, mirror flips, depth pops). Golden fixtures are in `tests/data/anim`. | `SpriteMotion.cpp` |

What can be extended: the part rig is the right base for retargeting. It already keeps bone lengths, layers parts with depth, and renders in a pixel-art-safe way. What is missing: a 3D skeleton type, motion import, a camera model, editable and saved rigs (ADR-ANIM-001 schema 4), keyframed tracks and curves, and a workspace UI.

## 2. Decision for retargeting

Retarget **positions, not rotations**. Copying joint rotations onto a sprite with other proportions breaks foot contact and makes limbs cross wrongly. Instead:

1. The BVH is read into a joint tree. Forward kinematics gives world positions per frame, blended between frames.
2. A bone map links canonical humanoid bones to joints. Names are tried first (CMU/Mixamo, 3ds Max Biped, Daz, Unreal, Blender `.L/.R`), then the hierarchy's shape, and user overrides win.
3. A camera about the character (turn, tilt, roll, orthographic or perspective) projects the joints. Forward comes from the thighs (left minus right, crossed with up), then the toes.
4. The sprite gets the existing gait part rig (`buildGaitRig`, extracted from `gaitRender` with unchanged behaviour). Each frame:
   - Foot targets are the projected ankles, measured from the thighs and scaled by leg length, so the sprite keeps its own proportions. Two-bone IK finds the knee; the bend side comes from the projected source knee.
   - Lift is the source foot's height over its floor, on each leg's own ground row (three-quarter views stand the far foot higher).
   - Contact: a source foot near the floor and slow is planted. Otherwise the lower foot carries the weight, unless the body is airborne. Planted feet sit on the ground, and the hips sink when a planted leg could not reach.
   - The spine lean turns the body bands, and the arms turn to point along the projected source arms. Root travel is removed (in place) or kept.
   - Short "chibi" legs use the no-knee mode, and front views use lift and foreshortening, as the procedural walk does.
5. The camera never turns the art. If the camera is more than 30° from the drawn view (or tilted more than 40°), the result is flagged `approximateView` with a note that the view needs art drawn for it. This is the honest answer to "a 2D image has no 3D geometry".

FBX is not parsed: its SDK is proprietary, and reverse-engineered readers are large attack surfaces. glTF/GLB is planned (stage 3b). Until then, the CLI and dialog tell the user how to convert to BVH in Blender.

## 3. What is built (this change)

| Piece | Where | Verified |
|---|---|---|
| BVH parser: line-numbered errors; limits on bytes, joints, depth, frames and values; locale-free numbers; only the six standard channels; names with spaces; End Sites | `core/src/Mocap.cpp` | `tests/test_mocap.cpp`: FK numbers, rotation order, 11 malformed inputs |
| Bone mapping by name (4 schemes tested) and by shape (all names meaningless), with overrides | same | tests |
| Camera with 11 presets, roll, perspective | same | tests: facing, front/left, top-down, roll and perspective directions |
| Retarget onto the gait rig, plus a source-skeleton clip at the same frame count | same | tests on `rogue64`: 12 frames, one size, feet on the ground within the foot stagger, ≥ 10 distinct frames, `checkClip` clean, no depth pops |
| `acestudio-cli retarget` | `cli/MocapCommands.cpp` | Compiled locally with strict warnings only. Not run: `cli/main.cpp` needs the generated `Version.h`, so CI is the first full build. |
| Motion Studio window (Animation > Motion Studio): a 3D view (drag turns and tilts, wheel zooms, arrow keys), a synced 2D preview, turn/tilt/roll sliders, presets, perspective, span, fps, amount, in place, bone table, notes, Open as Document | `app/src/MotionStudioDialog.*` | **Not run.** Computer use is not allowed here, and Qt is not in the local toolchain. CI compiles it, and the UI audit (`anim.motionStudio`) opens it. |

Measured locally (zig clang -O2, 12 frames of a synthetic 30-frame walk): knight32 0.8 ms, rogue64 3–4 ms, and a 119×249 hero 41–50 ms per full retarget. The window re-runs it 120 ms after the last change, on the UI thread.

The walk used in tests is generated in the test (`walkBvh`), so no third-party mocap is committed. A visual review of local renders (the rogue and the user's hero sheet, side and front) showed: the planted foot holds; the near and far feet keep their own ground rows; short legs step a few pixels and do not tear; front views lift the feet in turn. Arms were not found in these sprites, so arm retargeting is covered only by the code path, not by an image.

## 4. Remaining work, staged (none of this is built)

| Stage | Scope | Notes |
|---|---|---|
| 2 | Saved, editable rigs (ADR-ANIM-001 schema 4): bone tree, add/remove/rename/reparent, joint limits, IK chains, mirror, lock, undo; GUI bone overlay on the canvas | Needs the chief architect's approval for the schema bump |
| 3b | glTF/GLB animation import (skins plus channels; no extensions executed); a motion library with presets (idle, walk, run, jump, attack, ...) from CC0 sources or generated, plus save-as-preset | License each motion. Do not bundle CMU clips without checking terms. |
| 3c | Non-humanoid mapping: quadruped (four legs on `buildGaitRig`'s four-leg mode), wing chains onto `RigFlight` parts, spine/tail chains onto column bends | |
| 4 | Multi-view: a direction set (front, back, side, three-quarter art) chosen by camera turn, with a turn clip between views; reference-view slots | This is where "new view" stops being an approximation |
| 5 | AI assist (optional, worker or cloud only, with an explicit cost prompt): pose estimation of the sprite for auto-rig hints, hidden-surface inpainting of parts, view generation, text-to-motion (an LLM picks and parameterises library motions; no generative model where procedural motion works) | Respect the no-cost-without-approval rule |
| 6 | Timeline tracks: bone keyframes, curves (linear, Bezier, eases, stepped), events, blending between clips (idle→walk→run with phase match) | |
| 7 | Mesh deformation for HD art (triangle mesh, bone weights, weight painting); pixel art keeps the rigid part path | |
| 8 | Workspace: a dockable Motion Studio page (structure tree, canvas with gizmos and onion skin, 3D panel, properties, timeline) replacing the dialog | |
| 9 | Exports of skeletal data (Godot Skeleton2D/AnimationPlayer, Spine-JSON-compatible subset), with checks in engine test projects | |

## 5. Risks
- **Proportions:** chibi sprites have legs about 1/6 of their height, so a realistic walk gives steps of only a few pixels. That is the right scale, but it looks small. `amount` goes up to 2×.
- **Arm detection** depends on `analyzeSubject`. Sprites with cloaks often have no separate arms.
- **The UI thread** blocks on 1000 px sprites. The dialog should use a reduced copy, as Animate Sprite does (not done yet).
