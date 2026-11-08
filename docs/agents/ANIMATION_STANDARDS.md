# Animation Standards

Owner: `animation-director`. Thresholds are enforced by `.claude/skills/animation-quality/scripts/anim_qa.py`. Change a threshold only with an ADR, and never to make a failing animation pass.

## Universal rules
1. **Anatomy constant.** The same parts are visible or correctly occluded in every frame. Bone lengths are constant. Silhouette area varies ≤ 8% for rigid-bodied creatures unless the motion explains it (wings opening, for example).
2. **Attached.** The count of connected parts never changes unless something is actually thrown or detached.
3. **Articulated.** Motion goes through joints. Rotating, scaling, shearing or mesh-warping the whole sprite does not count as animation.
4. **Depth-correct.** Far-side limbs, wings and fins render behind the body and near-side ones in front. Z-order swaps only when the geometry crosses.
5. **Natural motion.** Arcs, ease-in/ease-out, anticipation, follow-through, overlapping action, and weight that responds to forces.
6. **Turns, not flips.** A direction change passes through intermediate poses. Mirroring between adjacent frames counts as a defect.
7. **Loops.** The last→first change is ≤ 1.5× the median frame-to-frame change. No duplicate frames unless they are held intentionally and documented.
8. **Pixel art.** Binary alpha, a stable palette (no new colours after frame 0 for bodies), stable clusters on rigid areas, and centroid jitter ≤ 1 px for in-place cycles.

## Dragon / flying creatures (QA checklist)
- [ ] Both wings animate through the whole cycle. The far wing is foreshortened and phase-correct, never static.
- [ ] The wing chain goes shoulder → elbow → wrist → digits. Joint angles stay within limits.
- [ ] Membranes stretch on the downstroke and slacken on the upstroke, with no paper folding and no shear tearing.
- [ ] No wing passes through the torso. Z-order is correct in every frame.
- [ ] The torso heaves against the wingbeat, the head stabilizes, and the tail lags with overlap.
- [ ] Anatomy is intact (limbs, horns, tail count and lengths constant).
- [ ] Flap/hover/glide loops are seamless.

## Fish / aquatic (QA checklist)
- [ ] The swimming mode matches the species: anguilliform has a whole-body wave; carangiform moves the rear third; thunniform moves the peduncle and tail only.
- [ ] A traveling wave runs head → tail, with amplitude increasing toward the tail. The head yaw stays small.
- [ ] Fins are phase-coordinated: pectorals brake and hover, the caudal fin provides thrust.
- [ ] Turns use spine curl and intermediate views. No 1-frame mirror. Wave phase continues through the turn.
- [ ] Acceleration and deceleration change frequency and amplitude, not just playback speed.
- [ ] No broken anatomy (fins attached, body length constant).

## Humanoid / quadruped (QA checklist)
- [ ] Gait phases (contact, down, passing, up) are present, and quadruped footfall order is correct for the gait.
- [ ] The contact foot moves 0 px relative to the ground while planted (no sliding).
- [ ] Limb lengths and head/body proportions are constant.
- [ ] Pelvis and shoulders counter-rotate, and the center of mass rises and falls plausibly.
- [ ] Equipment stays rigid on its bone, and clothing follows through without tearing.

## Fire / effects (QA checklist)
- [ ] The effect is simulated or procedurally generated. A static flame image that is rotated, stretched or warped fails.
- [ ] Tongues rise, split and merge. The silhouette changes every frame, irregularly (`diff_irregularity` reported, and reviewed together with the frames).
- [ ] Embers follow the flow and fade.
- [ ] The loop is seamless (seam ratio ≤ 1.5).
- [ ] The palette ramp is coherent. No orphan-pixel noise.
- [ ] Static parts (logs, torch handle) stay perfectly stable.

## Review evidence required
Frame strip, metrics JSON, enlarged crops of defects, verdict per checklist item, and a list of what was not checked.
