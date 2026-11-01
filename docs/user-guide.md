# AceStudio User Guide

Version 0.9.0. AceStudio is a pixel-art and game-asset studio for Windows with local AI
tools. This guide covers everyday use; the README has the full feature list and the CLI.

## 1. Install, trial and licence

**Install.** Run `AceStudio-<version>-setup-x64.exe`. It installs for your Windows user only (no
administrator rights) into `%LOCALAPPDATA%\Programs\AceStudio` by default, adds Start menu entries and
opens `.acestudio` files with AceStudio. It includes a private Python for the AI features, so
you do not need to install Python.

**Trial.** Everything works for 14 days. After that you can still open, view and inspect your
projects, and autosave and crash recovery keep working, but saving and exporting need a
licence. The status bar shows the days left.

**Licence.** **Help › Licence…** (or the status-bar button) → paste your key → **Activate**.
Keys are checked offline; AceStudio never contacts a licence server.

**Uninstall.** Windows Settings › Apps › AceStudio › Uninstall. Your projects, settings and
downloaded AI models are kept; **Help › Privacy** lists where they are if you want to remove
them too.

## 2. The window

- **Canvas** in the middle: pixel-exact, integer zoom (mouse wheel), pan with Space, the
  middle button, or Shift + wheel. **View › Pixel Grid** shows the grid when zoomed in.
- **Tools** (left), **Layers**, **Properties**, **Palette** (right), **Timeline** and
  **Preview** (bottom), **Log**. Drag panels to rearrange; **Window › Reset Layout** restores
  them and **Window › Workspaces** saves named layouts.
- Several documents open as tabs. **File › Open Recent** lists recent projects.

## 3. Drawing

| Tool | Key | Notes |
|---|---|---|
| Pencil | B | Pixel-perfect lines; dither density in Properties |
| Eraser | E | |
| Line, Rectangle, Ellipse | L, U, O | Shift for straight lines, squares, circles |
| Paint bucket | G | Contiguous or global, tolerance |
| Gradient | D | Dithered or smooth |
| Eyedropper | I | Hold Alt with any tool for a temporary eyedropper |
| Selections | M, Q, W | Rectangle, lasso, magic wand; Shift adds, Alt subtracts |
| Hand | H / Space | |

`[` and `]` change the brush size; X swaps colours. **Edit › Brush Tips** imports custom
brush shapes. Mirror painting and alpha lock are in Properties. Every action can be undone
(Ctrl+Z / Ctrl+Y), and **Edit › Keyboard Shortcuts…** rebinds any command.

**Colour modes** (**Image › Color Mode**): RGBA, Indexed (drawing snaps to the palette) and
Grayscale. The Palette panel edits colours and loads or saves `.gpl`, `.pal`, `.hex` and
`.txt` palettes.

**Seamless tiles:** **View › Tiled Mode** wraps painting around the canvas edges and shows the
neighbouring repeats.

## 4. Layers

The **Layer** menu and the Layers panel add, group, duplicate, merge (Ctrl+E), reorder and
delete layers, and set opacity, blend mode (Normal, Multiply, Screen, Overlay, Darken, Lighten,
Add, Subtract, Difference), visibility and locks.

- **Layer › Layer Mask:** paint white to show and black to hide, without erasing pixels.
- **Clipping:** a clipped layer only shows where the layer below it has pixels.
- **Layer › New Adjustment Layer:** Brightness/Contrast, Hue/Saturation, Invert, Posterize,
  Threshold, Levels and Curves change everything beneath them without touching the pixels.

## 5. Animation

- **Frames:** Alt+N new, Alt+D duplicate, Alt+←/→ move, `,` and `.` step, Enter plays. Each
  frame has its own duration (double-click it in the Timeline).
- **Tags** (Ctrl+Alt+T) name a range of frames (walk, idle…) with a direction (forward,
  reverse, ping-pong) and a repeat count. Playback and export honour them.
- **Linked cels** share one image across frames (marked ∞ in the Timeline): change it once,
  every linked frame follows.
- **Onion skin** (F3) shows earlier frames in red and later frames in blue; **View › Onion
  Skin Settings…** sets how many and how strong.

## 6. Export

**File › Export Animation** has Sprite Sheet, Animated GIF, APNG, WebP and PNG Sequence;
**File › Export Frame as PNG** saves the current frame:

- Sprite sheets: horizontal, vertical, grid or packed; padding, spacing, trim, merge
  duplicates, power-of-two size, integer scale; Aseprite-compatible JSON (hash or array) that
  Phaser and most engines read.
- Pick a tag or a frame range; scale up with nearest-neighbour so pixels stay sharp.
- **Engines:** tick Godot, Unity or Tiled to also write `.tres`, `.meta`/`.anim` or `.tsx`
  files that import without further setup.
- AceStudio asks before replacing any existing file.

## 7. AI Studio (local)

Everything runs on your PC; nothing is uploaded.

1. **AI Studio › Set Up AI Runtime…** installs the AI packages into a private environment
   (about 3 GB for an NVIDIA GPU, less for CPU only). It asks before downloading.
2. **AI Studio › Models…** lists the models with their size and licence. A model downloads
   only after you accept its licence, and is verified with SHA-256.
3. Use the tools:
   - **Generate Image…** — SDXL text-to-image or image-to-image, with a pixel-art mode that
     matches the canvas size and the document palette. Needs an NVIDIA GPU with 8 GB+ video
     memory (12 GB recommended).
   - **Remove Background…** (BiRefNet) and **AI Upscale…** (Real-ESRGAN ×4) also run on the CPU.
   - **Characters…** keeps a character's description, palette and reference images.
     **Generate Animation…** draws one frame per pose of an idle, walk, run or jump cycle,
     checks them for consistency, redoes the outliers and adds them as one undoable step.

### The AI Studio workspace

The **AI Studio** workspace (Alt+2) is one form for every kind of asset.

1. **Describe** what you want.
2. **Configure** it: the type (character, monster, weapon, tileset, background, …), the art
   style, the perspective, the output size, the palette and whether to remove the background.
3. **References** (up to four, FLUX.2 [klein] only). Choose what each image is for:
   - *Character* keeps that character.
   - *Style* copies the drawing style but not the subject.
   - *Palette* is not sent to the model; its colours are used for the final pixels.
   - The other roles work the same way for their aspect.
4. **Generate.** Every result is checked for cut-off parts, stray pixels, soft edges, an
   opaque background, the size and the colours.

From a result, you can:

- **Open as Document**, **Add as Layer**, **Animate…**, **More Like This** or **Use as
  Reference**.
- **Add to Style:** adds the result to the current game style.
- **Character Pack…:** generates the front, back and diagonal views of the character with it
  as the reference, and animates every view (2, 4 or 8 directions).

**Game style.** **New Style from References…** measures your reference images: palette,
outlines, shading, light direction, colour and detail. **Lock Game Style** then applies that
style to every generation:

- *Light* adds the style's words.
- *Medium* also sends its reference images.
- *Strong* also uses its exact palette.

Each result shows how closely it matches the style and is flagged when it drifts.

**Batches:**

- **Bestiary Sheet…** makes a 3×3 to 7×7 (or custom) grid of different creatures. You can
  generate it as one sheet that is cut into sprites, or as one image per creature. Both report
  the time per creature, so you can compare them on your GPU. Optionally, each creature gets
  an idle animation.
- **Asset Pack…** makes a themed set (dungeon, forest, sci-fi, town), one item in five tiers,
  or your own list. With a game style locked, the pack matches it.
- **Pet to Pixel…** turns a photo of your pet into a sprite.

FLUX.2 [klein] 4B (Apache-2.0, about 16 GB) is the default model when it is installed. It
runs on an 8 GB NVIDIA GPU by storing weights in 8 bits and moving parts to system memory.
SDXL with Pixel Art XL is used otherwise; it cannot use reference images.

Jobs show progress in the AI panel and can be cancelled. The AI runs in a separate process:
if it crashes, the editor and your documents are unaffected; **Restart AI Worker** starts it
again.

### Animate a sprite (no AI)

**Animation › Animate Sprite…** (or **Animate…** on an AI result) turns one still sprite with a
transparent background into animations. AceStudio measures the sprite's silhouette:

- Two separate feet walk.
- Four feet trot in diagonal pairs.
- A round shape without feet hops.
- Anything else rocks.

If the guess is wrong, choose the **Body** yourself.

Tick the actions you want: idle, walk, run, sprint, jump, fall, land, three attacks, dodge,
roll, dash, block, hurt, death, cast, swim, fly, wing flap, climb, sit, sleep, or custom
(bob, sway, squash, tilt and leg swing). The preview plays the highlighted action.

**Open as Document** puts every action in one document, with a tag for each.
**Export Strips…** writes one PNG strip per action.

Frames use only the sprite's own colours (the hurt flash is the exception). Every clip is
checked for parts that vanish, jumps between frames and a loop that does not close. For
**Wing flap**, set the **Wing line** just below the wings' root: everything above it folds.

## 8. Assets

The **Assets** menu:

- **Tileset Generator** — seamless autotile sets (Blob 47, Wang 16, Cardinal 16) from a base
  and up to three terrain textures, with a sandbox to paint test levels.
  **Check Tileset Seams** finds edges that don't match.
- **Parallax Background**, **UI Kit Generator** (9-slice buttons and panels with exact
  borders), **Smart Slicer** (cut a sheet into sprites) and **Pack Sprite Atlas**.

Each can also write Godot, Unity and Tiled files. Sprite sheets can also be written for
**Phaser** (with frame names that `anims.createFromAseprite` reads, plus a small loader module)
and **GameMaker** (one `_stripN.png` per animation, which GameMaker splits on import).

### World Builder

The **World** workspace (Alt+5) opens on a map canvas. **Add** brings in:

- tile sheets, from a PNG or the open document;
- props;
- parallax backgrounds.

The tools:

- **Paint**, **Erase**, **Fill** and **Rect** work on the selected layer.
- **Prop** places the selected prop; drag a prop to move it, and right-click it to remove it.
- **Solid** marks collision cells.
- **Light** adds a lamp for the lighting preview.

Layers can be hidden and reordered. **Show collision**, **Lighting preview** and
**Camera / parallax preview** (with a camera slider) show the level as a game would.
Ctrl+Z undoes.

**Save** writes a `.acemap` file, with its images in a `.assets` folder beside it.
**Export to Tiled…** writes a `.tmx` map with:

- its tile sheets and props;
- the parallax layers;
- a collision object layer (solid cells merged into rectangles);
- a preview PNG.

Godot (with a Tiled importer), Unity (SuperTiled2Unity), GameMaker and Phaser import Tiled maps.
The **Tilesets and Tools** tab has the tileset generator and the other asset tools.

## 9. Scripts and AI assistants

- **Scripts › Run Script…** runs a Lua 5.4 script in a sandbox (no network, files only where
  you allow). A script's changes are one undo step. API: [lua-api.md](lua-api.md).
- **Scripts › Set Up AI Assistants (MCP)…** connects Claude Code, Cursor, VS Code, Codex or
  another MCP client so it can create, edit, render and export projects for you. **Allow AI
  Assistants to Use the Open Document** controls whether they may change the document you have
  open; each change they make is one undo step. **Add for Me** adds AceStudio to the assistant's
  own settings file for you. Your other settings are kept, and the old file is saved as
  `.bak`.
- From a terminal: `acestudio-cli connect claude|claude-desktop|codex|cursor|opencode|vscode`.
  Add `--project <folder>` for one project or `--global` for all, `--remove` to take it out,
  and `--dry-run` to see the change first. For Claude Code's user-wide setting, it prints the
  `claude mcp add` command to run, because Claude Code manages that file itself.

## 10. Accessibility

- **View › Theme › High Contrast**; AceStudio follows Windows high-contrast mode automatically.
- **View › Text Size** enlarges all text (100–200%).
- Every command is in a menu with a rebindable shortcut, and every control can be reached
  with Tab and is named for screen readers.

## 11. Your work is safe

- Saves are atomic, and the previous version is kept as `<name>.acestudio.bak`.
- Open documents are autosaved to recovery files. After a crash, AceStudio offers to restore
  them on the next start.
- A damaged or newer-version project is reported, never silently changed.

## 12. Troubleshooting

| Problem | What to do |
|---|---|
| AI setup fails | Check the AI panel's message; **Help › Open Logs Folder** has the details. A proxy or firewall must allow pypi.org and download.pytorch.org. |
| "Not enough video memory" | Close other GPU programs, lower the image size, or use the CPU-only tools. |
| A model download is refused (401/403) | The model is gated: accept its terms on its website, then enter your access token in **Models…**. |
| Saving is disabled | The trial has ended: **Help › Licence…**. Your work is not lost; recovery files are kept. |
| The window layout is broken | **Window › Reset Layout**. |

Logs: **Help › Open Logs Folder**. Include `acestudio.log` when you report a problem.
