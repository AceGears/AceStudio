# ADR-ANIM-001: Part rig for creature animation

| | |
|---|---|
| Status | Partly implemented (2026-11-08, on main): steps 2–5 without the schema change. See section 8. Schema 4 (step 1) is still proposed and needs approval from `acestudio-chief-architect`. |
| Deciders | `animation-director` (12), `rigging-engineer` (13), `acestudio-chief-architect`, `asset-pipeline-engineer` (format) |
| Evidence | `docs/agents/AUDIT_2026-11-08-animation.md`, `docs/agents/audit-evidence/2026-11-08/` |
| Supersedes | Nothing. Builds on `a8f1b3b` (whole-pixel rig), `0f15b26` (AI key poses) and `fa23f09` (sideways swim). |

## 1. Context: what the code does today (main @ 39d48e7)

All non-AI creature motion is in `core/src/SpriteMotion.cpp` and `core/src/PixelRig.cpp`. Effects are in `core/src/Effects.cpp`. AI key poses are in `core/src/KeyPoses.cpp` and are driven from `app/src/SpriteAnimateDialog.cpp`.

| Piece | How it works | Where |
|---|---|---|
| Subject analysis | Silhouette heuristics: ground rows, foot column runs, leg top (rows that stay split), arms (thin runs beside the legs that differ in colour), front view (one foot run under a wider torso), facing (toe overhang), and head side for swimmers (tail waist). | `SpriteMotion.cpp:648-866` |
| "Parts" | A per-pixel label map built from rectangles and rows: ground, legs (pixels below `legTop` nearest a foot run), arms (rectangles), wing (everything above a wing line inside a span, or a painted mask), and the body (everything else). Parts are not separated layers. Nothing behind a part exists, so a moved part leaves a hole or exposes nothing. | `PixelRig.cpp:106-131`, `SpriteMotion.cpp:378-430` |
| Walk/run/idle (`rigRender`) | Whole-pixel moves. Each leg is a row stack. Knee bend comes from shifting rows sideways. Length changes come from dropping or doubling thigh and shin rows (`resizeRows`). The planted foot moves back at a constant speed. The swing foot moves on an eased arc. Body bob is `sin(4πt)`, and the head lags one frame. Arms are sheared from the shoulder. The z-order is fixed: far leg, near leg, body, arms. | `PixelRig.cpp:20-39, 57-279` |
| Flap (`flapRender`) | Wing pixels are folded about one horizontal root row. Every wing row `y` is redrawn at `R - d·fold(t - lag·u)`, where the fold keys are `{1, .55, -.35, -.75, -.65, -.3, .35, .75, 1}` (a fast down and a slow up), and gaps are filled by repeating the row. The body rises by `lift` px on the downstroke. Both wings are a single mask, so they share phase and foreshortening. The wings are drawn after the body, so they are always in front. | `PixelRig.cpp:281-350` |
| Swim (`swimRender`) | Each column gets a lateral offset `z(k) = lateral · envelope(u) · sin(2π(beats·t - lag) - 2πu/λ)`. Four envelopes are used (anguilli-, subcarangi-, carangi-, thunniform). In side view the bend is shown by dropping columns (arc length `1/√(1+dz²)`) plus a 0.3× vertical wave. Fins are not parts. | `PixelRig.cpp:352-439` |
| Every other action, and every body without legs | Legacy affine "FramePose" (translate, scale, rotate, shear, leg swing) sampled nearest-neighbour. | `SpriteMotion.cpp:170-315, 374-547` |
| Directions / turn | GUI only (Character Pack). "Rotate" mode mirrors the clip and rotates each frame with `rotateSprite` (Scale2x ×3 then nearest rotation). "AI views" mode asks klein for front, back and three-quarter stills, then animates each one independently. No turn clip exists. | `app/src/pages/GeneratorPage.cpp:888-1031`, `SpriteMotion.cpp:893-915` |
| AI key poses (flap only) | Four poses: `up` (the original), plus `level`, `down` and `rising`, each drawn by FLUX.2 klein inpainting the wing mask at 512 px (2 variants each, seed 4242). The result is reduced to the sprite palette, aligned on the bottom 38% (legs), and body pixels are restored where the colour is near (L1 < 110). The clip is assembled as 4 frames with no in-betweens. | `KeyPoses.cpp:9-223`, `SpriteAnimateDialog.cpp:340-430` |
| Fire | A new flame every frame. A teardrop fuel shape is eroded by three octaves of tileable value noise rising one period per loop, with a sideways warp. It is quantised to a 6-step ramp of the sprite's own flame colours, and rim pixels are clamped to level ≤ 2. Sparks follow seeded paths, and embers glow from a noise threshold. The canvas is the source size, so the flame is clamped at row 0. | `Effects.cpp:112-281` (clamp at `:231`) |
| App QA of procedural clips | `checkClip`: area change ≤ 35% (≤ 70% for `flap`), flicker blips, floating feet, loop IoU. It has no part, z-order, mirror, ping-pong or anatomy checks. `AnimationQA.cpp::evaluateFrames` (silhouette IoU, ground, height, palette, facing) is only used for AI-generated frames. | `SpriteMotion.cpp:966-1051`, `AnimationQA.cpp:337-422` |

### Why it cannot meet `ANIMATION_STANDARDS.md`
The 11-08 re-audit measured the installed nightly build. Every limit traces back to one missing concept: **a sprite is a single raster with a label map, not a set of layered parts with hidden pixels, bones and depth.**
- A part cannot move without exposing nothing behind it. This is why a dragon wing can only fold over the body (F4/F5: the head is hidden), and why a walking torso cannot rotate.
- Without per-part z, the wings are always in front, the far wing cannot pass behind the torso, and legs cannot cross correctly in a front view.
- Without bones there are no joint limits and no constant bone lengths (legs change length by dropping rows), and the wing has no shoulder→elbow→wrist→digit chain.
- The fish's lateral bend can only show as column dropping. Body length shrinks by 4–10% (measured 51–56 px against 57 px), fins are frozen, and a turn is a mirror.

Tuning these renderers cannot fix the dragon, fish or front-view walk.

## 2. Decision

Introduce a **2.5D part rig** in core. It is shared by every species module and by the AI key-pose path.

1. **Parts**: separate RGBA layers cut from the sprite. Each part carries the pixels hidden behind other parts, filled by inpainting.
2. **Bones**: a tree of 2D bones with rest length, angle limits and a parent. Parts are bound rigidly to one bone, or skinned along a short chain (tail, spine, membrane).
3. **Depth**: each part has a base z, and the pose can override z per frame. Near and far sides are explicit (`side: near|far|centre`).
4. **Rendering stays pixel-art-correct.** Rigid parts move by whole pixels and rotate with the existing `rotateSprite` path (Scale2x ×3 plus nearest). Skinned chains use whole-pixel row/column deformers (generalised `resizeRows`) or an affine-per-triangle membrane mesh sampled nearest-neighbour. Only source palette colours are written.
5. **Species motion modules** produce `Pose2D` sequences (bone angles, z overrides, root offset). They never write pixels.
6. **Effects** get their own procedural field (`FlameField`). It does not use the rig.

Alternatives considered:
- (a) Keep the label-map renderers and add more heuristics. Rejected: the 11-07 and 11-08 evidence shows that occlusion and depth defects are structural.
- (b) Full 3D (voxelise, then render). Rejected for now: high cost, and it breaks pixel-art cluster stability. It remains an escalation path.
- (c) AI-only frames (klein per frame). Rejected as the primary path: there is no temporal consistency (independent seeds and variants), it is GPU-heavy, and the body has to be pasted back. It is kept as an optional **key-pose source** that feeds the rig: AI draws a pose, and the rig is fitted to it.

## 3. What must change (by requirement)

### 3.1 Part segmentation + occlusion inpainting
- New `core/include/acestudio/core/Rig.h`, `core/src/RigSegment.cpp`:
  - `struct Part { int id; std::string name; Mask mask; Image pixels; Image hidden; IntRect box; int bone; Side side; int zBase; }`
  - `Result<Rig> segmentParts(const Image& sprite, const motion::Subject& subj, const SegmentHints& hints)`. It promotes today's heuristics (`analyzeSubject` feet, legs and arms, the `WingSpec` mask, the swimmer head side) to **masks**. It adds outline-aware region growing: split along the dark outline and colour boundaries inside the label region, and keep the 1-px outline with the part that owns it.
  - Hints come from the user (painted part masks in `SpriteAnimateDialog`, which already paints a wing mask) or from an optional AI segmenter (a worker task `image.segment`, SAM-family, chosen by `ai-research-engineer` per `AI_MODEL_STRATEGY.md`). Core stays dependency-free.
- New `core/src/RigInpaint.cpp`: `void inpaintHidden(Rig&, const Image& sprite, InpaintMode)`.
  - `Procedural` (default, CPU): for each part, fill the pixels that other parts cover with row/column continuation inside the part's convex outline, then re-quantise to the part's own palette and redraw the outline.
  - `Ai` (optional, GPU): reuse `keypose::inpaintInputs` and `keypose::reduceToSprite`, generalised from the wing mask to any part mask.
  - Output: `Part::hidden`, which is shown only when a part moves away.

### 3.2 Per-part z-order
- `struct PartPose { int part; int dx, dy; double angle; int z; bool visible; }` in `Pose2D`.
- New `core/src/RigRender.cpp`: `Image renderPose(const Rig&, const Pose2D&, IntRect box)`. Sort by `(z, zBase, id)`, then composite with binary alpha. This replaces the fixed draw orders at `SpriteMotion.cpp:515-525` and `PixelRig.cpp:190-196`.
- Rule from the standards: z swaps only when the geometry crosses. `RigRender` exposes `crossings(const Rig&, const Pose2D&)` so QA can assert this.

### 3.3 Wing chain + membrane mesh (dragon, bat, bird)
- Bones `shoulder → humerus → forearm (elbow) → wrist → digits[2..4]`. Angle limits come from a creature template (`core/data/rig/wing_template.json`).
- Membrane: a triangle fan between adjacent digit bones and the body edge. Each triangle is texture-mapped (affine, nearest) from the rest pose's inpainted membrane. Stretch is allowed up to 115% of the rest area and slack down to 70%. Below that, rows fold with a darker shade from the palette ramp, not a mirrored copy.
- Near and far wing are **separate parts**. The far wing gets x-foreshortening (0.6–0.8 of rest span), a phase lag of 0.03–0.06 cycle, and `z < torso`. The near wing gets `z > torso` except when its downstroke crosses behind the shoulder.
- Torso heave against the beat. Head stabilisation: the counter-offset keeps the head within ±1 px of its rest height. The tail is a 3–6-bone chain with a lag of 0.08–0.15 cycle per bone.
- Module: `core/src/anim/Flight.cpp` (owner `dragon-animation-specialist`). It replaces `flapRender` (`PixelRig.cpp:281`) and the affine `Fly` plan (`SpriteMotion.cpp:278-289`).

### 3.4 Fish spine chain with real turn views
- Spine: 8–12 bones head → peduncle → caudal. The lateral bend is a yaw per bone. In side view each segment is drawn with its own foreshortening (`cos(yaw)` width) and a depth shade, so **arc length is conserved** (constant body length). The current column dropping shortens the fish.
- Fins are parts on their own oscillators: pectorals (brake/hover, opposite phase to the tail when decelerating), pelvics and anal (small, lagging), dorsal (flex with the spine), caudal (thrust; it lags the peduncle by 0.1–0.2 cycle).
- Turn: a `turn` clip curls the spine into a C-shape, then passes through views `side → 3/4 → head-on (or top) → 3/4 → mirrored side`. Wave phase stays continuous across the turn. The views come from (a) user-drawn views, or (b) reference-consistent AI views (`reference-consistency-engineer`) fitted with the same rig. This replaces mirror plus `rotateSprite` in `GeneratorPage::finishPack` for swimmers.
- Speed: `swim(speed)` maps to beat frequency and amplitude (Videler-style tail-beat scaling). Dash is an acceleration ramp, not a translation lunge.
- Module: `core/src/anim/Swim.cpp` (owner `aquatic-animation-specialist`). It replaces `swimRender` (`PixelRig.cpp:352`).

### 3.5 Foot-lock gait (humanoid, quadruped)
- Two-bone IK legs: hip → knee → ankle, plus a rigid foot part. The planted foot is locked to a ground track that scrolls at `stride / (frames/2)` px per frame (an in-place cycle), with a sole row error of 0 px. Thigh and shin lengths are constant; the bend shows as the knee position, not as dropped rows.
- Pelvis and shoulders counter-rotate by 1 px of column shear on the torso parts. The centre of mass follows contact (high) → down (lowest) → passing → up (highest). Today's timing in `PixelRig.cpp:143-151` is correct and is kept.
- Front and back views step in place: the leg lifts, the knee comes forward and up, and there is no scissor. Front detection must not depend on mirror symmetry of the whole sprite. A shield breaks the 80% test at `PixelRig.cpp:72-82`. Use leg symmetry plus the hint from the Character Pack view key (`s`, `n`).
- Equipment (shield, sword, staff) is a **rigid part on the hand bone**: it is never sheared. Today's arm shear at `PixelRig.cpp:261-275` breaks the knight's shield.
- Quadruped footfall tables per gait (walk lateral sequence, trot diagonal, canter, gallop).
- Module: `core/src/anim/Gait.cpp` (owner `character-motion-specialist`). It replaces the leg loop in `rigRender`.

### 3.6 Procedural flame field (fire, torch, furnace, magic)
- New `core/src/fx/FlameField.cpp`. Several fuel sources sit along the log line, and each emits a tongue with its own phase. A scalar heat field is advected upward with buoyancy and curl noise. Tongues split where the heat saddles and merge where they overlap, and break off as islands near the tip.
- The canvas auto-extends upward (`EffectClip::box`) so the tip is never clamped. Today it is clamped at `Effects.cpp:231`, and the tip reaches row 0 in campfire F2 and F7–F9 and in 10 of 16 raging frames.
- Palette: a monotonic luminance ramp from the sprite's flame colours. Rim pixels take the darkest two steps only, which removes the isolated pale-yellow rim pixels (about 7 per frame today).
- Embers are particles with 4–10 frame lifetimes that follow the field velocity and cool through the ramp.
- Time variation uses ≥ 2 incommensurate loop-periodic frequencies, so `diff_irregularity` rises above 0.15. Today it is 0.08–0.10.
- Owner: `environmental-effects-specialist`. This does not depend on the rig.

## 4. Schema impact (`docs/project-format.md`)

`docs/project-format.md` still documents schema 1, but the code writes schemas 2 and 3 (`ProjectIO.cpp:225-238`, `kProjectSchemaVersion = 3`). `documentation-engineer` must first document 2 (tile mode, masks, clipping, adjustment layers) and 3 (Levels/Curves, linked cels).

Proposed **schema 4**. It is required only when the document contains a rig, so files without a rig stay readable by 0.10.x.

New ZIP entries:

| Entry | Compression | Contents |
|---|---|---|
| `rig/rig.json` | deflate | Rig model (below) |
| `rig/parts/P<id>.png` | stored | Part pixels at rest, RGBA, part-box sized |
| `rig/parts/P<id>_hidden.png` | stored | Inpainted pixels behind other parts (binary alpha) |

```json
{
  "rigVersion": 1,
  "source": { "layer": 3, "frame": 0 },
  "views": [ { "key": "e", "frame": 0 }, { "key": "se", "frame": 12 } ],
  "bones": [ { "id": 1, "parent": 0, "name": "spine0", "x": 31.5, "y": 20.0, "length": 6.0,
               "angle": 0.0, "min": -0.6, "max": 0.6 } ],
  "parts": [ { "id": 1, "name": "torso", "bone": 1, "side": "centre", "z": 0,
               "box": [12, 8, 30, 18], "file": "rig/parts/P1.png", "hidden": "rig/parts/P1_hidden.png",
               "skin": "rigid|rows|columns|mesh", "mesh": null } ],
  "template": "dragon|fish|biped|quadruped|custom"
}
```

Poses are stored as baked cels, as today, so every exporter keeps working. Optionally they are also stored as `tags[].extensions.rigPoses` (a list of `Pose2D` per frame), which lets the clip be re-rendered after a part edit.

Reader rules:
- Unknown part `skin` values are refused.
- A part file must equal `rig/parts/P<id>.png`.
- The limits from the "Limits enforced on read" section apply to part images.

`requiredSchemaVersion` gets `if (doc.rig()) v = std::max(v, 4);`.

## 5. Implementation sequence (serial dependency order)

| # | Step | Owner(s) | Files / functions | Depends on |
|---|---|---|---|---|
| 0 | QA gates first: anim_qa ping-pong and top-region guard (done 11-08). Port the same checks and a mirror check to `motion::checkClip`. Remove the `flap` 70% exemption (`SpriteMotion.cpp:980`) in favour of per-part checks. Add golden fixtures `tests/data/anim/{dragon128,fish57,campfire64,rogue64,knight32}.png` with expected-verdict JSON. | `temporal-consistency-engineer`, `qa-engineer` | `.claude/skills/animation-quality/scripts/anim_qa.py`, `core/src/SpriteMotion.cpp::checkClip`, `tests/test_sprite_motion.cpp` | – |
| 1 | Rig data model and schema 4 (read/write, round-trip, compat fixtures), with project-format docs for schemas 2–4. | `rigging-engineer`, `asset-pipeline-engineer`, `backend-architect`, `documentation-engineer` | `core/include/acestudio/core/Rig.h`, `core/src/ProjectIO.cpp::documentToJson/requiredSchemaVersion`, `docs/project-format.md`, `tests/test_project_io.cpp`, `tests/data/compat/era4.acestudio` | 0 |
| 2 | Part segmentation (procedural, plus a hint mask from the GUI). The AI segmenter is optional and comes later. | `rigging-engineer` (+ `ai-research-engineer` for the optional model) | `core/src/RigSegment.cpp::segmentParts`, generalising `SpriteMotion.cpp::analyzeSubject` and `keypose::wingMask` | 1 |
| 3 | Occlusion inpainting (procedural first; the AI mode reuses `keypose::inpaintInputs/reduceToSprite`). | `generative-art-engineer`, `pixel-art-director` (review) | `core/src/RigInpaint.cpp::inpaintHidden` | 2 |
| 4 | Part renderer with per-part z, whole-pixel rigid moves, rotation via `rotateSprite`, row/column skin (generalised `resizeRows`), and the membrane mesh rasteriser. | `graphics-engineer`, `rigging-engineer` | `core/src/RigRender.cpp::renderPose`, `crossings` | 3 |
| 5a | Foot-lock gait (biped, quadruped, front/back stepping, rigid equipment). | `character-motion-specialist` | `core/src/anim/Gait.cpp`; route from `animatePack` (`SpriteMotion.cpp:938`) | 4 |
| 5b | Wing chain, membrane mesh, near/far wings, head stabilisation, tail chain. | `dragon-animation-specialist` | `core/src/anim/Flight.cpp`; retire `flapRender`; refit `keypose::assemble` to fit the rig to AI poses and in-between | 4 (and 5a for the legs) |
| 5c | Fish spine chain, fin oscillators, speed model, turn clip with views. | `aquatic-animation-specialist`, `reference-consistency-engineer` (views) | `core/src/anim/Swim.cpp`; retire `swimRender`; replace the swimmer branch of `GeneratorPage::finishPack` | 4 |
| 6 | Temporal consistency pass: cluster stability, sub-pixel policy, loop closure, z-crossing assertion. | `temporal-consistency-engineer` | `core/src/anim/Temporal.cpp`, `checkClip` | 5a–5c |
| 7 | Surfaces: CLI `animate --rig auto|<file>`, MCP `sprite_animate` rig params, Lua `ace.animate` (Lua has no animate binding today), a GUI part painter, and a z/bone overlay in the preview. | `ui-ux-director`, `mcp-integration-engineer`, `scripting-plugin-engineer` | `cli/`, `mcp/`, `core/src/Script.cpp`, `app/src/SpriteAnimateDialog.cpp` | 5a–5c |
| 8 | Docs, user guide and known issues. Re-audit with this ADR's acceptance tests. | `documentation-engineer`, `animation-director`, `qa-engineer` | `docs/user-guide.md`, `docs/agents/AUDIT_*` | 6, 7 |
| E | **Flame field** (independent; can start in parallel with step 1). | `environmental-effects-specialist`, `performance-engineer` | `core/src/fx/FlameField.cpp`, `Effects.cpp::fire` | 0 |

Steps 5a, 5b and 5c share step 4, so they can run in parallel once step 4 has landed. Within a module, work stays serial.

## 6. Acceptance tests (per creature)

All tests run on the golden fixtures with `anim_qa.py` and C++ `checkClip`. They are in addition to the universal rules: binary alpha, no new colours on bodies, seam ratio ≤ 1.5, not ping-pong, and no adjacent horizontal mirror.

**Dragon flap (8–12 frames) and hover**
1. Torso, head, legs and tail parts: area CV ≤ 3%. The whole silhouette may vary only through the wing parts.
2. Head presence (top-region guard on the head part) ≥ 95% in every frame. The head is never covered by a wing.
3. The far wing changes in every frame (its static fraction is 0). Its span is 0.6–0.8 × the near wing's. Its phase lags the near wing by 0.03–0.06. It has `z < torso` in every frame.
4. Bone lengths are constant to ±0 px (rigid bones). Joint angles stay within the template limits in every frame.
5. No frame shows a wing as ≤ 2 rows thick (no edge-on paper fold). Membrane area stays within 70–115% of rest.
6. The torso root moves opposite to the wing-tip vertical velocity. The head's vertical offset stays within ±1 px. The tail tip lags the root by ≥ 1 frame.
7. There are ≥ 8 unique poses, and the ping-pong detector reports False.

**Fish swim, dash and turn**
1. Body length (arc length along the spine) equals the source within ±1 px in every frame. Today it is 51–56 px against 57 px.
2. Lateral amplitude per spine bone increases monotonically head → tail. The head-bone yaw is ≤ 10% of the caudal yaw.
3. Pectoral, pelvic and caudal fin parts each change in ≥ 50% of frames. The caudal fin lags the peduncle by 0.1–0.2 cycle.
4. Turn clip: ≥ 3 intermediate views, no adjacent mirror pair, spine curl ≥ 30° at mid-turn, and wave phase continuous (phase error ≤ 0.1 cycle across view switches).
5. Swim at speed 0.5 versus 1.0 differs in beat frequency (not only in frame duration).
6. All parts stay attached: component count is constant.

**Humanoid walk and run (side, front and back views)**
1. Planted-foot sole: row error 0 px. x speed equals the ground speed exactly (in-place) for the whole contact phase.
2. Thigh and shin lengths are constant (rigid parts plus IK). Head and torso parts are byte-identical up to translation.
3. Contact, down, passing and up are present. The centre of mass is lowest one frame after contact and highest one frame after passing.
4. Pelvis and shoulders counter-rotate by ≥ 1 px.
5. Equipment parts (shield, sword) are pixel-identical to rest up to translation and an allowed rotation, never sheared.
6. Front and back views: the legs never cross or merge (the two leg parts never overlap by more than the outline), and the steps happen in place.

**Fire (campfire, torch, raging)**
1. No flame pixel is on canvas row 0 in any frame. The canvas grows instead.
2. ≥ 2 distinct tongues (separate local maxima of the flame top profile) in ≥ 50% of frames. At least one split and one merge per loop.
3. `diff_irregularity` ≥ 0.15. The silhouette changes every frame.
4. Embers have lifetimes of ≥ 3 frames, move upward, and cool through the ramp.
5. Logs and stones change by 0 px. There are no new colours. Isolated flame pixels are ≤ the source count.
6. Seam ratio ≤ 1.5.

## 8. Implementation status (2026-11-08)

Built without a schema change. The rig is rebuilt from the sprite each time a clip is made, and the poses are baked into frames as before, so projects store nothing new. Schema 4 (step 1) is only needed once rigs are edited and saved, which comes with the GUI part painter.

| Step | State | Where |
|---|---|---|
| 0 | Partly. `checkClip` uses one area limit (35 %) for every clip; the 8 % flap rule is gone. The top-region check skips roll, flap and fly. `checkClip` reports a frame that mirrors the one before. Golden fixtures `dragon128`, `fish57`, `rogue64`, `knight32` and `campfire64` are in `tests/data/anim/`, with expected results in `expected.json`, and are checked by `tests/test_part_rig.cpp`. The campfire's expected results cover only colours, the top row, held frames and frame count, not the other fire tests of section 6. | `SpriteMotion.cpp::checkClip` |
| 1 | Not done (schema 4). | – |
| 2 | Procedural segmentation per species module: wings (wing line, colour growth, two-wing split by components, then erosion watershed, then column valley), fish (tail stalk, fins by opening), legs (thigh, shin, foot per leg). No GUI hint mask beyond the existing wing selection. | `RigFlight.cpp`, `RigSwim.cpp`, `RigGait.cpp` |
| 3 | `inpaintHidden`: row and column closure under front parts, filled ring by ring, outline colour on the edge. Procedural only. | `PartRig.cpp` |
| 4 | `renderPose`: per-pose z, whole-pixel rigid moves, RotSprite turns from an 8× Scale2x copy, column bends (`colDy`), curl (`bend`), skinning to the body, and a per-part floor row (`maxY`). | `PartRig.cpp` |
| 5a | Two-bone IK legs; the planted foot moves back exactly `speed` px a frame; the hip drops to keep planted feet reachable; front views step in place; arms are rigid at the shoulder. Walk, run and sprint (≥ 4 frames); idle keeps the older rig. Legs under a quarter of the height (big-headed characters) have no knee: each boot is one rigid part and the piece above it stretches. A side-on biped's shoulder and pelvis bands shift against each other by whole pixels (not in front views, and not for four-legged bodies). | `RigGait.cpp` |
| 5b | Near and far wings as separate parts: the far wing behind the body, trailing (lag 0.045) and smaller (0.8); the near wing skinned into the back; the head end of the body drawn over the near wing; the body heaves against the beat. Flap and fly. The tail (the thin end of the body away from the head) waves in whole-pixel columns, the tip 0.3 of a beat late. Not done: the membrane mesh; `keypose::assemble` is not refitted. `flapRender` stays as the fallback. | `RigFlight.cpp` |
| 5c | Body bend by whole-pixel columns (length kept), the tail fin turning and foreshortened behind the stalk, fins paddling on their own phases. Idle, swim and dash. Not done: the turn clip (it needs views). `swimRender` stays as the fallback. | `RigSwim.cpp` |
| 6 | `zPops` runs on every flap, fly, swim and gait clip, and `checkClip` reports the result. No module swaps depth today, so this guards future work. | `PartRig.cpp` |
| 7 | Not done: CLI, MCP and Lua options, and the GUI part painter. They reach the rig through `animate()` already, with no rig options. | – |
| 8 | This section, KNOWN_ISSUES and CHANGELOG. No re-audit on the PC yet. | – |

Measured with a local build (zig c++ and a Catch2 stand-in) on 2026-11-08. CI is the authoritative build.
- dragon128: flap area change 12.7 %, fly 8.9 %, no `checkClip` issues, 8 unique flap frames.
- fish57: idle, swim and dash have no issues. Width is 53–60 px against 57 px: the tail fin foreshortens and, at the dash, turns out.
- rogue64 and knight32: walk, run and sprint have no issues. The rogue run had 2 floating frames, from a turned shin poking below its foot; `maxY` fixed it.

Acceptance tests of section 6 that tests now cover:
- Dragon 6, tail lag only (the tip trails the middle of the tail by ≥ 1 frame), 2 (head in every frame), 3 (the far wing moves every frame and goes behind the body), 5 (the wing keeps ≥ 70 % of its pixels and ≥ 3 rows) and 7 (8 unique frames).
- Fish 1 in a weaker form (bounding width, not spine arc length) and fish 2 in part (the tail moves more than the head).
- Humanoid 1 (the planted foot stays on its row at one speed), 2 (bone lengths through the IK test, and the head and torso parts copied whole), 4 (side views only) and 6 (front view).

The rest are not tested yet.

## 7. Consequences
- The schema bump to 4 is limited to rigged documents. Exports are unchanged, because they bake cels.
- CPU cost rises: segmentation and inpainting run once per sprite, while rendering is cheap. `performance-engineer` sets a budget: rig build ≤ 300 ms for 128 px sprites, and pose render ≤ 2 ms per frame.
- AI becomes optional input (segmenter, hidden-pixel inpainter, view and key-pose source) rather than the renderer. That keeps results deterministic and testable.
- The current renderers stay as a fallback for `Body::Object`/`Blob` until step 7 ships. Their QA verdicts do not change.
