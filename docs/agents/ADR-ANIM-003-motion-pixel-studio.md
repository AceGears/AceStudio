# ADR-ANIM-003: Motion & Pixel Studio, the plan for the November 2026 overhaul brief

| | |
|---|---|
| Status | Accepted as a staged plan on 2026-11-15. Two stages are built: pixel-art in-betweens (this change) and glTF/GLB motion import (its own change; see ADR-ANIM-002). Everything else in section 3 is **planned, not built**. |
| Builds on | ADR-ANIM-001 (part rig), ADR-ANIM-002 (Motion Studio, BVH retargeting) |
| Input | The "Advanced Animation, Intelligent Sprite Generation & Professional Pixel Art Editor" brief (12 phases), with four reference videos: 3D-to-2D retargeting in a Bonebase-style Aseprite extension; the same tool's chibi template → import → camera turn → fewer frames → draw over workflow; a RIFE-based in-betweener for Aseprite (intermediates 1/3/7, K-centroid downscale, palette quantize, split/fixed timing, area check); and a vector pixel editor (pen with anchors, shapes, dithered gradients, blend modes). The videos were used for concepts only. No code, models or assets were taken from them. |

## 1. Rules that shape every stage

- **No paid cloud use without the user's approval**, and no public unauthenticated GPU endpoint. Every AI step stays optional, local first, with an explicit cost prompt when it is remote (`docs/agents/CLOUD_GPU_DESIGN.md`).
- **Pixel-art safety first.** Nothing may blend colours or add partial alpha to pixel art unless the user asks for it. That is why the in-betweener below is not a neural interpolator.
- **Nothing is called done until it has been run.** App code that cannot be compiled locally (Qt is not in the local toolchain) is marked "CI only" until CI and the UI audit have run it.
- **Working systems are kept.** New paths are added beside the procedural animator, the part rig and Motion Studio, not in place of them.

## 2. Decision: in-betweens by moving pixels, not by a neural interpolator

The reference tool runs RIFE and then repairs its output (downscale, quantize to palette, harden alpha). For pixel art that repair is lossy: RIFE blends colours and blurs edges, and the clean-up has to guess them back. AceStudio instead:

1. Matches every pixel of key A to key B and back (patch matching over 5×5 neighbourhoods, the centre weighted, with propagation and random search; ties prefer the figure's overall shift, so flat colour areas move together).
2. Cleans the motion field with a median filter and marks matches that disagree both ways.
3. Moves each pixel part of the way, drawing at 3× scale, and brings it back by a per-pixel majority vote (the K-centroid idea, with no new colours). A small tidy pass fills pinholes and removes isolated specks.
4. Checks each in-between's area against what the keys suggest and flags those off by more than the tolerance (default 25%), because a pose that changes too much needs a drawn key, not an in-between.

Every colour comes from a key, so the palette is kept and alpha stays 0 or 255. No model download, no GPU, about 250 ms per pair on a 132×249 sprite. What it cannot do is invent what neither key shows (a limb turning round). A neural interpolator stays possible later as an optional AI-worker step, after this one, never in its place.

## 3. The brief's phases against AceStudio today

| # | Phase in the brief | What exists | This change | Next stage (not built) |
|---|---|---|---|---|
| 1 | Unified workspace | Workspace rail with Draw / Animation / AI Studio / Assets pages; Motion Studio is a dialog | — | Motion Studio as a dockable page (ADR-ANIM-002 stage 8) |
| 2 | Professional pixel editor | Raster layers, masks, adjustment layers, blend modes, selection, symmetry, palettes, onion skin, tags, linked cels | — | Vector shape layers (pen with anchors, rectangle, ellipse, auto-close) rasterised pixel-perfectly; dithered gradient fill (ordered Bayer); stroke styles |
| 3 | Auto and manual rigging | Automatic part rig per clip (ADR-ANIM-001), not saved or editable | — | Saved, editable rigs: bone tree, overlay on canvas, IK chains, joint limits (needs the schema bump in ADR-ANIM-001) |
| 4 | Retargeting, glTF/FBX | BVH import and retarget (ADR-ANIM-002) | **Built**: glTF/GLB animation import (Motion Studio and `retarget --animation`; CI-verified, not yet used on a PC) | Motion library presets. FBX stays unsupported: the SDK is proprietary and reverse-engineered parsers are a large attack surface; convert with Blender |
| 5 | 2.5D | Camera turn/tilt/roll on retargets, flagged "approximate view" beyond 30° | — | Direction sets (drawn views chosen by camera turn), the honest form of 2.5D for pixel art |
| 6 | AI sprite generator | Local SDXL + pose ControlNet, characters, background removal, upscale | — | Hidden-part inpainting for rigs, view generation; local or approved cloud only |
| 7 | Frame interpolation | — | **Built**: in-betweens (core, CLI `inbetween`, Animation › Generate In-betweens…) | Optional neural interpolator as an AI-worker step, followed by the same palette-safe clean-up |
| 8 | Timeline with curves | Frames with durations, tags, linked cels | In-betweens follow split or fixed timing | Keyframed bone and property tracks; linear, Bézier, ease and stepped curves; clip blending |
| 9 | Creature animation | Gait (biped, quadruped), flight, swim modules on the part rig | — | Non-humanoid motion mapping (ADR-ANIM-002 stage 3c) |
| 10 | Style consistency | Pixel-art QA (palette, stray pixels, outline), clip checks | In-betweens keep the key palette by construction | Palette lock across a project; style reference for AI steps |
| 11 | Performance / cloud | Measured baselines (`PERFORMANCE_BASELINE.md`); cloud GPU designed, off | In-betweens run on a worker thread with cancel | Nothing paid without approval |
| 12 | QA and Windows installer | Catch2 suite, UI audit, fuzzing, CI on Linux and Windows, installer pipeline | Tests for the in-betweener (9 cases) | Release only through `docs/RELEASE.md`'s gates and the user's go-ahead |

## 4. What is built in this change

| Piece | Where | Verified |
|---|---|---|
| Motion field, in-betweens of a pair, sequences with split/fixed timing and loop, document layer in-betweening (other layers hold their key, tags that end on a key grow, durations set), cancellation, limits (1–7 intermediates, 1024×1024, frame cap) | `core/include/acestudio/core/Inbetween.h`, `core/src/Inbetween.cpp` | `tests/test_inbetween.cpp`, 9 cases, run locally (zig clang). The midpoint of a moved figure equals the moved figure exactly; no new colours appear; bad input and cancel are refused |
| `acestudio-cli inbetween` (keys or `--strip N`, `--n`, `--tolerance`, `--ms`/`--fixed`, `--loop-back`; writes strip PNG, GIF and JSON with flagged frames) | `cli/AssetCommands.cpp` | Strict-warning syntax check only; first full build is CI. The same core call was run on the user's 8-key hero walk (16 frames, about 2 s) and the frames were checked by eye: shapes hold, slight raggedness where boot and trousers overlap |
| Animation › Generate In-betweens… (selected block of frames as keys, or all frames; current layer; worker thread; one undo step; refuses to apply if the document changed meanwhile) | `app/src/InbetweenDialog.*`, `MainWindow.cpp`, UI audit list | **CI only.** Qt is not in the local toolchain and computer use was not allowed, so the dialog has not been opened |

## 5. Risks

- Large pose changes produce torn in-betweens. The area check flags many of these but not all (a limb can move without changing the area). The dialog and CLI say which frames to look at.
- Overlapping parts of one colour (boots over trousers) can swap pixels between them. Separate layers per part avoid it.
- The work copies the whole document, so a very large animation uses twice its memory while it runs.
