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

Jobs show progress in the AI panel and can be cancelled. The AI runs in a separate process:
if it crashes, the editor and your documents are unaffected; **Restart AI Worker** starts it
again.

## 8. Assets

The **Assets** menu:

- **Tileset Generator** — seamless autotile sets (Blob 47, Wang 16, Cardinal 16) from a base
  and up to three terrain textures, with a sandbox to paint test levels.
  **Check Tileset Seams** finds edges that don't match.
- **Parallax Background**, **UI Kit Generator** (9-slice buttons and panels with exact
  borders), **Smart Slicer** (cut a sheet into sprites) and **Pack Sprite Atlas**.

Each can also write Godot, Unity and Tiled files.

## 9. Scripts and AI assistants

- **Scripts › Run Script…** runs a Lua 5.4 script in a sandbox (no network, files only where
  you allow). A script's changes are one undo step. API: [lua-api.md](lua-api.md).
- **Scripts › Set Up AI Assistants (MCP)…** connects Claude Code, Cursor, VS Code, Codex or
  another MCP client so it can create, edit, render and export projects for you. **Allow AI
  Assistants to Use the Open Document** controls whether they may change the document you have
  open; each change they make is one undo step.

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
