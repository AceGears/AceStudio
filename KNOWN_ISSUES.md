# Known Issues and Limitations

Current as of 0.10.0. Everything that could be fixed in code for Phases 1–8 has been fixed and
tested; what remains below needs hardware we have not had, is a limit of a third-party
component or file format, or is a deliberate design choice. Nothing is listed as done that has
not been run.

## Needs hardware or machines not yet available (Phase 9 test matrix)
- **Clean Windows install:** the package ships the Visual C++ runtime DLLs next to the
  executables, and CI fails if any shipped binary imports a runtime DLL that is not packaged.
  Starting it on a Windows install that has never had the redistributable has not been tried.
- **Per-monitor DPI:** 150% and 200% scaling were checked (Linux, `QT_SCALE_FACTOR`), and the
  test PC runs the app at its own scaling. Dragging the window between monitors with different
  scaling has not been tried.
- **GPUs other than the test PC's:** everything AI was verified on one RTX 4070 Laptop (8 GB,
  driver 621.01).
  - SDXL at 1024², 30 steps, with model CPU offload: ~34 s warm, 50–56 s cold.
  - Animation frames (SDXL + pose ControlNet, optional IP-Adapter, FP8 weight storage, 28
    steps, background removal included): 46–56 s per frame warm, ~73–84 s for the first.
  - BiRefNet on CUDA: 2.6 s warm.
  - Real-ESRGAN 512→2048: 3.1 s.

  12 GB+ NVIDIA cards (SDXL resident), AMD/Intel GPUs on DirectML, NVIDIA drivers older than
  R580 (PyTorch cu126 path) and the CPU-only PyTorch profile have not been installed on a
  Windows PC. The Models dialog's VRAM figures are estimates and say so.
- **Real-display frame rate** on Windows is not measured, only compositing times (see below).

## Limits of third-party components and formats
- **Motion import:** BVH and glTF/GLB animations are read. FBX (proprietary SDK) is not planned:
  export it to glTF or BVH from Blender. glTF files that require an extension (Draco, mesh
  quantization, ...) and sparse accessors are refused; morph-target weights are ignored. A glTF
  animation is sampled at 30 fps by default. Humanoid motion goes onto two-legged sprites only.
- **Motion Studio** has not been opened on a PC yet (built by CI, opened by the UI audit). It
  retargets on the UI thread: about 50 ms for a 250 px sprite, so very large sprites will lag
  while dragging the camera. A camera far from the drawn view only changes the motion, not the art.
- **In-betweens** (Animation › Generate In-betweens…): the core is tested; the dialog has not been
  opened on a PC yet (built by CI, opened by the UI audit). In-betweens cannot show what neither
  key shows, so big pose changes tear; the area check flags many such frames but not all (a limb
  can move without changing the area). Touching parts of the same colour can swap pixels; keep
  separate parts on separate layers. The work holds a second copy of the document while it runs.
- **Motion onto a saved rig** (`rig motion`, ADR-ANIM-002 section 3.3): tested locally and in CI (core
  and CLI smoke test); Lua (`doc:rig_motion`, tested locally) and MCP (`rig_motion`, which runs the CLI; compiled,
  not called by a test) offer it too; there is no button for it in the app yet. Bones
  turn to point the way the projected skeleton points, so a limb seen end-on (a front-view walk's
  legs) barely moves and holds its last direction. Side-view legs numbered 0 and 1 are taken as
  near and far; if they come out swapped, pair them with `--bones`. Wings, fins and other
  non-humanoid rigs match nothing and are refused. Frames added for the keys are empty until baked.
- **Motion Studio panel** (ADR-ANIM-002): compiled and opened by the CI UI audit only; it has not
  been used on a PC since it became a dock. The panel does not follow the open document by
  itself: press Use Current Sprite after switching sprites or frames. Its controls no longer have
  Alt-key mnemonics (they would clash with the menu bar when docked), like the other panels.
- **Parts that bend across a joint** (`rig skin`, ADR-ANIM-004): a part follows at most two bones
  (its own and one child); there is no painted weight map, the split is a straight line through
  the joint halfway between the two bones' directions. The bend is a linear blend, so a very sharp
  bend (past about 120 degrees) pinches the inside of the joint. Command line, the MCP
  `rig_edit` tool, Lua (`doc:rig()` / `set_rig` carry it in the rig's `skins`) and the rig panel's
  Bends with box (compiled and audited by CI; no Qt test drives it). Auto-rig does
  not make bent parts; it still cuts a separate part for each bone.
- **Four-legged motion** (`retarget --body quadruped`, ADR-ANIM-002 section 3.5): tested on a
  drawn test dog and the `quad-walk` preset only, not on captured animal motion. The legs, the body's
  height, the back's pitch and the head's and tail's rise and fall follow the motion (since
  2026-11-18); sideways swings of the head and tail, and a head turning to look, are not shown
  (a side view cannot show them without art drawn for it), and the head and tail bend only up and
  down by moving columns, so a long neck or tail held at a steep angle shears rather than turns. Exactly four legs reaching the floor are needed (six-legged and winged skeletons are
  refused); legs cannot be paired by hand yet, and Motion Studio's bone table lists humanoid bones
  only. Sprites must be drawn from the side with their legs apart. `rig motion` takes four-legged
  motion on an auto-rigged four-legged sprite (since 2026-11-18; tested on the test dog): the legs,
  the back's pitch and the body's height become keys. Auto-rig cuts the head and the tail of a
  four-legged side view into parts of their own (since 2026-11-18), and the motion nods the head
  and raises and lowers the tail; a short end is a rigid part turned at one joint, and the joint
  can show a notch when it turns far. A long end (at least 8 px and 2.5 times its mean thickness)
  gets a second bone from its middle on ("neck" then "head", "tail" then "tail tip") and bends
  across that joint (since 2026-11-18): the tail's tip repeats the tail's swing 0.15 of the cycle
  later, the head turns half-way back against the neck. That is one bend, not a curve through
  several joints, and it is tested on a drawn test animal only (rectangles; no generated sprite).
  A long neck with a big head can fail the thickness test and stay rigid. Feet must be at least a twelfth
  of the sprite's smaller side (since 2026-11-20; before, of its width, so a long tail made thin
  legs vanish); legs thinner than that are still not found. The cut follows the
  body's outline (where the belly line ends, and what rises above the back next to it), tested on
  the test dog only; an end smaller than 2% of the body stays on the body. Feet keep their drawn
  angle to the ground. Legs are paired by position (behind or ahead of the middle of the legs, then the first
  numbered as the near one); `--legs "0=hind-near,..."` pairs them by hand, and `--bones` pairs
  are refused for four-legged motion. The facing is read from the head bone auto-rig cut (else
  from the highest body art); `--facing left|right` overrides it. Lua and MCP take `facing` and `legs` too; the app does not.
- **Wings from a 3D motion** (`retarget --body flyer`, ADR-ANIM-002 section 3.7): a humanoid
  motion's arms beat a winged side view's wings (each wing turns about its shoulder as its arm
  rises and falls, the hand end bends with the elbow, the body rises and sinks with the hips).
  Tested on a test dragon with its wings selected and checked by eye on the 128 px golden dragon
  with the built-in `flap` preset only; no captured bird or bat motion has been tried. Wing
  skeletons (a bird's or bat's own wing bones) are not read: the motion must have humanoid arms,
  and a four-legged motion is refused. The wings are found as in Animate's flap (the part above
  the wing line, 45% of the height unless `--wing-line` or Lua's `wing_line` says otherwise); when
  the head reaches above that line it turns with the wing, so lower the line. Motion Studio takes a
  wing line and, when the wings were selected in the document before Use Current Sprite, uses the
  selection as the wings (compiled by CI; no Qt test drives it); the command line, Lua and MCP take
  a wing line only. `--body auto` never picks wings. The wing stroke is up and down only: no
  forward sweep, twist or folding. The tail waves with the stroke as in Animate's flap (a thin tail
  behind the body only; the golden dragon's thick tail is not found as one). Fins (a swimming
  motion onto `RigSwim`) are not mapped.
- **Direction sets** (`directions`, ADR-ANIM-002 section 3.4): CLI and core only (no app page,
  Lua or MCP yet). The art is chosen, never turned or drawn: a direction with no view drawn near it
  shows the nearest view with the motion seen from the new direction, and is flagged. Mirrored
  views swap the character's left and right (a sword hand, a shield); draw that side or use
  `--no-mirror`. Views are read from file names only (no reference-view slots in a project yet).
  The turn clip cuts between drawn views; nothing in between is drawn. Retarget reads each view's
  legs as front or side, so three-quarter art moves like the nearer of the two.
- **Arm boxes for auto-rig** (`rig auto --arms`): CLI and core only (no way to draw the boxes in the
  app yet; not in Lua or MCP). A box takes every pixel inside it, so torso or cape pixels in the box
  move with the arm; keep boxes tight. The arm turns about where its pixels touch the body outside
  the box, which is the shoulder only when the box is cut at the shoulder.
- **The UI audit sometimes crashes at exit (Windows CI only).** After every command has run and
  the report is written, the app occasionally ends in an access violation while it shuts down
  (seen on 2026-11-17 and 2026-11-18 after the "exiting" step). Waiting for the background
  thumbnail, library and MCP threads before exit did not stop it, so the cause is still unknown.
  The audit now installs a crash report (`app/src/selftest/CrashReport.*`) that prints the
  exception, the thread and a symbolised stack into the test output, and MSVC builds write a PDB
  for it. On 2026-11-18 that report named the cause: a document view destroyed during shutdown
  told the AI service (already deleted) that drawing had stopped
  (`AiService::setDrawing`). Those connections now belong to the AI service, so they go with it;
  this would also have hit a user quitting with a document open. Whether this was the only cause
  is shown only by later CI runs.
- **Motion library** (ADR-ANIM-002 stage 3b): the presets, BVH writing and the saved-preset folder
  are tested locally and in CI, and the CLI is covered by the CLI smoke test; the Motion Studio
  Library row is built by CI only and has not been used on a PC. The presets are simple and
  procedural (no capture data), so they look mechanical next to real mocap. Sprites drawn from the
  front never rise off the ground in a retarget (the jump moves the arms and legs but not the
  height); side-view sprites do. Lua has `ace.retarget` and `ace.motion_presets` (tested); the MCP tools `motion_presets`, `motion_retarget` and
  `motion_directions` run the CLI commands and are compiled, not called by a test.
- **MCP tools for rigs, vector shapes and the palette lock** run the CLI commands that the CLI smoke
  test covers, but no test calls the tools themselves yet.
- **Palette lock** (ADR-ANIM-006): the core is tested; painting, saving, the CLI and the Palette
  panel controls are checked by CI only and have not been used on a PC. Merge Down with blend
  modes, smooth transforms and effects are not snapped; use Conform after them. The colour picker
  still lets you choose any colour, which is snapped when painted.
- **Vector shapes** (ADR-ANIM-005): the core and CLI are tested; the tool options are built by CI
  and opened by the UI audit but have not been drawn with on a PC. A shape cannot be selected or
  edited after it is drawn (undo, `vector clear` or rasterise instead), the Curve tool makes one
  Bezier segment per shape (no multi-anchor pen yet), and painting by hand on a cel with shapes is
  lost at the next shape added unless the cel is rasterised first. Linked cels cannot hold shapes.
- **Rigs** (Rig panel, Animation › Auto-Rig Current Layer, `acestudio-cli rig`): the core and CLI
  are tested; the panel and the bone overlay are built by CI and opened by the UI audit but have not
  been used on a PC, and bone dragging on the canvas has not been tried by hand. While dragging is
  on, a paint stroke that starts within 7 screen pixels of a joint or tip grabs the bone instead
  (turn **Drag bones on the canvas** off to paint there). Auto-rig handles bodies with legs (two or four), wings (cut below the wing line, so legs stay in
  the body) and fish (the body stays one rigid part); blobs and objects are refused and must be
  rigged by hand, and arms are only rigged where the silhouette analysis finds them. Every part is rigid on one bone (no bending across a joint). Pose keys with curves are saved
  in the rig, but an unkeyed working pose lives in the panel until it is keyed or baked, and moving
  to another frame of a keyed rig replaces it with that frame's keyed pose. There is no curve editor
  (custom Bezier points only through `rig key --curve bezier:…`), and keys are not drawn on the
  Timeline. Baking keys runs on the UI thread. Auto-rig runs on the UI thread
  (fast for sprites; a very large layer pauses the window briefly). Projects with a rig need
  schema 4 (this version or later).
- **ONNX Runtime DirectML:** onnxruntime-directml 1.24.4 fails inside a fused node when running
  BiRefNet on the test PC. AceStudio falls back to fewer fusions, then to the CPU (~16 s per
  image). NVIDIA GPUs with driver R580+ use CUDA instead.
- **BiRefNet on the CPU needs about 6.5 GB of free RAM.** The ONNX export has a fixed 1024×1024
  input; a smaller input fails inside the graph. AceStudio checks free memory, including
  container limits, before a CPU run. If too little is free, the job fails within seconds and
  says how much is needed, instead of the worker being killed. The 12 GB figure for BiRefNet
  General is an estimate.
- **Python:** the installer bundles Python 3.12, so nothing needs installing. Without it (a
  build from source, or a deleted `python` folder) AceStudio looks for a 64-bit Python
  3.10–3.14; ONNX Runtime publishes no Windows wheels for 3.15 yet, and none for 32-bit. An
  AI environment already made from another supported Python is kept, not rebuilt.
- **Small canvases and SDXL:** generation runs near 1024² and is reduced with nearest neighbour.
  At 64×64 that is a 16× reduction, twice Pixel Art XL's ~8× pixel size, so results are coarse;
  128×128 fits best. "Frame the subject" helps when the background is plain.
- **FP8 weight storage (Precision: Auto)** is used for pose-controlled generation on GPUs under
  10 GB, so the UNet and ControlNet fit in VRAM.
  - Speed, measured on the 8 GB test PC at 10 steps: FP8 ~20 s per frame, Full 505 s, because
    full precision spills into shared memory.
  - Quality: with the same seed and pose the two give different images (PSNR 13 dB). Both are
    clean knights of similar quality, so FP8 changes which image a seed gives without
    visibly degrading it.
  - Seeds do not carry over between precision modes.
- **SDXL ignores some prompt words.** It sometimes adds a floor, a floor shadow or scenery even
  when asked for a plain background. AceStudio now asks for "isolated on a plain white
  background" and adds floor and scenery terms to the negative prompt. A floor drawn beneath
  an airborne character is removed before the checks. A floor touching the feet stays and
  counts as part of the figure.
- **Real-ESRGAN** smears small pixel art; that is how the model works. Use Image › Scale for
  pixel art.
- **GIF export:**
  - At most 256 colours per frame (median-cut reduction beyond 255).
  - 1-bit transparency: alpha below 128 becomes transparent.
  - Durations in 10 ms steps, minimum 20 ms; browsers treat shorter delays as 100 ms.

  Use APNG or WebP for exact output.

## Design choices (documented behaviour, not defects)
- **Compositing runs on the CPU.** Large areas are split across threads with identical output.
  On the 2-vCPU test VM, a full 1024×1024 × 20-layer recomposite with mixed blend modes takes
  34–44 ms in 0.9.0 (58–68 ms before the blend loops were specialised per mode). Strokes
  recomposite only their dirty region (0.07 ms for 32×32). A GPU compositor would have to
  reproduce every blend mode, mask, clip and adjustment exactly; these numbers do not justify
  that cost. `bench/perf_budgets.json` fails CI if they grow several-fold.
- **Live generation preview** needs the optional 10 MB TAESD-XL model (MIT; Models dialog).
  Without it, generation shows progress only. Generate Animation shows the preview on the
  frame being generated.
- **What the animation checks can and cannot see:**
  - Each frame is checked against its own pose: ground line and height (so a jump may leave
    the ground), facing, shape continuity with its neighbours (relaxed by as much as the poses
    themselves differ), and colour profile.
  - The colour profile is compared with frame 1 and with the character's reference images.
    It uses 12 hues × 3 lightness levels and 4 greys. On real frames of one character it
    measured ≤ 0.31, and ≥ 0.41 for other subjects and recoloured copies; the thresholds are
    0.36 and 0.38.
  - It is a colour check, not face or identity recognition: a different knight in the same
    colours passes. Reference images (IP-Adapter) are what keep the identity; the check
    catches drift.
  - Between very different poses (a crouch and the top of a jump overlap 12%) the shape check
    allows almost any shape. There, only the colour and identity checks protect against a
    wrong drawing.
- **Cancelling generation:**
  - Between steps (FP8, ~1 s each) a cancel takes effect at the next step.
  - Loading SDXL and importing PyTorch cannot be interrupted. A cancel then waits for the load
    to finish (up to 3 minutes), so the model stays loaded. Measured: it cancelled about 40 s
    later and the next job skipped the load.
  - Full-precision steps on an 8 GB GPU take 25–45 s and are waited for the same way.
- **Identical poses share one job.** A jump's rise and fall use the same poses, so a 6-frame
  jump runs 4 SDXL jobs; the repeated frames are pixel-identical.
- **One AceStudio per data folder.** Opening a file while AceStudio runs sends it to the open
  window. `--new-instance` starts another window anyway; two windows then share settings, and
  the last one closed wins.
- **Committing an animation** fills a blank document from frame 1. Into a document with
  content, it appends the frames after the last one with their own tag. To place frames
  elsewhere, move the block in the timeline afterwards.
- **AI job folders** (inputs, raw 1024² outputs, previews) are kept for 7 days, then removed at
  start-up.
- **Free Transform** scales about the box centre. Shift keeps proportions and dragging past the
  centre flips. The preview uses fast nearest-neighbour rotation; the applied result uses
  RotSprite.
- **Linked cels** link pixel cels only; layer masks stay per frame. Duplicating frames creates
  independent copies, and duplicating a layer keeps its links.
- **Frame selections with gaps** can be deleted, re-timed and linked. Moving, dragging and
  duplicating need a continuous block, and the timeline says so.
- **Tags may overlap.** Playback, export defaults and the timeline tint use the first tag that
  contains the frame.
- **Layer transforms skip frames where the layer has no pixels.** Transforming an empty layer
  reports "The layer is empty".
- Onion-skin ghosts are cached. The first display after a structural change composites each
  ghost frame once (about 100 ms for one ghost each side at 1024² × 20 layers on the VM).
- **Saving large projects** PNG-compresses every cel, about 10 ms per 1024² cel per core
  (miniz deflates the whole cel even when it is mostly empty). Cels are compressed on all
  cores: 160 such cels take 1.3 s to save and 0.5 s to load on the 2-vCPU VM. Autosave writes
  to recovery files in the background.

## Phase 6 asset tools (design limits)
- Up to three terrains over a base, and textures of up to 4×4 tiles. Engines without per-cell
  variants (most autotile rule systems) use phase variant 0 or need the documented
  `x mod phases, y mod phases` rule. With Margins off in the sandbox, a level painted straight
  onto a much lower one shows its edge with no level in between (Margins is on by default).
- AI background removal (slicer, parallax) is BiRefNet followed by `refineCutout`, which gives
  hard pixel-art edges, brings back dropped parts that differ from the local background and
  opens background gaps the mask filled in. A sprite part with the same colour as the
  background next to it cannot be told apart from it.
- Clear Sky "Sky against ground" needs ground at the bottom of the layer. A scene whose bottom
  is mist that fades into the sky (one PC castle layer) keeps its sky; the dialog then says how
  little it cleared and suggests the AI cut-out, which handled that layer.
- Texture results depend on the prompt: SDXL with Pixel Art XL drew "lush green grass" as bushes
  on pale stone in three PC runs, while "dense short green grass lawn, solid green" gave a plain
  grass texture, so that is now the default.
- The atlas packer rotates only by 90° clockwise (TexturePacker's convention), never flips.

## Phase 7 integrations (design limits)

### MCP server
- No SSE stream resumption (`Last-Event-ID`). It is optional in 2025-11-25 and removed in
  2026-07-28. A dropped HTTP stream cancels its tool call (tasks keep running; poll them).
- A task cannot continue in another process: if the server that runs it is killed, the task
  reads as failed (its result is kept when it finished first).
- Each tool call starts `acestudio-cli`, so the CLI and the server never disagree. That costs
  a median of 2.8 ms per `info` call on the Linux build machine; Windows was not timed
  separately.
- Scripts from `app_run_lua` run on a copy, and their result is not applied if the user edits
  the document meanwhile; the tool then says so.
- Client setups: Claude Code 2.1.316 was run and connects to the server; Codex 0.166.0 reads
  the setup, but a Codex session (which needs a sign-in) was not started. Cursor and VS Code
  were not run; their setups follow their documentation (October 2026).

### Lua
- No network, processes or native modules. Files and `require`d modules only through the
  `ace` API and inside the allowed folders.
- In the app, Run Script works on a copy of the document; its result is dropped if you edit
  the document while it runs (run it again to apply it).

### Engine export
- Unity: the tiles use phase variant 0 only, and Tile Palettes are not generated. The optional
  Rule Tiles need 2D Tilemap Extras and cover Blob47 and Cardinal16 only; Wang16 uses
  AceStudio's dual grid. AceStudio's tile scripts use only Unity's built-in Tilemap module;
  they were checked in a project that also had Tilemap Extras installed.
- Tiled: sprite sheets get a `.tsx` with tile animations only when untrimmed and on a grid
  (horizontal, vertical or grid layout). The `.tsx` files were checked by tests, not loaded in
  Tiled itself.
- Godot keeps an isolated single cell empty in corner (Wang) mode. That is Godot's own
  behaviour, reproduced in `tests/engine`.

## Phase 8 commercial polish (limits)

### Installer and distribution
- The installer and executables are **not code-signed**. Seen on the test PC: SmartScreen
  blocks the installer ("Windows protected your PC") until the user picks More info › Run
  anyway. A code-signing certificate is required before release.
- The interactive install (wizard pages) was exercised on the test PC only up to the EULA
  page (removed in 0.10.0); the full install, AI setup and uninstall were tested silently in CI.
- Per-user install only by default; an all-users install is possible from the command line
  (`/ALLUSERS`) but was not tested.
- Nightly installers are attached to a GitHub release, public while the repository is.
- PRIVACY.md is a draft for legal review.

### Accessibility
- The UI audit checks that every control has an accessible name and can be reached with Tab,
  on every window the menus open. It does not prove the result is pleasant with a screen
  reader: NVDA and Narrator were not run.
- The canvas itself is a picture; drawing needs a pointer. Everything else (menus, panels,
  dialogs, timeline, layers) works from the keyboard.

### Regression suite
- Projects from 0.1.0 render differently where groups have content: 0.2.0 changed group
  compositing on purpose (isolated groups). The compat test allows exactly that.
- Performance budgets are ceilings several times the measured times, chosen so slow CI
  runners do not fail; they catch large regressions, not small ones.

## 0.10.0 premium studio: unresolved

These are the issues still open after the 0.10.0 work. Each was found by running the feature,
or is a case that has not been run yet.

- **FLUX.2 [klein] on the test PC** (RTX 4070 Laptop, 8 GB):
  - Speed: 3.6–3.8 s per step at 1024² and 2.1–2.4 s at 512², with a peak of 4.4 GB.
  - Verified: HD generation, the Character Pack's front and back views, and style matching
    (goblin 87 %, sword 71 %).
  - The pixel-art LoRA gave pure noise at adapter weight 1.0, because it is an rsLoRA with
    alpha 128. It now runs at 0.05; that fix has not been re-run on the PC.
  - The 7×7 bestiary works on the PC: 49 whole creatures in 192 s, made as six 3×3 sheets.
    Several brutes still look alike.
  - Not yet measured on the PC:
    - the grid-versus-staged benchmark;
    - Pet to Pixel.
  - At Medium, a style's reference image leaks its subject: a goblin took the knight's
    armour and red cape.
- **Animate Sprite moves parts rigidly.** It cannot draw new poses (a turn, a weapon drawn in
  another position). The router points those actions (cast, attack 2, climb) to the AI pipeline.
  - Walk and run now keep the standing foot on the ground (the leg stretches a pixel or two) and
    swing the arms found beside the body. Measured on a 32×48 test knight: before, the sprite
    was read as an object and rocked, with the feet 1 px off the ground in 6 of 8 frames; after,
    a biped with both feet planted in every frame. Not yet seen on generated sprites on the PC.
  - Arms are found only beside the legs (left of the left leg, right of the right one). An arm
    drawn in front of the body moves with the body. A wide coat or bulky armour wider than the
    legs can be taken for arms; the swing is small (0.6 × the leg swing).
  - Front views whose legs touch are split at the middle and step in turn instead of swinging.
  - A three-quarter stance (the far foot a row or two higher) is a biped; each foot keeps its
    own ground line. First seen on a generated 64 px rogue (benchmark run 1), which had
    rocked as an object. Under a long cloak a few hem pixels move with the legs.
  - **Part rig (ADR-ANIM-001):** flap, fly, swim, dash, walk, run and
    sprint now cut the sprite into parts that move and turn on their own. Implemented and tested
    with a local build on synthetic sprites and four golden sprites (dragon, fish, rogue, knight).
    Not yet seen in the app on the PC.
    - Wings: each wing turns about its shoulder. A far wing (found when the two wings are apart or
      can be split) is drawn behind the body and trails the near one. When no wing is found, the
      older flap is used. The wing finder is a heuristic; on a sprite whose wing and head share
      colours, select the wings first.
    - Fish: the body bends without losing length, the tail fin beats and foreshortens, and the
      fins paddle. There is still no turn clip (it needs other views).
    - Legs: each leg has a thigh, shin and foot, with the knee placed by IK. The planted foot
      stays on its row and moves back at one speed. Side-on bipeds twist at the shoulders and pelvis by a pixel or
      so (front views and four-legged bodies do not), and idle uses the older rig.
    - Nothing of the rig is saved in the project (no schema change), and there is no part painter yet.
    - Older note, from before the part rig: on a generated HD dragon (test PC), the old default
      span folded the head and neck in with the wings.
  - Selecting the wings first (the selection becomes the wing mask) gives exact control.
  - Sprites larger than 320 px are previewed from a reduced copy; the checks shown are the
    preview's. Opening or exporting renders at full size, which takes a few seconds for an
    HD sprite with many actions.
  - Generating flap keyframes with AI is not built. There is no 2D bone editor or IK for
    user-placed bones; the rig is measured from the silhouette.
- **Procedural effects:** fire was reviewed on two generated campfires on the PC. Water, sway,
  flag, smoke and pulse were reviewed on synthetic sprites only.
- **The animation router** decides fire from pixels only with a bright core taller than wide;
  other effects need a hint in the name (an imported file's name counts). Without a name, a
  tree or a flag gets body motion until the method is changed.
- **Performance work (0.10.0)** was measured on the test PC only (RTX 4070 Laptop, 8 GB, 32 GB
  RAM, Balanced): see `docs/devlog/2026-11.md`. Not measured: Eco and Performance profiles,
  SDXL, HD sizes, other GPUs. System RAM was full during the first model load (klein with CPU
  offload); machines with 16 GB have not been tried.
- **GameMaker export** follows GameMaker's `_stripN` import convention. It has not been
  imported into GameMaker, which is not available here.
- **Phaser export** was checked in Phaser 3.90.0 and 4.2.1 (headless Chromium) for loading,
  animation frames, durations and looping. It was not checked in a shipped Phaser game.
- **World Builder.**
  - Maps are exported to Tiled `.tmx`; there is no direct Godot TileMapLayer or Unity
    Tilemap scene export yet. The `.tmx` was checked structurally (unit tests, Python TOML
    and XML parsing; a PC-made level re-rendered from the TMX alone matches the preview) and has
    not been opened in Tiled itself.
  - The map canvas re-renders the whole map on every edit. That is fine for typical levels
    (a few thousand cells) and slow for maps near the 4096×4096 limit.
  - The Terrain tool paints blob-47 and cardinal-16 sheets from the Tileset Generator (it
    reads the sheet's `.json`). Wang-16 (dual-grid) sheets are not supported in the map yet.
- **Claude Code over MCP (Test F)** has not run on the test PC: this session cannot type into
  a terminal there. The MCP tools are covered by CI.
- **Uninstall (Test G)** ran on the test PC twice: projects, settings and models were kept, and the
  reinstall worked. With AceStudio open, the uninstaller asks to close it (seen on nightly-3f6e18d).
- **`connect claude --global`** prints the `claude mcp add` command instead of editing
  `~/.claude.json`, because Claude Code rewrites that file itself while running.
- **The VS Code and Claude Desktop user-wide paths** are the documented defaults (`%APPDATA%`).
  Portable or Insiders installs keep their settings elsewhere.
- **Acceptance tests A–G:** see `docs/devlog/2026-11.md` for what was run and with what
  result. Anything needing klein on the GPU is still open.

## 2026-11-08 agent work: implemented, CI-verified, not yet run on the test PC
Everything below is merged into main and passes CI (Linux GCC and Windows MSVC builds plus tests).
None of it has been run in the installed app on the test PC or with the real AI models.
- **Security** (SECURITY.md findings 8–11):
  - MCP HTTP always uses a token.
  - The app bridge checks that the process at the other end belongs to the same user.
  - AI job paths are confined.
  - torch and PyPI packages are installed in separate runs.
  - The new pip install flow has not run against the real PyTorch index.
  - A relative `ai.modelDirectory` setting is now refused.
- **AI worker speed and memory** (`bench/ai_worker_bench.md`): the expected gains are estimates until that benchmark is run on the GPU. The changes are:
  - the ONNX Runtime CUDA memory is capped;
  - FLUX.2 klein loads in stages, so RAM peaks lower;
  - models are unloaded after an idle time;
  - a job that runs out of GPU memory is retried once;
  - Real-ESRGAN is cached;
  - preview images are written in the background.
- **Animation:** the re-audit (`docs/agents/AUDIT_2026-11-08-animation.md`) still rates dragon,
  fish, fire and walk as **failing** the standard. The following fixes landed after it:
  - fire flames are no longer clipped at the top, split into tongues and flicker irregularly;
  - the dragon flap keeps the head visible;
  - front-view walk is detected even with a shield, and held items are no longer sheared;
  - the app's checks now report ping-pong clips and a head covered by a part, and flaps no longer have the 70 % allowance.
  
  These fixes were checked by tests and Python ports only, not re-rendered on the PC. The part rig
  (ADR-ANIM-001, see above) now adds separate fins and wing depth. Fish turning still needs views.
- **UI:** dock tab labels now follow the text size back down. The `--ui-audit` screenshots wait
  for each page's fade-in. Screen readers are still not tested.
- **Remote GPU:** design only (`docs/agents/CLOUD_GPU_DESIGN.md`). Nothing has been built or provisioned.

## Planned in later phases
- Plugin API (Phase 8 or later). Profiling of BiRefNet on CUDA: part of its 2.6 s is CPU
  pre/post-processing. FP16 weights, I/O binding and GPU-side resizing are to be measured in
  the Phase 8 profiling pass.
