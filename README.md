# AceStudio

AI-assisted game asset creation and 2D animation studio for Windows.

> **Status: Phase 2a (core editor), version 0.2.0.** You can draw pixel art with layers and
> frames, undo anything, and recover work after a crash. Animation tooling, AI generation and
> engine export are planned — see [ROADMAP.md](ROADMAP.md). Nothing in the app pretends to
> work when it doesn't; unavailable menus are disabled and name the phase that delivers them.

## What works today

- **Drawing:** pencil (pixel-perfect), eraser, line, rectangle, ellipse, paint bucket,
  eyedropper, rectangle select, hand; brush size/shape; mirror painting; alpha lock; indexed
  and grayscale colour constraints.
- **Editing:** undo/redo for everything; layers (add, group, rename, reorder, duplicate,
  merge, delete, opacity, blend, locks); frames (insert, duplicate, delete, duration); palette
  editing; PNG import/export.
- **Safety:** atomic saves with `.bak`, autosave to recovery files, crash-recovery prompt.
- **Desktop app:** multi-document tabs; dockable Tools, Layers, Properties, Palette, Timeline
  and Log panels; dark and light themes; pixel-exact canvas (nearest-neighbour, integer zoom,
  grid); frame playback; recent projects; layout persistence.
- **Project format:** versioned ZIP + JSON + PNG ([spec](docs/project-format.md)); atomic saves
  with a `.bak` of the previous version; corruption and newer-version detection.
- **CLI:** `acestudio-cli new | info | validate | render | version`, all with `--json`.

### Shortcuts

| Key | Action | Key | Action |
|---|---|---|---|
| B | Pencil | E | Eraser |
| L | Line | U | Rectangle |
| O | Ellipse | G | Paint bucket |
| I | Eyedropper (Alt = temporary) | M | Rectangle select |
| H / Space | Hand / pan | X | Swap colours |
| [ / ] | Brush size | Shift | Square/circle shapes, line from last point |
| Ctrl+Z / Ctrl+Y | Undo / redo | Ctrl+A / Ctrl+D | Select all / deselect |
| Delete | Clear selection or layer | Ctrl+Shift+N | New layer |
| Ctrl+J / Ctrl+E | Duplicate layer / merge down | Alt+N / Alt+D | New / duplicate frame |
| Enter | Play / pause | , / . | Previous / next frame |

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
```

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
