# Why the hero walk failed, and what to fix

Date: 2026-11-12. Status: research and recommendations. Fixes 2 and 3 are implemented (see the table); the rest are not.

## What happened

A user gave AceStudio a generated 4 × 4 walk sheet (a 250 px, big-headed hero on a black background; four directions, four poses each) and asked for animations. Three attempts were delivered and all were rejected:

1. The sheet's own frames, played in sheet order. Jumpy: a limp, a stall, and a standing pose that did not match the stepping poses.
2. The same frames reordered, with cleaner edges. Still "not smooth".
3. Animate Sprite (the part rig) on one standing pose per direction, 8 frames. "Terrible": the boots slide, nothing else moves, and the side views have ragged edges.

Every one of these passed `checkClip` with no issues. Each was judged acceptable from stills laid out side by side, never from playback.

## Findings

### 1. A walk is a set of drawn poses, and the rig cannot draw them

Animation teaching describes a walk by its key poses: contact, down, passing and up, with four drawings as the practical minimum (contact, passing, and their mirrors) [1][2][3]. The poses differ in the shape of the legs and arms, not only in where they are.

The part rig moves, turns and stretches pieces of one drawing. For legs under a quarter of the height it does not even bend a knee: the boot slides and lifts as one piece. That cannot produce a contact pose or a passing pose. Our own standard already says so: `ANIMATION_STANDARDS.md` rule 3 requires motion through joints, and rule 5 requires arcs, follow-through and overlapping action.

Cutout animation is known to look stiff without deformation; users of skeletal tools say the same, and squash, stretch and mesh deformation are the usual remedies [4][5]. Sources on walk quality name the same three faults we shipped: feet that slide (the illusion of friction is lost), legs with no bend, and no counter-rotation or arm swing [6][7].

Conclusion: for a detailed character, the procedural rig is the wrong tool for a walk. It is a fit for small, simple sprites and for motions that really are rigid (a bob, a flap about a hinge, a fish bend).

### 2. The check passed because it does not look for a walk

`checkClip` measures area change, frame-to-frame overlap, the loop seam, flicker, mirrored frames, depth pops and floating feet. None of these asks whether the legs take poses. A sprite that slides rigidly passes all of them.

An open-source walk-cycle generator checks different things: frames half a cycle apart must lead with opposite legs; head size and floor are locked to one master frame; the planted foot must move the same distance every frame; limbs must match the pose guide. Frames that fail are redrawn with a new seed. Even so, its authors report 73 % of frames passing at the first seed and 4 of 12 cycles passing a first look, and call the output "drafts that are checked, not finished art" [8]. Research on generated animation also finds that appearance and motion quality correlate only moderately, so they need separate measures [9].

### 3. Drawn frames fail when the upper body changes

Attempts 1 and 2 used real drawn poses and were still jumpy, because the head, torso, hands and details were redrawn in every frame. Vendors and tools that make sprite sheets all converge on the same remedy: lock identity, proportions, palette and scale to one reference, vary only the pose, and align every frame to one origin and ground line [10][8]. The classic RPG sprite format works on that basis: a neutral frame and two step frames per direction [11]. (Why three frames is enough at that size is our reasoning, not something the sources state.)

### 4. Generating poses is the working approach, and it is not yet reliable

Sprite Sheet Diffusion conditions on one reference image plus a pose sequence, with a reference network for appearance, a pose guider, and a motion module for smoothness. The authors report lost fine detail (hair, props), weak subject consistency and a small dataset [12]. The walk-cycle generator above uses a depth mannequin (near limbs bright, far limbs dark, so the model knows which leg is in front), a video model for the whole cycle, then per-frame sharpening from one master frame; it needs an 8 GB GPU and about 20 minutes per 12-frame cycle on an older card [8].

AceStudio already has pose-conditioned generation (OpenPose ControlNet + IP-Adapter). The depth-mannequin idea and the per-frame checks with redraw are what it lacks.

### 5. Interpolating between drawn frames does not work for pixel art

Flow-based interpolators blend the two frames; users report blended colours and lost patterns on pixel art, and the research notes blur and ghosting under large motion [13][14]. Not worth building.

### 6. A black background is the wrong background

The cutter keys on closeness to the border colour. On a black sheet, black outlines and shading are the background colour, so edges are lost wherever they touch it. Guides on sprite keying say to choose a key colour the sprite does not contain (magenta or bright blue when green is in the art), to flood from the corners with a tight tolerance rather than key globally, and to check edges against a dark backdrop [15][16]. Several tools use a segmentation model instead of a colour key [8][16]. AceStudio has both pieces: it controls the generation prompt, and it ships BiRefNet for `ai remove-bg`.

### 7. The review process hid the problem

The frames were judged from contact strips. A strip shows that each frame is clean; it does not show sliding, stiffness or popping. The searches found no source on review workflow, so this finding rests on what happened here, not on the literature.

## What to fix

Ordered by how much each would have changed this outcome.

| # | Fix | Where | Size |
|---|---|---|---|
| 1 | **Route by what the sprite needs.** When the input is a sheet that already has poses, use them; do not re-animate one frame. When the subject is a detailed character (large, many colours, short or overlapping legs), do not offer the rig's walk as the result: say that a walk needs drawn poses and route to the pose pipeline. | `anim_router`, Animate Sprite dialog, CLI `animate` | Medium |
| 2 | **Done in the core and the CLI** (`stabilizeCycle`, `acestudio-cli cycle`); not in the app's dialogs. **A "stabilise a drawn cycle" tool.** From a sheet of drawn poses: one fixed head and upper body per direction, drawn legs swapped underneath, one origin and ground line, a whole-pixel bob, frame order fixed by which leg leads. | new, core + CLI + dialog | Medium |
| 3 | **Partly done:** legs that only slide (figures of 96 px and more) and the same leg stepping twice (front views). Not done: planted-foot speed, and motion above the legs. **Walk checks in `checkClip`.** Legs take at least two distinct shapes; frames half a cycle apart lead with opposite legs; a planted foot does not move relative to the ground speed; something above the legs moves against them. A walk that only slides must fail. | `SpriteMotion.cpp::checkClip`, `anim_qa.py` | Medium |
| 4 | **Generate on a key colour, cut with segmentation.** Ask for a flat magenta (or a colour absent from the palette) background in sheet prompts; in the cutter, prefer BiRefNet when the runtime is there, and fall back to the border key. Warn when the background colour also appears inside the subjects. | `AssetGen.cpp::gridPrompt`, `extractGrid`, generator page | Medium |
| 5 | **Pose pipeline upgrades.** Depth-mannequin conditioning for near and far limbs; lock head size and floor to a master frame; per-frame checks with redraw on a new seed; a review sheet that boxes failed frames. | `ai_worker`, animation pipeline | Large |
| 6 | **Review in motion.** Export a GIF or APNG with every preview; in the audit skills, require playback (or a difference strip and a foot-track plot) before a clip is called acceptable. | `.claude/skills/animation-quality`, CLI | Small |
| 7 | **Say what the rig is for.** In the dialog and the user guide: the rig suits small sprites and rigid motions; it does not draw new poses. | `docs/user-guide.md`, dialog text | Small |

Not recommended: frame interpolation (finding 5), and further tuning of the short-leg shuffle (finding 1).

## Limits of this research

- Most claims about consistency methods come from vendor pages and one open-source project, not independent benchmarks.
- No source measured dark outlines under different key colours; fix 4 needs a test on our own sheets.
- The paper [12] was read in part (the page was cut off), and its dataset counts were inconsistent between the text and the summary.
- Nothing here has been tried in AceStudio.

## Sources

1. itch.io, "Pixel art walk cycle animation": https://itch.io/blog/526535/pixel-art-walk-cycle-animation
2. Sprite AI, "How to animate pixel art" (vendor): https://www.sprite-ai.art/guides/how-to-animate-pixel-art
3. Pluralsight, "12 steps to a great walk cycle animation": https://www.pluralsight.com/blog/tutorials/12-steps-walk-cycle-animation
4. Esoteric Software forum, "Deforming body parts": https://tr.esotericsoftware.com/forum/d/1007-deforming-body-parts
5. Wulverblade, "Frame vs. mesh animations": https://wulverblade.com/frame-vs-mesh-animations/
6. SIGGRAPH Education, "Learning to walk": https://education.siggraph.org/static/HyperGraph/animation/character_animation/walking/learning_to_walk.htm
7. Cursa, "Building a walk cycle with contacts, passing, and extremes": https://cursa.app/en/page/character-animation-starter-kit-building-a-walk-cycle-with-contacts-passing-and-extremes
8. MareArts, marearts-moim-pose (README): https://github.com/MareArts/marearts-moim-pose
9. Animation2Code (arXiv 2606.28593): https://arxiv.org/pdf/2606.28593
10. Sloyd, sprite sheet generator (vendor): https://www.sloyd.ai/spritesheet-generator
11. itch.io forum answers on RPG Maker frames: https://itch.io/post/10325809
12. Sprite Sheet Diffusion (arXiv 2412.03685): https://arxiv.org/abs/2412.03685
13. itch.io forum thread on RIFE and pixel art: https://itch.io/post/3066631/view-in-topic
14. arXiv 2512.18241 (frame interpolation, limits of flow-based methods): https://arxiv.org/pdf/2512.18241v1
15. Loopsprite, "Remove green background" guide: https://loopsprite.pages.dev/extract/guide-remove-green-background.html
16. Chroma-key hill-climbing write-up: https://idvorkin-ai-tools.github.io/chroma-key-explainer
