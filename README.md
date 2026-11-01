# AceStudio

AI-assisted game asset creation and 2D animation studio for Windows.

> **Status: Phase 6 (environment and asset tools), version 0.7.0.** You can draw and animate
> pixel art with layers, frames, tags and onion skin, export sprite sheets (Aseprite-compatible
> JSON), GIF, APNG, WebP and PNG sequences, undo anything, and recover work after a crash. AI
> Studio runs SDXL, BiRefNet and Real-ESRGAN locally in a supervised worker, generates
> character animations from pose skeletons, and makes seamless textures. The Assets menu builds
> autotile sets (blob 47, Wang, 4-neighbour) with seam checks, parallax backgrounds, UI kits
> with 9-slice data, sliced sheets and packed atlases. Engine-specific export, MCP and Lua are
> Phase 7 — see [ROADMAP.md](ROADMAP.md). Nothing in the app pretends to work when it doesn't.

## What works today

- **Drawing:** pencil (pixel-perfect, dither density), eraser, line, rectangle, ellipse, paint
  bucket, gradient (dithered or smooth), eyedropper, hand; brush size/shape; mirror painting;
  alpha lock; indexed and grayscale colour constraints.
- **Selection:** rectangle, lasso, magic wand; add/subtract/intersect; invert.
- **Image:** canvas size, crop, flip/rotate canvas; flip, rotate, RotSprite and scale layers;
  outline; colour-mode conversion.
- **Palettes:** edit, load/save .gpl/.pal/.hex/.txt, extract from image.
- **Shortcuts:** every menu action can be rebound (Edit › Keyboard Shortcuts).
- **Editing:** undo/redo for everything; layers (add, group, rename, reorder, duplicate,
  merge, delete, opacity, blend, locks); palette editing; PNG import/export.
- **Animation:** frames (insert, duplicate, delete, drag or Alt+←/→ to reorder, per-frame and
  all-frame durations); tags with colour, direction (forward/reverse/ping-pong) and repeat
  count; playback that honours tags; onion skin (frames before/after, opacity, red/blue tint,
  wrap); a Preview dock that keeps looping while you draw.
- **Export:** sprite sheets (horizontal, vertical, grid, packed; padding, spacing, trim, merge
  duplicates, power-of-two, scale) with Aseprite JSON (hash or array); animated GIF, APNG and
  WebP (lossless); PNG sequences. Existing files are only replaced after you confirm.
- **AI Studio (local):** SDXL text-to-image and image-to-image with a pixel-art mode (canvas
  size, document palette), BiRefNet background removal, Real-ESRGAN ×4 upscaling, a job queue
  with progress and cancel. Runs in a separate Python process: a model crash never takes the
  editor down. Setup installs pinned packages into a private environment; models download only
  after you accept their licence, and are verified with SHA-256. See
  [docs/ai-worker-protocol.md](docs/ai-worker-protocol.md).
- **Characters and animation (AI):** character profiles with reference images and palette;
  pose skeletons with an editor and idle/walk/run/jump cycles; Generate Animation produces
  one pose-guided frame per pose (OpenPose ControlNet, IP-Adapter references), scores them
  for consistency, corrects or regenerates the outliers and commits them as a tagged layer in
  one undo step. Optional live preview while generating.
- **Assets:**
  - Tileset Generator: seamless autotile sets, Blob 47, Wang 16 or Cardinal 16, with a base and up to three terrain levels.
  - Terrain Sandbox, Parallax Background and UI Kit Generator (9-slice).
  - Smart Slicer and a deterministic atlas packer.
- **Game engines:**
  - Godot 4: SpriteFrames, TileSet terrains and StyleBoxTexture `.tres` files.
  - Unity: sprite `.meta` files with borders and pivots, and `.anim` clips.
  - Tiled: `.tsx` Wang sets.
  - Choose them in the Export, Tileset, UI kit and Atlas dialogs, the CLI (`--engines`), Lua or MCP.
- **Lua scripting:** sandboxed Lua 5.4 with the `ace` API (images, documents, every asset
  tool). Run from **Scripts**; a script's changes are one undo step. See [docs/lua-api.md](docs/lua-api.md).
- **AI assistants (MCP):** `acestudio-mcp` lets Claude Code, Cursor, VS Code, Codex and other
  MCP clients create, render and export projects, generate tilesets, atlases and UI kits, run
  local AI and Lua, and work on the document open in the app. Setup: **Scripts › Set Up AI
  Assistants** (see below).
- **Safety:** atomic saves with `.bak`, autosave to recovery files, crash-recovery prompt.
- **Desktop app:** multi-document tabs; dockable Tools, Layers, Properties, Palette, Timeline
  Preview and Log panels; dark and light themes; pixel-exact canvas (nearest-neighbour,
  integer zoom, grid); recent projects; layout persistence.
- **Project format:** versioned ZIP + JSON + PNG ([spec](docs/project-format.md)); atomic saves
  with a `.bak` of the previous version; corruption and newer-version detection.
- **CLI:** `acestudio-cli new | info | validate | render | export | tileset | seams | tileset-template |
  atlas | slice | uikit | run | ai | version`, all with `--json`.

### Shortcuts

| Key | Action | Key | Action |
|---|---|---|---|
| B | Pencil | E | Eraser |
| L | Line | U | Rectangle |
| O | Ellipse | G | Paint bucket |
| I | Eyedropper (Alt = temporary) | M | Rectangle select |
| H / Space | Hand / pan | X | Swap colours |
| Q | Lasso select | W | Magic wand (Shift add, Alt subtract) |
| D | Gradient | Ctrl+Shift+R | Rotate layer (RotSprite) |
| [ / ] | Brush size | Shift | Square/circle shapes, line from last point |
| Ctrl+Z / Ctrl+Y | Undo / redo | Ctrl+A / Ctrl+D | Select all / deselect |
| Delete | Clear selection or layer | Ctrl+Shift+N | New layer |
| Ctrl+J / Ctrl+E | Duplicate layer / merge down | Alt+N / Alt+D | New / duplicate frame |
| Enter | Play / pause | , / . | Previous / next frame |
| Alt+← / Alt+→ | Move frame left / right | F3 | Onion skin on/off |
| Ctrl+Alt+T | Animation tags | | |

## Build

Requirements: CMake ≥ 3.25, a C++20 compiler, Git (dependencies are fetched and pinned by
CMake), and Qt ≥ 6.4 (6.12 LTS recommended) for the desktop app.

### Windows (MSVC 2022 + Qt 6.12)

From a "x64 Native Tools Command Prompt for VS 2022":

```bat
set CMAKE_PREFIX_PATH=C:\Qt\6.12.0\msvc2022_64
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release
build\windows-release\app\AceStudio.exe
```

Before running outside the build tree, stage Qt next to the executable (dynamic linking is
required by the LGPL):

```bat
windeployqt --release build\windows-release\app\AceStudio.exe
```

### Core, CLI and tests only (no Qt)

```sh
cmake --preset core-only
cmake --build --preset core-only
ctest --preset core-only
```

## CLI quick reference

```sh
acestudio-cli new hero.acestudio --width 64 --height 64 --frames 4
acestudio-cli --json info hero.acestudio
acestudio-cli validate hero.acestudio          # exit 3 if damaged
acestudio-cli render hero.acestudio --frame 0 --scale 8 --out hero.png
acestudio-cli export hero.acestudio --format sheet --layout packed --trim --out hero_sheet
acestudio-cli export hero.acestudio --format gif --tag walk --scale 4 --out walk.gif
acestudio-cli ai setup --profile onnx-windows --yes          # private Python env + packages
acestudio-cli ai download birefnet-lite --accept-license MIT
acestudio-cli ai remove-bg photo.png --out cutout.png
```

AI requirements: Python 3.10+ installed (python.org, "Add to PATH"); for generation an NVIDIA
GPU with 8 GB+ VRAM (12 GB recommended). AI files live in `%LOCALAPPDATA%\AceStudio\ai`.

```sh
acestudio-cli export hero.acestudio --format sheet --engines godot,unity --out hero   # + .tres, .meta, .anim
acestudio-cli tileset --base water.png --terrain sand.png,grass.png --tile 16 --engines godot,tiled --out ground.png
acestudio-cli uikit --out ui.png --scale 2 --engines godot
acestudio-cli run make_walk.lua --allow assets     # sandboxed Lua (docs/lua-api.md)
```

Exit codes: 0 ok · 1 usage · 2 I/O or not found · 3 corrupt (or seams found) · 4 newer version ·
5 internal · 6 the Lua script failed.
Existing files are never overwritten without `--force`.

## AI assistants (MCP)

`acestudio-mcp` is an [MCP](https://modelcontextprotocol.io) server. It speaks protocol
2026-07-28, and 2025-11-25 and older through `initialize`. In the app, **Scripts › Set Up AI
Assistants** shows a ready-to-paste setup for your client. `acestudio-mcp --print-config
claude-code|cursor|vscode|codex` prints the same. For example, for Claude Code:

```sh
claude mcp add --transport stdio --scope user acestudio -- "C:\path\to\AceStudio\acestudio-mcp.exe"
```

- **Files.** The tools only read and write files inside the `--root` folders. The default is
  the folder the client starts the server in, usually your project. Existing files are kept
  unless the assistant asks to overwrite.
- **The open document.** `app_document`, `app_run_lua` and `app_open_file` work with the
  document open in a running AceStudio. Each change is one Edit › Undo step. Turn this off with
  **Scripts › Allow AI Assistants to Use the Open Document**.
- **HTTP.** `acestudio-mcp --http 39200` serves Streamable HTTP at
  `http://127.0.0.1:39200/mcp`, on loopback only.

## Portable mode

Put an empty file named `portable` next to `AceStudio.exe` (or `acestudio-cli.exe`) and all
settings, logs, recovery files and AI data are kept in a `data` folder beside it instead of
the user profile.

## User data

Settings and logs live in `%APPDATA%\AceStudio` (Linux: `~/.config/AceStudio`). Set
`ACESTUDIO_DATA_DIR` to use another folder (portable installs, testing).

## Documentation

| File | Purpose |
|---|---|
| [ARCHITECTURE.md](ARCHITECTURE.md) | System structure and decision records |
| [ROADMAP.md](ROADMAP.md) | Phases with acceptance criteria and status |
| [TESTING.md](TESTING.md) | How to test, and recorded results |
| [LICENSE_AUDIT.md](LICENSE_AUDIT.md) | Third-party and AI model license audit |
| [KNOWN_ISSUES.md](KNOWN_ISSUES.md) | Current limitations |
| [CHANGELOG.md](CHANGELOG.md) | Release history |
| [docs/lua-api.md](docs/lua-api.md) | The Lua scripting API and sandbox |
| [tests/mcp/README.md](tests/mcp/README.md) | MCP conformance and SDK client checks |
| [tests/engine/README.md](tests/engine/README.md) | Godot and Unity import checks |
| [docs/research/](docs/research/) | Stack and AI model research with sources |

## License

Proprietary — see [LICENSE](LICENSE). Third-party components: [LICENSE_AUDIT.md](LICENSE_AUDIT.md).
