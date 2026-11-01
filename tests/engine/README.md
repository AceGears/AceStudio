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

1. Make a throwaway project folder with `Assets/AceStudio/`, holding
   `engine_fixture`'s PNG, `.meta`, `.anim` and `.json` files, and
   `Assets/Editor/AceStudioImportCheck.cs` (from `tests/engine/unity/`).
2. Open the folder in Unity (Unity Hub › Add › Add project from disk). After the
   import, the script writes `acestudio_unity_report.json` in the project folder.
   **AceStudio › Write Import Report** runs it again.
3. Run `python tests/engine/compare_unity.py <fixture folder> acestudio_unity_report.json`.

The report lists every sprite Unity made from AceStudio's `.meta` files, the import
settings and every clip's sprite keys. The comparison checks these:

- Every texture is a Sprite in Multiple mode, Point filtered, uncompressed, without mipmaps.
- Sprite names, rects, 9-slice borders, pivots and pixels per unit match the `.meta`.
- The rects are the JSON frames flipped to Unity's bottom-left origin.
- The 9-slice borders match the UI kit JSON.
- Clip keys, lengths and loop flags match the `.anim` files.

Last run: Unity 6000.6.7f1 (Windows), 2026-11-01. Everything matched: 11 textures, 190
sprites, 3 clips and 13 keys, with no import errors in the Editor log. Clip lengths equal the
frame durations: walk 0.40 s, idle 0.45 s, die 0.65 s. The report is kept as
`unity/report-6000.6.7f1.json`.
