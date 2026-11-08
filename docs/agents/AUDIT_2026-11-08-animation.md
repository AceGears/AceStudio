# Animation Re-audit: 2026-11-08 (current main)

Participants: Animation Director (12), with the Dragon (14), Aquatic (15), Character Motion (16), Effects (17) and Temporal Consistency (18) specialists.

Build: the installed `%LOCALAPPDATA%\Programs\AceStudio` (reports `AceStudio 0.10.0 (project schema 3)`; nightly-39d48e7 per the brief). Source read on branch `agents/anim-reaudit` (main + CI commit). No app or core code was modified, the GUI was not launched, and no diffusion model was loaded.

Evidence is in `docs/agents/audit-evidence/2026-11-08/`: enlarged crops, difference images, and `qa/*.json` (anim_qa reports without per-frame blobs). Raw outputs stayed in the session scratchpad.

## 1. How each motion is produced now (code reading)

| Motion | Algorithm | Where |
|---|---|---|
| Walk/run/idle (legged) | Whole-pixel rig. Legs are row stacks, and the knee comes from sideways row shifts. Length changes by dropping or doubling thigh and shin rows (`resizeRows`). The planted foot moves back at constant speed, and the swing foot follows an eased arc. Body bob is `sin(4πt)` (contact → down → passing → up), and the head lags one frame. Arms are sheared from the shoulder. The z-order is fixed: far leg, near leg, body, arms. | `core/src/PixelRig.cpp:20-39, 57-279` |
| Flap | `flapRender`. The wing mask (above a wing line, or painted) is folded about a single root row. Each row is redrawn at `R - d·fold(t - u/16)`, with keys `{1, .55, -.35, -.75, -.65, -.3, .35, .75, 1}` (fast down, slow up) and the tips trailing. Gaps are filled by repeating rows. The body rises on the downstroke. Both wings form one mask, drawn after the body. | `PixelRig.cpp:281-350` |
| Fly/hover and other non-rig actions | Legacy affine `FramePose` (translate, scale, shear, rotate, leg swing), sampled nearest-neighbour. | `SpriteMotion.cpp:170-315, 374-547` (Fly `:278`) |
| Swim/idle/dash (swimmer) | `swimRender`. Per-column lateral offset `lateral·envelope(u)·sin(2π(beats·t-lag) - 2πu/λ)` with 4 envelopes (anguilli-, subcarangi-, carangi-, thunniform). In side view it is shown as column dropping (`pos += 1/√(1+dz²)`) plus a 0.3× vertical wave. The caudal fin lags 0.1 beat. Dash doubles the beats and adds a translation lunge, and idle is a 1-px hover. | `PixelRig.cpp:352-439` |
| Swim style routing | By name (eel → anguilliform, tuna → thunniform, …), else by aspect ratio. | `AnimRouter.cpp:63-88`, `PixelRig.cpp:358-363` |
| Directions / turn | **GUI only** (Character Pack). Swimmers default to "rotate": the clip is mirrored for `w` and each frame rotated with `rotateSprite` for the other headings. There is no turn clip. | `app/src/pages/GeneratorPage.cpp:888-1031`, `SpriteMotion.cpp:893` |
| AI key-pose flap | **GUI only, needs FLUX.2 klein**. Four poses: `up` (original), plus `level`, `down` and `rising`, each inpainted around a protected body (2 variants, seed 4242, 4 steps). The result is reduced to the sprite palette and aligned on the bottom 38%, and body pixels with a near colour are restored. Assembled as 4 frames with no in-betweens. | `KeyPoses.cpp:9-223`, `app/src/SpriteAnimateDialog.cpp:340-430` |
| Fire | Generated every frame. A teardrop fuel shape is eroded by three octaves of rising tileable noise plus a swirl warp, quantised to a 6-step ramp of the sprite's own colours, with rim pixels clamped to level ≤ 2. Sparks follow seeded paths, and embers come from a noise threshold. Logs stay in front. The canvas is the source size, and the flame top is clamped at row 0. | `Effects.cpp:112-281` (`:231`) |
| App QA (procedural) | `checkClip`: area change ≤ 35% (**≤ 70% for flap**), single-frame flicker, floating feet, loop IoU. It has no anatomy, z-order, mirror or ping-pong checks. `AnimationQA.cpp::evaluateFrames` covers AI frames only. **The CLI reported `issues: []` for every clip below.** | `SpriteMotion.cpp:966-1051`, `AnimationQA.cpp:337-422` |

## 2. What was generated (installed CLI, procedural only)

| Asset | Command | Notes |
|---|---|---|
| Dragon | `animate original_dragon128.png --action flap,fly,idle` | Benchmark sprite. Body detected as **biped**. |
| Fish | `animate fish_reconstructed.png --action idle,swim,dash`; `--action swim --swim {anguilli,subcarangi,carangi,thunni}form`; `--face left` | `C:\Users\Danie\Downloads\acestudio-test-fish.png` **does not exist** (searched the whole profile). The source was reconstructed losslessly from the 11-07 evidence `swim8_death_e.png` cell 0: that frame is the identity pose, ×2 nearest with consistent 2×2 blocks, background keyed. It is 57×40 px with 32 colours (`input_fish_reconstructed.png`). The `fish` name hint routes it to subcarangiform. |
| Fish turn | Not reachable from CLI, MCP or Lua. | Only the GUI Character Pack makes headings. The `w` heading equals `--face left`, which is an **exact mirror of `e` in all 12 frames** (verified). The "turn" evidence is the `e` → `w` cut. |
| Campfire | `effect original_campfire64.png --effect fire --style campfire` (also `auto` and `raging`) | `auto` routes to campfire and gives a byte-identical result. |
| Humanoid | `animate --action idle,walk,run` on `original_rpg64.png` (rogue, 3/4 view), `original_char32.png`, and the `samples/knight.acestudio` character layers | The knight layers were extracted with a sandboxed Lua script (`ace.open`, `doc:cel`). |
| Not run | AI key poses (diffusion), the Character Pack, and every GUI-only path. | Diffusion was excluded by the brief, and the GUI was not launched. |

## 3. Metrics (anim_qa.py, extended 11-08)

| Clip | area CV | jitter | seam ratio | ping-pong (pairs i=n-i) | static frac | top-region presence (min, frame) | warnings |
|---|---|---|---|---|---|---|---|
| dragon flap (8) | **19.9%** | 4.0 | 0.52 | no (0/3, 8 unique) | 0.00 | **0.009 F5** | area, top-region F2–F7 |
| dragon fly (6) | 1.5% | 2.4 | 1.02 | no | 0.00 | **0.44 F3** | top-region F1–F5 (resampled scale) |
| dragon idle (4) | 1.6% | 0.36 | 2.38* | no | 0.00 | 1.0 | seam* |
| fish swim, subcarangiform (12) | 0.6% | 0.07 | 0.74 | no (0/5) | **0.48** | 0.77 | static |
| fish swim, anguilliform | 1.2% | 0.17 | 1.36 | no | 0.00 | **0.28 F9** | top-region |
| fish swim, carangi / thunniform | 0.7 / 0.5% | 0.1 | 1.35 / 0.57 | no / no (thunni: 6 unique in 12) | **0.69 / 0.78** | 1.0 | static |
| fish idle (12) | 0.8% | 0.41 | 10.4* | no | 0.00 | 1.0 | seam* |
| fish dash (5, one-shot) | 1.3% | 1.1 | – | no | 0.00 | 0.52 F2 | top-region |
| campfire (12, effect) | 0.8% | 0.14 | 0.58 | no (12 unique) | 0.64 (logs) | – | none; diff_irregularity **0.095** |
| raging (16, effect) | 1.0% | 0.19 | 0.62 | no | 0.64 | – | parts 1↔2 (torn tips); irregularity 0.082 |
| rogue64 walk / run (8) | 1.8 / 3.5% | 0.24 / 0.62 | 1.0 | no | 0.00 | 1.0 | none |
| char32 walk (8) | 3.2% | 0.24 | 1.02 | no | 0.00 | 1.0 | none |
| knight walk (8) | 3.0% | 0.34 | 0.93 | no | 0.00 | 1.0 | none |
| rogue / char32 idle (4) | 0.9 / 1.9% | 0.13 | 3.7 / 3.4* | no | 0.07 / 0.14 | 1.0 | seam* |

\* For these 4-frame idles and the 1-px fish hover, the seam step equals the largest in-cycle step (for example rogue F3→F0 0.49 against F1→F2 0.50). The high ratio comes from a median of 3 values, not from a pop. The loop seam was checked by eye and passes. The defect is the coarse 1-px whole-body step.

Additional measurements:
- **Dragon head presence.** A template of the head (x68–93, y64–89) matched against each flap frame: F0 1.00, F1 1.00, F2 0.90, **F3 0.19, F4 0.08, F5 0.46**, F6 1.00, F7 1.00.
- **Fish body length** against the 57 px source: subcarangiform 54–55, carangiform 55–56, thunniform 55–56, **anguilliform 51–52**. The first 31 of 54 columns from the head never change (subcarangiform).
- **Fire.** The flame top reaches **row 0 in campfire F2, F7, F8 and F9**, and in 10 of 16 raging frames. Logs and stones change by **0 px** in every frame. There are about 7 isolated pale-yellow (255,255,151) pixels per frame on the dark rim (26–29 isolated flame-zone pixels against 21 in the source).
- **Walk soles.** The rogue's planted foot moves back 2 px/frame at a constant speed (F1–F3: 8 → 10 → 12; F5–F7: 27 → 29 → 31). The knight and char32 show legs scissoring and merging in F3–F5.

## 4. Verdicts per checklist item

### Dragon (flap; fly)
| Item | Verdict | Evidence |
|---|---|---|
| Both wings animate; far wing foreshortened and phase-correct | **FAIL** | One mask: both wings fold identically in every frame, with no foreshortening or phase offset (`dragon_flap_x2.png` F0–F7). In the fly clip the wings are static (affine bob). |
| Wing chain shoulder → elbow → wrist → digits | **FAIL** | No joints. Rows fold about the root row, and the only "chain" is the 1/16-cycle tip lag (`PixelRig.cpp:312`). |
| Membrane stretch/slack; no paper folding | **FAIL** | F2 is an edge-on single-row "wing" line. F3–F6 hang the membrane upside down below the root (mirrored rows). Gap filling repeats rows. |
| No wing through torso; z-order correct | **FAIL** | The wings are drawn over the body. The near wing covers the head and eye in F3–F5 (head presence 0.19 / 0.08 / 0.46; `dragon_flap_head_F0F2F4F5_x4.png`). The far wing is also in front. |
| Torso heave, head stabilised, tail lag | **PARTIAL** | The body rises on the downstroke (F2–F5). The head moves with the body, and the tail is rigid. |
| Anatomy intact | **FAIL** | Head hidden F3–F5. Area CV 19.9% (limit 8%). Fly F2/F4: a 1-px transparent tear across the hips at the leg line (row 102, 13 px; `dragon_fly_feet_F0F2F4_x4.png`). |
| Loops seamless | **PASS** | Flap seam 0.52, 8 unique poses, not ping-pong. Fly seam 1.02. |
| **Overall** | **FAIL (improved)** | Against 11-07: the ping-pong with 5 poses is gone, and the timing is now asymmetric with tip lag and heave. The vertical squash has become a row fold. The head is still lost (11-07 F4, now F3–F5). |

AI key-pose flap (`0f15b26`): **not checked**, because it needs a diffusion model and the GUI. From the code: 4 frames with no in-betweens, poses generated independently (no temporal constraint), alignment on the legs only, and `restoreBody` overwriting wing pixels whose colour is near a body colour. These are risks to depth and continuity that must be verified on real output.

### Fish (swim, idle, dash, turn)
| Item | Verdict | Evidence |
|---|---|---|
| Swimming mode matches species | **PASS (new)** | The four envelopes behave as specified: anguilliform moves the whole body (0 static columns), subcarangiform the rear ~45%, and thunniform only the peduncle and tail (static 0.78). Styles are chosen by name or aspect. |
| Travelling wave head → tail, amplitude rising, small head yaw | **PARTIAL** | The wave travels and grows toward the tail, and the head is still. The lateral bend is shown only as column dropping, which reads as texture shimmer in the rear half (`diff_fish_F0F3_F0F6_x4.png`), plus a ±1–2 px vertical tail shift. |
| Fins phase-coordinated | **FAIL** | Fins are not parts. The pectorals and pelvics never move independently (the front 31 columns are frozen), and the caudal moves only as columns. |
| Turns: spine curl, intermediate views, no mirror | **FAIL** | No turn exists. The CLI, MCP and Lua have no direction or turn tool. The GUI heading `w` is the exact mirror of `e` (12/12 frames identical to the mirror; `fish_turn_e_to_w_x4.png`), and the other headings are `rotateSprite` rotations. |
| Accel/decel changes frequency and amplitude | **PARTIAL** | Dash doubles the beat and raises the amplitude, but it is also a 4–5 px translation lunge (F1–F3). There is no ramp. |
| Anatomy (fins attached, body length constant) | **FAIL** | Fins stay attached (1 component). The body is 2–6 px short in every frame (anguilliform up to −10%). The idle is a whole-body 1-px hover (F1 → F2, F4 → F5, F6 → F7, F11 → F0). |
| **Overall** | **FAIL (improved)** | Against 11-07: the bend is no longer purely in the wrong plane, and species modes exist. Fins are frozen, turns are still mirrors and rotations, and the body length is not constant. |

### Humanoid (rogue 3/4, char32, knight front view)
| Item | Verdict | Evidence |
|---|---|---|
| Gait phases contact/down/passing/up | **PASS** | Body lowest at F1/F5 and highest at F3/F7, with the head lagging one frame (top rows 2, 3, 4, 3, 2, 3, 4, 3). |
| Contact foot does not slide | **PASS for side/3-4 view; FAIL for front view** | Rogue: the planted foot moves back at a constant 2 px/frame (in place). Knight and char32 (front-facing) walk like side views: the legs scissor and merge in F3–F5 (`knight_walk_x10.png`, `char32_walk_x8.png`). Front detection needs 80% mirror symmetry, and the knight's shield breaks it (`PixelRig.cpp:72-82`). |
| Limb lengths and proportions constant | **PARTIAL** | Thigh and shin lose or gain rows for the knee bend (`resizeRows`, ≥ 65% kept). Head and torso are rigid (top-region presence 1.0 everywhere). |
| Pelvis/shoulder counter-rotation; centre of mass | **FAIL / PASS** | The torso is a rigid block with no counter-rotation. The centre of mass rises and falls correctly by 1 px. |
| Equipment rigid; clothing follows through | **FAIL** | The knight's shield (detected as an arm) is sheared in F1, F3, F5 and F7 (`knight_walk_x10.png`). The rogue's cloak hem is static. |
| Loop | **PASS** | Walk seam 0.93–1.02. |
| **Overall** | **FAIL (improved)** | Against 11-07 (feet sank, 1-px cel offsets): there is now a real whole-pixel walk cycle with correct phases and no foot sinking. The remaining failures are front-view handling, rigid equipment and torso rotation. |

### Fire (campfire; raging)
| Item | Verdict | Evidence |
|---|---|---|
| Simulated or procedural, not a warped still | **PASS (new)** | A new flame is generated every frame from rising noise (`Effects.cpp:146-281`). |
| Tongues rise, split, merge; irregular change | **PARTIAL** | The silhouette changes every frame, and the tips tear off (raging part count 1↔2). It is a single teardrop envelope that never splits into separate tongues. `diff_irregularity` is 0.095 / 0.082 (constant-rate rise). |
| Embers follow the flow and fade | **PARTIAL** | Sparks rise and cool through the ramp (raging F0, F3, F10, F11), but there are only 2–5 of them and they are single pixels. Ember lifetime was not measured. |
| Loop seamless | **PASS** | Seam ratio 0.58 / 0.62. |
| Palette coherent, no orphan noise | **PARTIAL** | No new colours (29 = frame 0). About 7 isolated pale-yellow rim pixels per frame. |
| Static parts stable | **PASS (fixed)** | Logs and stones change by 0 px in all frames (11-07: 109–145 px/frame). |
| Flame not clipped | **FAIL** | The tip is at row 0 in campfire F2 and F7–F9 and in raging 10/16 frames (`Effects.cpp:231`, canvas = source size). |
| **Overall** | **FAIL (minor; largely improved)** | The remaining defects are clipping, single-tongue shape, mechanical rate and rim speckle. |

## 5. Owners and fix requests (summary; the full plan is in ADR-ANIM-001)
- Dragon → `rigging-engineer` + `dragon-animation-specialist`: separate near and far wing parts with z, a wing bone chain, a membrane mesh, and a hidden-pixel inpaint of the head and neck. Remove the `flap` 70% area exemption from `checkClip`.
- Fish → `aquatic-animation-specialist`: a spine chain that conserves arc length, fin parts with oscillators, and a turn clip with real views. No mirror or rotate headings for swimmers.
- Humanoid → `character-motion-specialist`: front/back stepping in place, a front-view detector independent of equipment, rigid equipment parts, and torso counter-rotation.
- Fire → `environmental-effects-specialist`: grow the canvas upward, use multi-source tongues, apply a monotonic rim ramp, and add frequency variety.
- QA → `temporal-consistency-engineer`: port the ping-pong, top-region and mirror checks to C++ `checkClip`.

## 6. anim_qa.py changes (this audit)
- `pingpong(frames)` checks whether frame i equals frame n-i for every pair. It reports matched pairs and unique poses, and warns when every pair matches and n ≥ 4.
- `top_region_presence(frames)` finds the top-most opaque region of frame 0: the 4-connected piece holding the highest pixel, within the top 20% of the bbox. It is template-matched (exact colours, search ±max(4, h/6) px) in every frame, and the check warns when a frame keeps less than 60% of it (more than 40% lost). It applies to body clips only.
- Self-tests: `.claude/skills/animation-quality/scripts/test_anim_qa.py`, stdlib unittest with synthetic images. **11 tests, all passed** (`python -I …/test_anim_qa.py -v`, 0.03 s).
- Calibration on the old 2026-11-04 benchmark flap GIFs (`original_flap.gif` and `optimised_flap.gif`, whose 11-07 verdict was "ping-pong, 5 unique poses"): both report **ping-pong True, 3/3 pairs (1,7), (2,6), (3,5), 5 unique poses**, which reproduces the manual 11-07 finding. The top-region guard drops to 0.00–0.01 in F2–F6. The current flap reports ping-pong False with 8 unique poses. On current clips the guard flags the dragon flap (wing tip, plus the head when measured with a head template), the anguilliform fish and the dash. It reports 1.0 on all walks. A top-region warning must be read with the frames: it can be a wing tip folding legitimately.

## 7. Not checked
- The AI key-pose flap, the Character Pack (directions and AI views) and every GUI preview: they are GUI-only, need diffusion, or both.
- The real `acestudio-test-fish.png`: it is missing. The reconstruction is pixel-exact for the old build's identity frame, but the original file could differ outside the sprite box.
- Quadruped gait, sub-pixel and cluster stability on large sprites, and timing at the intended FPS. Frames were reviewed as strips, not played back.
- Engine imports of these clips.
