# Changelog

All notable changes are documented here. Format follows Keep a Changelog; versions follow SemVer.

## [0.1.0] — 2026-10-29 — Phase 1 foundation (unreleased)

### Added
- `acestudio_core` (Qt-free): RGBA8 images, W3C blend modes, palettes (≤256), document model
  with layers, groups, frames, cels, animation tags and extension metadata; flattening.
- `.acestudio` project format v1: ZIP + JSON + PNG cels, CRC verification, size limits,
  schema versioning, verify-before-write, atomic save with `.bak` backup.
- Atomic file writes with flush-to-disk on Windows and POSIX.
- Typed, versioned settings with corrupt-file quarantine and newer-schema protection.
- Rotating, thread-safe logger with live listener.
- `acestudio-cli`: `version`, `new`, `info`, `validate`, `render`; `--json` output; documented
  exit codes; never overwrites without `--force`.
- Qt 6 desktop shell: welcome page, multi-document tabs, dockable Layers/Properties/Palette/
  Timeline/Log panels, dark/light themes, pixel-exact canvas with integer zoom, grid,
  cursor-anchored zoom and panning, frame playback honouring per-frame durations, recent
  projects, persisted layout, drag-and-drop open, unsaved-changes prompts.
- Build: CMake ≥ 3.25, pinned dependencies, strict warnings, CMake presets, Qt license guard.
- Tests: 39 Catch2 unit tests + 1 CLI end-to-end test.
- CI workflow for Linux (core) and Windows (MSVC + Qt 6.12).
- Docs: architecture/ADRs, project format spec, research notes, license audit, roadmap.
