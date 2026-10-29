# Known Issues and Limitations

Current as of 0.1.0 (Phase 1).

## Not yet verified
- **Not yet run on real Windows or with MSVC.** Core, CLI and tests cross-compile for Windows
  with MinGW-w64 and all pass under Wine, which exercises the Win32 file code
  (`MoveFileExW`, `FlushFileBuffers`, `_wfopen`, `SHGetKnownFolderPath`). The Qt app has not
  been built for Windows, and Wine is not a substitute for NTFS/antivirus behaviour on a real
  machine. The CI Windows job is written but has not run.
- **Qt 6.12 not yet compiled against.** Code targets Qt ≥ 6.4 APIs; `jurplel/install-qt-action`
  availability of 6.12 is unverified.
- **High-DPI** behaviour has not been checked at 150%/200% Windows scaling.

## Functional limitations (by design for this phase)
- No drawing tools, undo/redo, or layer editing UI yet (Phase 2). The only document edit
  available in the app is toggling layer visibility, which marks the document modified.
- No autosave or crash-recovery prompt yet (Phase 2). Saving is atomic and keeps a `.bak`.
- Group layers are composited by multiplying opacity down the hierarchy; group blend modes are
  not applied in isolation. Proper isolated group compositing is planned for Phase 2.
- Indexed and grayscale color modes are stored but not enforced during painting (no painting yet).
- No AI, tileset, MCP, Lua or plugin functionality yet; the corresponding menus contain a single
  disabled entry naming the planned phase.
- Workspace presets (Pixel Art, Animation, …) are not implemented; layout persistence and
  Reset Layout are.

## Cosmetic
- Long file paths in the Properties panel wrap across several lines.
- The Log panel requests the Consolas font; other platforms fall back to a default monospace.
