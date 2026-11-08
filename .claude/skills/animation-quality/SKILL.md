---
name: animation-quality
description: Evaluate an AceStudio animation (sprite sheet, frame folder or GIF) for anatomy, depth, motion quality, temporal stability and loop continuity, producing a PASS/FAIL review with frame-referenced evidence. Use whenever an animation is produced, changed, or reported as looking wrong (dragon wings, fish swimming, walking, fire/effects).
---

# Animation Quality Review

Metrics find candidates; **eyes decide**. A review is never complete without looking at the frames.

## 1. Collect
- The exported sheet/GIF and its frame metadata (Aseprite/Phaser JSON or Godot `.tres`), frame size, intended FPS, whether it should loop, creature type, and the reference sprite if any.

## 2. Run the metrics (stdlib Python, no installs)
```bash
python -I .claude/skills/animation-quality/scripts/anim_qa.py <sheet.png> --json <sheet.json> --report <out.json>
python -I .claude/skills/animation-quality/scripts/anim_qa.py <sheet.png> --grid 8x1
python -I .claude/skills/animation-quality/scripts/anim_qa.py <fire.gif> --kind effect
```
Exit code 1 means warnings were raised. Metrics reported:
| Metric | Catches |
|---|---|
| `area_cv` | proportion drift, growing/shrinking/missing parts (body only) |
| `component_counts` changing | detached or missing limbs/wings/fins |
| `centroid_jitter_px` | registration jitter in in-place cycles |
| `palette_union` vs `palette_frame0` | palette drift between frames |
| `semi_alpha` | non-binary alpha in pixel art |
| `loop_seam_ratio` | pop when looping (last→first change vs typical change) |
| near-zero `consecutive_diff` | held/duplicate frames |
| horizontal-mirror pairs | abrupt sprite flipping on direction change |
| `static_frac` | parts that never move (e.g. a frozen far wing) |
| identical area+palette with motion | whole-sprite warp/translation instead of articulation |
| `diff_irregularity` (effects) | low value = mechanical, periodic flicker |

## 3. Look at the frames
Use the Read tool on the sheet and on enlarged crops (nearest-neighbor ×4). Check frame by frame:
1. **Anatomy** — same body parts in every frame, attached, proportions constant.
2. **Articulation** — motion through joints (shoulder→elbow→wrist; spine wave; hip→knee→ankle), not rotating/stretching the whole image.
3. **Depth** — far limbs/wings/fins behind the body, near ones in front; no part cuts through the torso.
4. **Motion** — arcs, ease-in/out, anticipation, follow-through, overlapping action (tail, hair, fins lag).
5. **Weight/contact** — feet planted during contact (no sliding); body heave responds to wingbeats/steps.
6. **Turns** — direction changes go through intermediate poses, never a 1-frame mirror.
7. **Loop** — last→first frame is a normal step.
8. **Pixel art** — grid, palette, outlines and clusters stable; no shimmer on rigid areas.

Creature-specific checklists: see `docs/agents/ANIMATION_STANDARDS.md` (dragon, fish, humanoid/quadruped, fire).

## 4. Report
```
Animation: <file>  Frames: <n> @ <fps>  Type: <creature/effect>
Metrics: <key numbers + warnings>
Verdict: PASS | FAIL
Defects:
  - F<frame>[ (x,y)]: <what is wrong> → owner: <agent>  fix: <specific request>
Evidence: <paths to report JSON / crops>
Not checked: <anything skipped and why>
```
