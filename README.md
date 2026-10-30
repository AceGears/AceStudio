# AceStudio

AI-assisted game asset creation and 2D animation studio for Windows.

> **Status: Phase 4 (AI infrastructure), version 0.5.0.** You can draw and animate pixel art
> with layers, frames, tags and onion skin, export sprite sheets (Aseprite-compatible JSON),
> GIF, APNG, WebP and PNG sequences, undo anything, and recover work after a crash. AI Studio
> runs models locally in a supervised worker: background removal, upscaling and SDXL
> generation, with licence-gated model downloads. Real SDXL and BiRefNet runs still need
> verifying on a GPU machine. Tilesets and engine-specific export are planned — see [ROADMAP.md](ROADMAP.md). Nothing in the app pretends to
> work when it doesn't; unavailable menus are disabled and name the phase that delivers them.

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
- **Safety:** atomic saves with `.bak`, autosave to recovery files, crash-recovery prompt.
- **Desktop app:** multi-document tabs; dockable Tools, Layers, Properties, Palette, Timeline
  Preview and Log panels; dark and light themes; pixel-exact canvas (nearest-neighbour,
  integer zoom, grid); recent projects; layout persistence.
- **Project format:** versioned ZIP + JSON + PNG ([spec](docs/project-format.md)); atomic saves
  with a `.bak` of the previous version; corruption and newer-version detection.
- **CLI:** `acestudio-cli new | info | validate | render | export | ai | version`, all with `--json`.

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

Exit codes: 0 ok · 1 usage · 2 I/O or not found · 3 corrupt · 4 newer version · 5 internal.
Existing files are never overwritten without `--force`.

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
| [docs/research/](docs/research/) | Stack and AI model research with sources |

## License

Proprietary — see [LICENSE](LICENSE). Third-party components: [LICENSE_AUDIT.md](LICENSE_AUDIT.md).
