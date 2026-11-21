# ADR-ANIM-002: Motion Studio, 3D skeletal motion on 2D sprites

| | |
|---|---|
| Status | Stage 1 (audit) done. Stage 3 core (BVH import, mapping, camera, retarget onto the part rig) built and tested locally on 2026-11-15. A first Motion Studio window is in the app. Stage 3b glTF/GLB animation import built and tested locally (core); the motion library was built on 2026-11-17 (section 3.2), direction sets and four-legged motion on 2026-11-18 (sections 3.4 and 3.5). Everything else is planned below, not built. |
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
| Motion Studio window (Animation > Motion Studio): a 3D view (drag turns and tilts, wheel zooms, arrow keys), a synced 2D preview, turn/tilt/roll sliders, presets, perspective, span, fps, amount, in place, bone table, notes, Open as Document | `app/src/MotionStudioPanel.*` | **Not run.** Computer use is not allowed here, and Qt is not in the local toolchain. CI compiles it, and the UI audit (`anim.motionStudio`) opens it. |

### 3.1 glTF/GLB import (stage 3b, first part)

| Piece | Where | Verified |
|---|---|---|
| `parseGltf` / `loadMotion` / `gltfAnimationNames` / `motionAnimationNames`: GLB container and `.gltf` JSON; the first skin the animation moves (else the first skin; no skin: the subtree under the animated nodes); translation, rotation and scale channels sampled at 30 fps (option) with LINEAR (slerp), STEP and CUBICSPLINE (Hermite with tangents) | `core/src/MocapGltf.cpp`, declared in `Mocap.h` | `tests/test_mocap.cpp`: a skinned chain under an Armature turned -90° about X and scaled 0.01 (LINEAR translation and rotation, STEP, CUBICSPLINE) matches world positions computed independently with glTF matrix math to 1e-9 at 14 times, rest pose included, head above hips (up stays up); non-uniform and animated scale and animated child translation also match to 1e-9 |
| Conversion: offsets are the rest pose in world axes, rotations are each joint's world turn away from rest as Z-X-Y Euler channels, the root has position channels, and a joint gets position channels only when translation or scale animation moves it. The transforms of nodes above the skeleton are baked into the root | same | as above; a mutation check (Hermite tangents removed, one Euler sign flipped) makes those tests fail |
| Untrusted input: 64 MB (file plus external buffers), 512 joints, 16384 nodes, 4096 channels and 2^20 keyframes per channel, 200000 frames, JSON depth 64 checked before parsing; every accessor checked (`byteOffset + (count-1)*stride + elementSize` within the view, the view within the buffer, the buffer within its data), component type, type, `normalized`, byteStride; sparse accessors refused; buffers only from the GLB BIN chunk, base64 `data:` URIs or relative file names inside the file's folder (no `..`, no absolute or drive paths, no URL schemes, symlinks resolved and checked); `extensionsRequired` refused; unknown chunks, extensions and channel paths are skipped, never followed. Errors name the animation, channel, node and accessor | same | tests: truncated GLB, bad version, chunk length overflow, first chunk not JSON, accessor out of bounds, bad componentType/type/normalized/byteStride, sparse, missing bufferView, buffer shorter than declared or over the limit, bad target node and interpolation, wrong output count, times going backwards, node cycles and double parents, matrix plus TRS, `extensionsRequired`, glTF 1.0, bad skin joint, JSON nested 100000 deep, 14 unsafe or broken URIs, byte flips over the whole GLB, and every limit. A `fuzz_gltf` libFuzzer target was added (not run here: clang/libFuzzer is CI only) |
| A Mixamo-named humanoid walk (the BVH test walk written as a GLB under a 0.01 Armature) | tests | same joints and parents as the BVH, positions equal to the BVH's in metres within 2 µm every frame, and the retarget onto `rogue64` passes the same checks as the BVH walk |
| `acestudio-cli retarget` reads `.glb`/`.gltf`, `--animation <index|name>` | `cli/MocapCommands.cpp`, `cli/main.cpp` | Compiled locally with strict warnings only, not run |
| Motion Studio: open filter `*.bvh *.glb *.gltf`; a file with several animations asks which one (`QInputDialog::getItem`) | `app/src/MotionStudioPanel.cpp` | **Not compiled or run locally** (no Qt here). CI compiles it |

glTF limits: morph weights are ignored; `KHR_animation_pointer` and other extension-defined targets are skipped; Motion time 0 is the animation's first keyframe; a sampled frame rate other than 30 can be set in `GltfOptions::fps` (not yet in the UI or CLI).

Measured locally (zig clang -O2, 12 frames of a synthetic 30-frame walk): knight32 0.8 ms, rogue64 3–4 ms, and a 119×249 hero 41–50 ms per full retarget. The window re-runs it 120 ms after the last change, on the UI thread.

The walk used in tests is generated in the test (`walkBvh`), so no third-party mocap is committed. A visual review of local renders (the rogue and the user's hero sheet, side and front) showed: the planted foot holds; the near and far feet keep their own ground rows; short legs step a few pixels and do not tear; front views lift the feet in turn. Arms were not found in these sprites, so arm retargeting is covered only by the code path, not by an image.

### 3.2 Motion library (stage 3b, second part)

| Piece | Where | Verified |
|---|---|---|
| Built-in presets made by code: idle, walk, run (loops, ending one frame before they repeat) and jump, attack, hurt (one-shot, starting and ending at rest). A Mixamo-named humanoid, Y up, facing +Z, 30 fps; the walk and run travel forward so retargeting finds the facing | `core/include/acestudio/core/MotionLibrary.h`, `core/src/MotionLibrary.cpp` | `tests/test_motion_library.cpp`: every preset maps to a full humanoid, keeps its timing, stays above the floor and retargets onto the rogue fixture; the walk alternates its feet with one planted; the jump leaves the floor. Run locally (zig clang) and in CI. Retargets onto the rogue and knight fixtures were looked at by eye |
| `writeBvh`: depth-first joints, BVH-safe unique names, values in file order; refuses several roots, no frames, values BVH cannot hold | same | round trip of a preset and of a motion with awkward names and a non-depth-first order |
| Saved presets: BVH files in `<user data>/motions`; names are 1–64 ASCII letters, digits, spaces, `-`, `_`, not a built-in or Windows-reserved name, which also keeps every lookup inside the folder | same | save, refuse a duplicate, overwrite, list (an unreadable file is listed with its error), load, refuse `../`, remove |
| CLI: `retarget <sprite> preset:NAME [--library DIR]` (one-shot presets do not loop); `motion list`, `motion export <preset> --out f.bvh`, `motion save <file> --name N`, `motion remove N` | `cli/MocapCommands.cpp`, `cli/main.cpp` | `tests/cli_smoke.cmake` motion section, also replayed locally against a local CLI build (GIF writing is stubbed locally) |
| Motion Studio: Library row (presets, Save as Preset…, Remove) | `app/src/MotionStudioPanel.*` | CI build only; not used on a PC |

The presets are procedural: they show the motion's shape (stride, arm swing, crouch, tuck, strike)
without the nuance of captured motion. A sprite drawn from the front does not rise in a retarget
(the hips only drop for front views), so the jump reads best on side-view sprites.

### 3.3 Motion onto a saved rig (2026-11-17)

| Piece | Where | Verified |
|---|---|---|
| `rigs::keysFromMotion`: a BVH/glTF/preset motion sampled like retarget and keyed onto the document's rig. Matched bones point the way their stretch of the projected skeleton points (held when seen end-on); IK chains with both bones matched reach for the skeleton's foot (or toe), measured from the chain's top joint and scaled to the rig, clamped to the rig's ground; the root rises and sinks with the hips and travels unless in place. Keys on the keyed frames are replaced; frames are added at the end; the rest frame is never keyed. `rigs::drawnCamera` guesses the drawn view from the legs | `core/include/acestudio/core/RigMotion.h`, `core/src/RigMotion.cpp` | `tests/test_rig_motion.cpp`: walk on the auto-rigged rogue and knight (keys, matches, feet never below the ground, bake works), a side view swings the thighs out of step, a jump lifts the root, keys outside the range stay, refusals (no rig, rest frame, unknown bone, nothing matches), a hand-made rig matched by joint names and by `bones` pairs. Run locally and in CI |
| CLI `rig motion` (with `--first`, `--bones`, `--bake` and retarget's timing, view and `--map` options, which now share one parser) | `cli/MocapCommands.cpp`, `cli/RigCommand.cpp` | `tests/cli_smoke.cmake` motion-keys section, replayed locally (rogue and knight): keys, first frame, frames added, refusals, `--bake` keeps the rest frame as rigged and changes a jump frame. Contact sheets of baked walks and jumps were looked at |

Arms keep their drawn pose by default (`armsFromDrawing`): they turn by the change of their
stretch from its average direction over a loop, or from the first frame of a one-shot, so a sprite
drawn with raised weapons keeps them up instead of dropping them to the motion's hanging arms
(checked in `tests/test_rig_motion.cpp`, and by eye on the rogue with arm boxes).

Matching is by name: auto-rig names (`body`, `head`, `legN thigh|shin|foot`, `armN`), joint names
(`LThigh`, ...), or `--bones` pairs. Not done: a button in Motion Studio or the Rig panel, Lua and
MCP access, non-humanoid mapping (stage 3c).

### 3.6 Dockable Motion Studio (2026-11-18)

| Piece | Where | Verified |
|---|---|---|
| `MotionStudioPanel` (was `MotionStudioDialog`): a `QWidget` in the dock `dock.motionStudio`, floating and hidden at start, shown and raised by Animation › Motion Studio (sized 980x760 and centred the first time), in Window and hidden by the Minimal workspace; `setSprite` keeps the motion, camera and timing ("As drawn" re-reads the sprite's facing); Use Current Sprite asks the window for the current frame; Open as Document leaves the panel open; the preview stops while hidden and resumes when shown; no Alt mnemonics | `app/src/MotionStudioPanel.*`, `app/src/MainWindow.*` | CI build and UI audit only (no Qt here, and computer use is not allowed) |

### 3.5 Four-legged motion (stage 3c, first part, 2026-11-18)

| Piece | Where | Verified |
|---|---|---|
| `creatureMap`: legs by shape (leaf joints within 8% of the height above the floor in the rest pose, climbed up while the parent's other children stay low, so a toe and heel count once; never into the root); front and back along the longest horizontal spread of the leg tops in the first frame, the front being where the root travels or else the end nearer the highest other leaf (the head) | `core/src/Mocap.cpp`, `Mocap.h` | `tests/test_creature_motion.cpp`: the preset's four legs (hind first, forward +Z), a humanoid refused (2 legs), on the spot and turned a quarter turn the head still tells the front |
| `retarget` with `body = Quadruped`: the gait rig's four-leg mode; drawn legs take the source leg at the same end and side (near to near by depth); each foot moves from where it is drawn by the source foot's offset from its leg top around the loop's average, scaled per leg; lift, planting, the body's sink so planted feet reach, and the hind hock bending back as the procedural gait does | same | tests: a 72x48 test dog (`tests/data/anim/dog72.png`, drawn for the tests): 12 frames, at least 6 distinct, no floating frames or mirror flips, only the sprite's colours; a humanoid motion is refused |
| `quad-walk` preset: a four-beat walk (left hind, left front, right hind, right front), the lowest toe on the floor every frame | `core/src/MotionLibrary.cpp` | tests: four legs found, every foot lifts, travels forward |
| CLI `--body auto|biped|quadruped` on `retarget` and `directions`; JSON `body` and `legs`; Motion Studio picks four-legged motion for four-legged sprites | `cli/MocapCommands.cpp`, `app/src/MotionStudioPanel.cpp` | `tests/cli_smoke.cmake` (run locally with a placeholder GIF writer); the dialog is compiled by CI only. The test dog's walk sheet was looked at |

Built later on 2026-11-18: back pitch and head and tail follow-through.
`CreatureMap::head` and `tail` are the non-leg ends (above the floor band) furthest forward and
furthest behind the hind legs in the first frame. Per frame, the hind and front leg tops' heights
over the root (the back's pitch) and the head's height over the front tops and the tail's over
the hind tops, each from its neutral (the loop's mean, or a one-shot's first frame), become rows
(`backColumns`: `PartPose::colDy` of the body, straight between the leg pairs, the head and tail
moves fading in beyond them); the leg hips follow their end. Side views only. Verified:
`tests/test_creature_motion.cpp` (head and tail found by shape on `quad-walk`; the test dog's nose
rises and falls at least 2 rows and more than with a stiff back, neck and tail, at most a quarter
of its height; colours kept); the dog's walk sheet was looked at.

Built later on 2026-11-18: `rig motion` for four-legged rigs (`rigs::keysFromMotion`, see
`RigMotion.h`): legs paired by hind/front (from the rig's facing, read from its highest body art)
and near/far, thighs and shins aimed along the creature legs, IK reach scaled by the straight
top-to-foot distance, the root's height from the hips' mean, "body" turned by the back's change.
Verified: `tests/test_rig_motion.cpp` (the test dog faces right, two hind and two front legs,
every thigh swings, every foot lifts and lands, none below the ground, the root within 3 px;
`--bones` refused) and a `tests/cli_smoke.cmake` step, run locally; the baked sheet was looked at.

Then: auto-rig feet are held at their drawn angle (turned against their parents' total turn);
`MotionKeysOptions::facing` (`--facing`) and `legs` (`--legs N=hind-near|hind-far|front-near|front-far`)
override the facing and the pairing. Verified: `tests/test_rig_motion.cpp` (every foot at its rest
angle in every key; facing left swaps hind and front; hand pairs win, near and far differ; a missing
leg and leg pairs with a humanoid motion refused) and `tests/cli_smoke.cmake`, run locally.

Then: head and tail bones. `rigs::autoRig` on a four-legged side view reads the body part's
outline per column; the back and belly rows are those most columns over the legs share. Going
outwards from the legs, the first column whose bottom leaves the belly line starts the end; the
columns before it that rise above the back join it above the back line (the top of a head, a
raised tail). Each end over 2% of the body becomes a layer ("head", "tail") above the body, on a
bone from the middle of its first column to its farthest pixel. `keysFromMotion` turns "head" by
the change of the line from the front legs' top to the motion's head end, and "tail" from the hind
legs' top to the tail tip (relative, like the back); the head bone also gives the facing.
Verified: `tests/test_rig_motion.cpp` (the test dog's nose all in the head layer, the tail's
colour in the tail layer, no pixel lost; both turn by more than 0.05 and less than 1 rad over the
walk; the keys bake), run locally; the baked walk was looked at.

Then: long ends bend. An end whose reach (pivot to farthest pixel) is at least 8 px and whose
reach squared is at least 2.5 times its pixel count gets two bones: the first to the middle of the
art half-way along the reach, the second ("head" on "neck", "tail tip" on "tail") from there to the
tip; the one part layer is skinned across the joint with a band of a quarter of the reach either
side (`rigs::skinLayer`). `keysFromMotion` matches "neck" as it matched "head", and treats the far
bones as followers: "tail tip" turns by 0.8 of the tail's change `round(0.15 * frames)` keys
earlier (wrapping in a loop, held at the start of a one-shot); "head" turns by -0.5 of the neck's
own turn. Verified: `tests/test_rig_motion.cpp` (a drawn animal with a long neck and tail gets the
four bones and two skins and loses no pixel; the head's turn is -0.5 of the neck's in every key;
the tip swings, and not in step with the tail; the keys bake; the test dog's short ends stay one
bone each), run locally; the baked walk was looked at.

Then: sideways swing (2026-11-20). A pose holds a per-bone `stretch` (`RigPose::stretch`, 0.1 to
4, absent = 1): the bone's parts and children are scaled along its rest direction about its head,
then turned (`rigs::solve` gives each bone the 2 by 2 map `BoneWorld::xx..yy`; `bakePose` draws it
as a part pose with width, height and lean, or passes it to the skinned renderer). For each
matched "head", "neck" and "tail" of a four-legged motion, `keysFromMotion` takes how much of the
stretch from the leg tops to the end shows on screen (projected length over true length) against
its reference (a loop's largest value, a one-shot's first frame), held to 0.4..1.25, scaled by the
amount; a change under 2% writes nothing. A note says how much shorter each part gets. Verified:
`tests/test_rig.cpp` (a half-length bone, its children, a turn, version 4 only when used, JSON
round trip and refusals, interpolation, a baked part shorter in the sprite's own colours and an
unstretched bake unchanged), `tests/test_rig_motion.cpp` (a 60 degree wag shortens the tail by
more than 10%, is as drawn at some key, bakes at least 2 px narrower, half the amount gives half
the shortening), a Lua case and CLI smoke lines, run locally; a baked wag was looked at.

Not done: which way a swung part points (toward and away look the same), a head turning to look,
a curve through more than one joint (a skin follows one child bone). Wings: section 3.7. Fins: section 3.8.

Six legs (2026-11-21). `creatureMap` takes four, six or eight floor-reaching legs: sorted along the
body, the two furthest back are the hind pair and the two furthest forward the front pair
(`CreatureMap::legs`, as before, so every four-legged reader keeps working); those between go to
`CreatureMap::middle`, back to front, and `leg(i)` indexes all of them (0-3 `legs`, then `middle`).
`retargetQuadruped` and `keysFromMotion` group the drawn legs when the motion has middle pairs and
more than four legs are drawn: sorted from the back by hip (sprite) or bone head (rig) x, split
into min(source pairs, drawn legs / 2) equal groups, the first taking the hind pair, the last the
front pair, the others the middle pairs in order; within a group near and far alternate as before.
Otherwise the old hind/front split by the middle stands, with a note when middle pairs go unused
or a four-legged motion drives more than four legs. Built-in preset `hex-walk`
(`MotionLibrary.cpp`): hips, thorax, neck, head, abdomen, three pairs of three-bone legs along the
thorax; 0.6 s; the alternating tripod (left hind, right middle, left front against the others).

Verified: `tests/test_creature_motion.cpp` (the preset's legs found as hind, front and middle
pairs with head and abdomen ends, at least three feet down in every frame, five legs refused; 7
frames on a drawn ant in its own colours with nothing floating; the middle pair left out on the
test dog; quad-walk on the ant), `tests/test_rig_motion.cpp` (an auto-rigged ant's six thighs
matched hind, hind, middle, middle, front, front from the back, each swinging, keys bake; the
notes for four-on-six and six-on-four), Lua and CLI smoke lines, run locally; the ant's strip and
baked rig frames were looked at.

Not done: eight legs tried, captured insect motion, a middle leg's own knee direction, legs
splayed sideways, `--body auto` telling six legs from four.

### 3.7 Wings from a humanoid motion (stage 3c, second part, 2026-11-18)

`RetargetOptions::body = Flyer` (`retargetFlyer` in `Mocap.cpp`) poses the flight rig that
`flightRender` uses (`buildFlightRig`, the same wing cut, near and far wing, shoulder skin) from a
humanoid motion instead of a procedural beat. Per frame, each source arm's elevation (the upper
arm's angle above the horizontal, in world space, so the camera does not change it) and its elbow
(the forearm's elevation over the upper arm's) are read. The drawn wing is the top of the stroke:
the frame where that arm is highest. A wing's turn is `amount` times the arm's drop from there
(clamped to -0.5..2 rad; the far wing 0.8 of it, foreshortened up to 15%), its hand bend 0.9 of
the elbow's change (clamped to 1.2 rad). The near wing follows the source arm nearer the camera at
the first frame. The body (and the head end drawn over the near wing) moves by the hips' height
from neutral (the loop's mean, a one-shot's first frame), at most an eighth of the sprite's height.
The camera defaults to the side the head faces. A four-legged motion (creatureMap finds four legs)
is refused, as is a skeleton without hips and both arms. `checkClip` skips its top-region check for
Flyer clips, as for Animate's flap and fly. The built-in `flap` preset (0.5 s loop) raises the arms
out to the side to 150 degrees and beats them down to 25, the forearms trailing on the way down and
folding on the way up, the hips lifted by the downstroke.

| What | Where | Verified |
|---|---|---|
| `retargetFlyer`, `RetargetOptions::wingLine` / `wingMask` | `core/src/Mocap.cpp`, `Mocap.h` | `tests/test_wing_motion.cpp` (with a wing selection on a test dragon: 6 frames, the first as drawn, the wing's lowest row at least 8 rows below the drawn one and less with amount 0.5, wing pixels kept, the head untouched, only the sprite's colours; idle notes that the arms hardly move and the wing stays within 3 rows; quad-walk and a sprite without wings refused), run locally; the 128 px golden dragon's flap strip looked at |
| `flap` preset | `core/src/MotionLibrary.cpp` | the test above (arm raised above 0.8 rad and beaten below -0.6 rad); preset count tests |
| CLI `--body flyer`, `--wing-line`; Lua `body = "flyer"`, `wing_line`; MCP `body`/`wing_line`; Motion Studio Body box | `cli/MocapCommands.cpp`, `core/src/Script.cpp`, `mcp/AceTools.cpp`, `app/src/MotionStudioPanel.cpp` | `tests/cli_smoke.cmake` (the golden dragon: 6 frames, no issues; quad-walk and a bad wing line refused), run locally with a placeholder GIF writer; Lua, MCP and the panel are compiled (the panel by CI only), not driven by a test |

Then: the tail waves with the near wing's stroke (`flightTailColumns` and `tailColumnDy`, shared
with `flightRender`; a loop either side of mid-stroke, a one-shot from its start, 0.3 of the span
later at the tip, at most 0.15 of the tail's length either way); Motion Studio has a wing line and
takes a selection made before Use Current Sprite as the wing mask. Verified: the test dragon's tail
tip moves at least a row and at most its reach (`tests/test_wing_motion.cpp`), with the part rig,
sprite motion and key pose tests still passing; the panel is compiled by CI only.

Then: a skeleton's own wing bones (2026-11-21). `retargetFlyer` first looks for wing chains: a
chain starts at a joint with "wing" in its name whose parent has not, and runs to its farthest
descendant; its middle joint is the one on that path nearest half the reach. With two or more
chains the two longest drive the wings in place of the arms (no humanoid map is needed and four
legs are not refused); the root above the first chain stands in for the hips; forward is from that
root to a joint named head, else square to the line between the wing roots (a guess, noted when
sweep shows); the source picture is scaled by the skeleton's longest rest distance. Elevation and
elbow are read as for arms (shoulder to middle joint, middle joint to tip). Two more numbers per
frame: fold, 1 minus the shoulder-to-tip distance over its longest in the span, and sweep, the
arcsine of the first bone's part along forward. Against the top frame of the stroke, fold times the
amount shortens the drawn wing along its length (`PartPose::sy` for a wing whose farthest pixel is
more above or below its shoulder than beside it, else `sx`; 0.55 to 1.1), and sweep leans an
upright wing (`PartPose::shear` = the tangent of the sweep's change, at most 0.5 rad, toward the
head for forward). `renderPose` now scales and leans a skinned root by the same weight it turns by
(the near wing's root is skinned; scaled at full strength it left a streak). Built-in preset
`wingbeat` (`MotionLibrary.cpp`, `PresetInfo::winged`): hips, chest, neck, head, tail, two wings of
three bones; 0.6 s; the shoulder from 55 degrees up to 35 down, on the way up swept back 14 degrees
with elbow and wrist each turned back up to 50.

Verified: `tests/test_wing_motion.cpp` (the preset rises above 0.8 rad, falls below -0.5, its tip
comes within 85% of its longest reach and its elbow goes behind its shoulder; on the test dragon 7
frames, the first as drawn, the wing at least 5 rows lower at the bottom, at most 90% of its
pixels at its most folded and more than that with half the amount, the head untouched, only the
sprite's colours; a skeleton with no joint named head gets the guess note; with one wing chain
renamed the skeleton is read by its arms' shape, with no fold), the flight, part rig and sprite
motion tests still passing, a Lua line and CLI smoke lines on the golden dragon, run locally; the
dragon's strip was looked at. The test does not separate sweep from the turn (both move the wing
toward the tail), and the golden dragon's wings are drawn out to the side, so sweep has not been
looked at on real art.

Not done: wing bones not named wing, a crease at the wrist, sweep on a wing drawn out to the side,
twist.

### 3.8 Fins from a motion's spine (2026-11-20)

`mocap::retarget` with `Body::Swimmer` (`retargetSwimmer`, `core/src/Mocap.cpp`) poses the swim
rig (`buildSwimRig`, `RigSwim.cpp`: body, tail fin past the stalk, other fins). The spine is the
joint path from the first joint whose name contains "head" to the last whose name contains
"tail"; without both, the skeleton's longest rest-pose path (from the head when one is named;
else the end that moves less across the path is the head). Per frame each chain joint's offset
from the head joint, across the rest axis, is taken in the one of two perpendicular directions
that swings more, divided by the chain's length, and measured from its neutral (a loop's mean, a
one-shot's first frame). The body's columns take that wave (0 to 0.8 of the chain runs head to
stalk when there is a tail fin, else the whole chain), in whole pixels, neighbours at most one
apart, at most 35% of the height. The tail fin rides the stalk and turns by the chain's slope
after 0.8 against its slope before (at most 0.6 rad), narrowing through the middle of its sweep
as Animate's swim does; the other fins ride their columns and paddle (below). Built-in preset `swim`
(`MotionLibrary.cpp`): Head (root), five spine joints yawing 3 to 22 degrees with a phase lag of
0.1 to 0.7 cycles, 0.8 s; two pectoral fins off the first spine joint pitching 14 degrees in
opposite phase and a dorsal fin off the second pitching 6 degrees. `PresetInfo::swimmer` marks it. The clip check skips the top-region
test for these clips (the back moves down with the wave).

Verified: `tests/test_swim_motion.cpp` (the preset's tail end sweeps while the head stays; on a
drawn fish the tail fin's mean row moves 3 px or more, the eye 1 px or less, pixel count within
10%, only the sprite's colours, more amount gives more sweep; a one-shot starts as drawn; a
6 by 3 sprite is refused), a Lua case and the CLI smoke test on `fish57.png`, run locally; the
goldfish strip was looked at.

Paddling fins (2026-11-20). A fin joint is a child of a spine joint, not itself on the spine,
whose name contains fin, flipper, pectoral, pelvic or dorsal. Its direction runs to its farthest
descendant (from its parent when it has none). Per frame two numbers are taken from the unit
direction: its part along the spine where it joins (the line between the spine joints either
side), and its part upward; each is measured from its neutral (a loop's mean, a one-shot's first
frame), and the one that swings more gives the turn (its arcsine; for upward movement a lower fin
lifting counts as toward the tail, a top fin lifting as toward the head). A joint that swings
less than 0.02 is ignored. A fin joint is a top fin when it points up at least as much as
sideways. Each drawn fin (`SwimFin`, top or lower by its root against the body's middle) takes
the joint of its kind nearest along the body and turns about its root by the turn times the
amount, at most 0.35 rad (further and the root, drawn behind the body, shows past its edge).
Verified: `tests/test_swim_motion.cpp` (a fish with a fin below and one above: the lower fin's
mean column moves 1 to 6 px and is further toward the tail when the motion's fin is; the top fin
moves less; pixel counts within a quarter; a one-shot starts as drawn; a walk leaves the fins
riding and says so), run locally; the goldfish strip was looked at beside one with paddling off.

Fins bend with the body (2026-11-21). A fin rode its root column in one piece, so a long fin under
a lifting tail stayed behind and showed as a line past the belly. Both swims (`RigSwim.cpp`,
`retargetSwimmer`) now give each fin column the body's offset at that column less the offset at
the fin's root (`PartPose::colDy`). Verified: `tests/test_swim_motion.cpp` ("A fin along the belly
bends with the body's columns"), the swim, sprite motion, part rig, pixel rig and key pose tests,
run locally; the goldfish strip was looked at again and the line is gone.

Not done: left and right fins told apart, fins foreshortened as they flap, a wave in two planes,
fins on `rig motion`.

### 3.4 Direction sets (stage 4, first part, 2026-11-18)

| Piece | Where | Verified |
|---|---|---|
| `directions::chooseView`: the drawn view nearest a camera turn, or the mirror of a view on the other side (a tie goes to drawn art); `retargetDirections`: the motion retargeted per direction onto the chosen art, the camera turned to that direction, `RetargetOptions::drawnTurn` set to the art's turn so the 30 degree "approximate view" check measures from the art actually used; `turnClip`: a turn on the spot that shows the nearest view at each step, repeats dropped; `clipsDocument`: the clips on one project, one tag each, every clip's figure centred and standing on one row | `core/include/acestudio/core/DirectionSet.h`, `core/src/DirectionSet.cpp`, `Mocap.cpp` | `tests/test_direction_set.cpp`: names and turns, choice and mirror ties, refusals, a walk in 8 directions from a front and a side view (choices, flags, notes, west matches the mirrored east by silhouette), a turn clip on one ground row, the document's tags and cels. Run locally |
| CLI `directions <views folder> <motion> --out <project>` (`--dirs`, `--no-mirror`, `--turn-clip`, `--turn-step`, `--turn-ms`, `--name`, retarget's timing, tilt and `--map` options) | `cli/MocapCommands.cpp` | `tests/cli_smoke.cmake` direction-set section, run locally; the 8-direction walk sheet was looked at |

The art is never turned: this stage stops "new view" from being an approximation only where a
view is drawn. Not done: a turn clip that draws in-between views, reference-view slots in the
project and the app, three-quarter-aware leg handling in retarget (three-quarter art is read as
front or side), rig motion per direction, and Lua and MCP access.

## 4. Remaining work, staged (none of this is built, except where marked)

| Stage | Scope | Notes |
|---|---|---|
| 2 | Saved, editable rigs (ADR-ANIM-001 schema 4): bone tree, add/remove/rename/reparent, joint limits, IK chains, mirror, lock, undo; GUI bone overlay on the canvas | Needs the chief architect's approval for the schema bump |
| 3b | ~~glTF/GLB animation import~~ (built, section 3.1); ~~a motion library with presets and save-as-preset~~ (built, section 3.2) | More presets (crouch, climb, cast, die) and CC0 captured clips, each licensed |
| 3c | (four-legged first part built, section 3.5; wings from humanoid arms built, section 3.7) Non-humanoid mapping: quadruped (four legs on `buildGaitRig`'s four-leg mode), wing chains onto `RigFlight` parts, spine/tail chains onto column bends | |
| 4 | (first part built, section 3.4) Multi-view: a direction set (front, back, side, three-quarter art) chosen by camera turn, with a turn clip between views; reference-view slots | This is where "new view" stops being an approximation |
| 5 | AI assist (optional, worker or cloud only, with an explicit cost prompt): pose estimation of the sprite for auto-rig hints, hidden-surface inpainting of parts, view generation, text-to-motion (an LLM picks and parameterises library motions; no generative model where procedural motion works) | Respect the no-cost-without-approval rule |
| 6 | Timeline tracks: bone keyframes, curves (linear, Bezier, eases, stepped), events, blending between clips (idle→walk→run with phase match) | |
| 7 | Mesh deformation for HD art (triangle mesh, bone weights, weight painting); pixel art keeps the rigid part path | |
| 8 | Workspace: a dockable Motion Studio page (structure tree, canvas with gizmos and onion skin, 3D panel, properties, timeline) replacing the dialog | |
| 9 | Exports of skeletal data (Godot Skeleton2D/AnimationPlayer, Spine-JSON-compatible subset), with checks in engine test projects | |

## 5. Risks
- **Proportions:** chibi sprites have legs about 1/6 of their height, so a realistic walk gives steps of only a few pixels. That is the right scale, but it looks small. `amount` goes up to 2×.
- **Arm detection** depends on `analyzeSubject`. Sprites with cloaks often have no separate arms.
- **The UI thread** blocks on 1000 px sprites. The dialog should use a reduced copy, as Animate Sprite does (not done yet).
