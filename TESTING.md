# Testing

## Running the tests

```sh
cmake --preset core-only && cmake --build --preset core-only && ctest --preset core-only
```

On Windows use the `windows-release` preset. Tests never touch real user data: they use unique
temporary directories, and `ACESTUDIO_DATA_DIR` redirects the app's settings and logs.

## Suites

| Suite | Kind | Covers |
|---|---|---|
| `test_color_image` | unit | hex parsing, luma, image limits, bounds safety, fill/copy/blit clipping, palette capacity and lookups |
| `test_blend` | unit | all 9 blend modes against reference values, opacity, transparent backdrops, exact opaque fast path |
| `test_document` | unit | layer/group hierarchy, cel rules, frame insert/duplicate/remove with cel and tag shifting, compositing with offsets/visibility/opacity/groups, corruption detection (cycles, duplicate IDs) |
| `test_png` | unit | lossless RGBA round trip, garbage rejection, file I/O |
| `test_project_io` | unit/integration | lossless save/load, `.bak` creation, no temp-file leftovers, truncated archive, CRC failure on damaged pixel data, non-ZIP input, newer-schema refusal, refusal to save an invalid document |
| `test_fileutil` | unit | atomic write create/replace/backup, size limits, timestamps, data-dir override |
| `test_settings` | unit | defaults, type enforcement, persistence, corrupt-file quarantine, unknown/mistyped values skipped, newer schema not overwritten |
| `test_log` | unit | level filtering, file sink, listener, rotation |
| `cli_smoke` | end-to-end | every CLI command, JSON output, exit codes, overwrite protection, corrupt-file handling |

## Recorded results

### 2026-10-29 — Phase 1, version 0.1.0

| Environment | Build | Result |
|---|---|---|
| Linux, GCC 13.3, Qt 6.4.2, RelWithDebInfo | core + CLI + tests + app | ✅ 40/40 CTest (39 unit test cases + cli_smoke) |
| Linux, GCC 13.3, Release, `-Werror` (CI config), clean build | core + CLI + tests + app | ✅ builds with zero warnings; 40/40 |
| Windows target via MinGW-w64 GCC (posix), `-Werror`, run under Wine 64 | core + CLI + tests (no app) | ✅ zero warnings; 39/39 test cases, 584 assertions; cli_smoke passed |
| Cross-platform format check | — | ✅ Windows-built CLI writes a project the Linux build validates; Linux-written project read by Windows build; frame renders pixel-identical |

**Not yet tested:** MSVC compilation, Qt 6.12, the desktop app on real Windows, high-DPI
scaling, and the GitHub Actions workflow itself. Wine is not real Windows: NTFS semantics,
antivirus file locking and `MoveFileExW` behaviour under load still need verification on a
Windows 10/11 machine.

### Manual desktop checks (Linux, Xvfb 1600×1000, Qt 6.4.2)

| Check | Result |
|---|---|
| App launches with a project path argument and opens it | ✅ |
| Canvas fits to window at integer zoom (2200% for 32×32), nearest-neighbour, grid visible | ✅ screenshot reviewed |
| Multiply layer at 43% opacity composites correctly (matches CLI render) | ✅ |
| Layers panel shows group nesting, blend mode and opacity | ✅ |
| Timeline: Enter starts playback, frames advance, Enter stops on the current frame | ✅ |
| Ctrl+Q exits cleanly; settings.json written with recent project and window layout; log written | ✅ |
| Defects found and fixed during this check: canvas fitted before final window size; transport icons blank on platforms without stock media icons; truncated frame label | ✅ fixed and re-verified |
