# ADR-ANIM-002: Motion Studio, 3D skeletal motion on 2D sprites

| | |
|---|---|
| Status | Stage 1 (audit) done. Stage 3 core (BVH import, mapping, camera, retarget onto the part rig) built and tested locally on 2026-11-15. A first Motion Studio window is in the app. Stage 3b glTF/GLB animation import built and tested locally (core); the motion library is not built. Everything else is planned below, not built. |
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

1. The BVH, or one glTF/GLB animation, is read into a joint tree. Forward kinematics gives world positions per frame, blended between frames.
2. A bone map links canonical humanoid bones to joints. Names are tried first (CMU/Mixamo, 3ds Max Biped, Daz, Unreal, Blender `.L/.R`), then the hierarchy's shape, and user overrides win.
3. A camera about the character (turn, tilt, roll, orthographic or perspective) projects the joints. Forward comes from the thighs (left minus right, crossed with up), then the toes.
4. The sprite gets the existing gait part rig (`buildGaitRig`, extracted from `gaitRender` with unchanged behaviour). Each frame:
   - Foot targets are the projected ankles, measured from the thighs and scaled by leg length, so the sprite keeps its own proportions. Two-bone IK finds the knee; the bend side comes from the projected source knee.
   - Lift is the source foot's height over its floor, on each leg's own ground row (three-quarter views stand the far foot higher).
   - Contact: a source foot near the floor and slow is planted. Otherwise the lower foot carries the weight, unless the body is airborne. Planted feet sit on the ground, and the hips sink when a planted leg could not reach.
   - The spine lean turns the body bands, and the arms turn to point along the projected source arms. Root travel is removed (in place) or kept.
   - Short "chibi" legs use the no-knee mode, and front views use lift and foreshortening, as the procedural walk does.
5. The camera never turns the art. If the camera is more than 30° from the drawn view (or tilted more than 40°), the result is flagged `approximateView` with a note that the view needs art drawn for it. This is the honest answer to "a 2D image has no 3D geometry".

FBX is not parsed: its SDK is proprietary, and reverse-engineered readers are large attack surfaces. glTF/GLB is read (stage 3b, section 3.1); the CLI and dialog tell the user to export FBX to glTF from Blender.

## 3. What is built (this change)

| Piece | Where | Verified |
|---|---|---|
| BVH parser: line-numbered errors; limits on bytes, joints, depth, frames and values; locale-free numbers; only the six standard channels; names with spaces; End Sites | `core/src/Mocap.cpp` | `tests/test_mocap.cpp`: FK numbers, rotation order, 11 malformed inputs |
| Bone mapping by name (4 schemes tested) and by shape (all names meaningless), with overrides | same | tests |
| Camera with 11 presets, roll, perspective | same | tests: facing, front/left, top-down, roll and perspective directions |
| Retarget onto the gait rig, plus a source-skeleton clip at the same frame count | same | tests on `rogue64`: 12 frames, one size, feet on the ground within the foot stagger, ≥ 10 distinct frames, `checkClip` clean, no depth pops |
| `acestudio-cli retarget` | `cli/MocapCommands.cpp` | Compiled locally with strict warnings only. Not run: `cli/main.cpp` needs the generated `Version.h`, so CI is the first full build. |
| Motion Studio window (Animation > Motion Studio): a 3D view (drag turns and tilts, wheel zooms, arrow keys), a synced 2D preview, turn/tilt/roll sliders, presets, perspective, span, fps, amount, in place, bone table, notes, Open as Document | `app/src/MotionStudioDialog.*` | **Not run.** Computer use is not allowed here, and Qt is not in the local toolchain. CI compiles it, and the UI audit (`anim.motionStudio`) opens it. |

### 3.1 glTF/GLB import (stage 3b, first part)

| Piece | Where | Verified |
|---|---|---|
| `parseGltf` / `loadMotion` / `gltfAnimationNames` / `motionAnimationNames`: GLB container and `.gltf` JSON; the first skin the animation moves (else the first skin; no skin: the subtree under the animated nodes); translation, rotation and scale channels sampled at 30 fps (option) with LINEAR (slerp), STEP and CUBICSPLINE (Hermite with tangents) | `core/src/MocapGltf.cpp`, declared in `Mocap.h` | `tests/test_mocap.cpp`: a skinned chain under an Armature turned -90° about X and scaled 0.01 (LINEAR translation and rotation, STEP, CUBICSPLINE) matches world positions computed independently with glTF matrix math to 1e-9 at 14 times, rest pose included, head above hips (up stays up); non-uniform and animated scale and animated child translation also match to 1e-9 |
| Conversion: offsets are the rest pose in world axes, rotations are each joint's world turn away from rest as Z-X-Y Euler channels, the root has position channels, and a joint gets position channels only when translation or scale animation moves it. The transforms of nodes above the skeleton are baked into the root | same | as above; a mutation check (Hermite tangents removed, one Euler sign flipped) makes those tests fail |
| Untrusted input: 64 MB (file plus external buffers), 512 joints, 16384 nodes, 4096 channels and 2^20 keyframes per channel, 200000 frames, JSON depth 64 checked before parsing; every accessor checked (`byteOffset + (count-1)*stride + elementSize` within the view, the view within the buffer, the buffer within its data), component type, type, `normalized`, byteStride; sparse accessors refused; buffers only from the GLB BIN chunk, base64 `data:` URIs or relative file names inside the file's folder (no `..`, no absolute or drive paths, no URL schemes, symlinks resolved and checked); `extensionsRequired` refused; unknown chunks, extensions and channel paths are skipped, never followed. Errors name the animation, channel, node and accessor | same | tests: truncated GLB, bad version, chunk length overflow, first chunk not JSON, accessor out of bounds, bad componentType/type/normalized/byteStride, sparse, missing bufferView, buffer shorter than declared or over the limit, bad target node and interpolation, wrong output count, times going backwards, node cycles and double parents, matrix plus TRS, `extensionsRequired`, glTF 1.0, bad skin joint, JSON nested 100000 deep, 14 unsafe or broken URIs, byte flips over the whole GLB, and every limit. A `fuzz_gltf` libFuzzer target was added (not run here: clang/libFuzzer is CI only) |
| A Mixamo-named humanoid walk (the BVH test walk written as a GLB under a 0.01 Armature) | tests | same joints and parents as the BVH, positions equal to the BVH's in metres within 2 µm every frame, and the retarget onto `rogue64` passes the same checks as the BVH walk |
| `acestudio-cli retarget` reads `.glb`/`.gltf`, `--animation <index|name>` | `cli/MocapCommands.cpp`, `cli/main.cpp` | Compiled locally with strict warnings only, not run |
| Motion Studio: open filter `*.bvh *.glb *.gltf`; a file with several animations asks which one (`QInputDialog::getItem`) | `app/src/MotionStudioDialog.cpp` | **Not compiled or run locally** (no Qt here). CI compiles it |

Not done in 3b: the motion library (presets, save-as-preset). glTF limits: morph weights are ignored; `KHR_animation_pointer` and other extension-defined targets are skipped; Motion time 0 is the animation's first keyframe; a sampled frame rate other than 30 can be set in `GltfOptions::fps` (not yet in the UI or CLI).

Measured locally (zig clang -O2, 12 frames of a synthetic 30-frame walk): knight32 0.8 ms, rogue64 3–4 ms, and a 119×249 hero 41–50 ms per full retarget. The window re-runs it 120 ms after the last change, on the UI thread.

The walk used in tests is generated in the test (`walkBvh`), so no third-party mocap is committed. A visual review of local renders (the rogue and the user's hero sheet, side and front) showed: the planted foot holds; the near and far feet keep their own ground rows; short legs step a few pixels and do not tear; front views lift the feet in turn. Arms were not found in these sprites, so arm retargeting is covered only by the code path, not by an image.

## 4. Remaining work, staged (none of this is built, except where marked)

| Stage | Scope | Notes |
|---|---|---|
| 2 | Saved, editable rigs (ADR-ANIM-001 schema 4): bone tree, add/remove/rename/reparent, joint limits, IK chains, mirror, lock, undo; GUI bone overlay on the canvas | Needs the chief architect's approval for the schema bump |
| 3b | ~~glTF/GLB animation import~~ (built, section 3.1); a motion library with presets (idle, walk, run, jump, attack, ...) from CC0 sources or generated, plus save-as-preset | License each motion. Do not bundle CMU clips without checking terms. |
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
