# Real-engine checks

The engine exporters (`core/EngineExport`, `core/AssetFiles`) are unit-tested
for their text formats; these checks load the output in the real engines.

## Godot 4

    tests/engine/run_godot_check.sh <godot-4 binary> <build dir> <work dir>

Builds `engine_fixture`'s output into a throwaway project, runs
`godot --headless --import`, then `check.gd`, which loads the SpriteFrames,
StyleBoxTexture and TileSet resources, paints the fixture's level map with
Godot's own terrain tools (`TileMapLayer.set_cells_terrain_connect`) and
reports the tile chosen for every cell. `compare_godot.py` compares that with
AceStudio's placement: for Blob47 and Cardinal16 every cell must match.

Last run: Godot 4.6.2-stable (Linux, headless), 2026-10-31: all cells match.

## Unity

Unity is not scriptable here; the same fixture is copied into a Unity project's
`Assets/` folder on the test PC and checked in the editor (see TESTING.md).
