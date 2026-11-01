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
   `engine_fixture`'s PNG, `.meta`, `.anim`, `.asset`, `.cs` and `.json` files, and
   `Assets/Editor/AceStudioImportCheck.cs` and `AceStudioPackageSetup.cs` (from
   `tests/engine/unity/`). The second one adds `com.unity.2d.tilemap.extras`
   through the Package Manager the first time the project opens.
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
- The tiles load as their types (`AceStudio.AceStudioAutoTile`, `UnityEngine.RuleTile`,
  `AceStudio.AceStudioWangSet`). Painting the fixture's level map with them (level 1 or the
  base tile on one Tilemap, level 2 on another) makes Unity choose the same tile as AceStudio
  in every cell: AceStudio's autotiles and the Rule Tiles for Blob47 and Cardinal16, and the
  AceStudioDualGrid corner tiles for Wang16, both as painted and after a rebuild.

Last run: Unity 6000.6.7f1 (Windows) with 2D Tilemap Extras 9.0.1, 2026-11-01. Everything
matched: 11 textures, 190 sprites, 3 clips, 13 keys and 1780 painted cells, with no console
warnings. Clip lengths equal the
frame durations: walk 0.40 s, idle 0.45 s, die 0.65 s. The report is kept as
`unity/report-6000.6.7f1.json`.

## Phaser 3 and 4

    cd <work dir>
    acestudio-cli run <repo>/tests/engine/phaser/make.lua --base .
    acestudio-cli export hero.acestudio --format sheet --out out/hero.png --engines phaser,gamemaker
    npm install --ignore-scripts phaser@3.90.0
    npm install --ignore-scripts --prefix p4 phaser@4.2.1
    sed 's#PHASER#node_modules/phaser/dist/phaser.min.js#' <repo>/tests/engine/phaser/check.html.in > check3.html
    sed 's#PHASER#p4/node_modules/phaser/dist/phaser.min.js#' <repo>/tests/engine/phaser/check.html.in > check4.html
    NODE_PATH=$(npm root -g) node <repo>/tests/engine/phaser/run.js .

`run.js` serves the folder, opens both pages in headless Chromium (Playwright),
loads the sheet with the exported `hero_phaser.js` (`load.aseprite`,
`anims.createFromAseprite`) and prints each animation's frames, durations and
repeat.

Last run (2026-11-01): Phaser 3.90.0 and 4.2.1 both made `walk` (frames 0-3,
120/100/100/100 ms) and `hit` (frames 4-5), looping, and played `walk`. The
first run found a real problem: Phaser looks frames up by number, and the
sheet named them "hero 0", … so no animation got frames. Sheets exported for
Phaser now name frames "0", "1", ….

## GameMaker

Not run in GameMaker (not available here). The export follows GameMaker's
strip convention: `<name>_stripN.png` is split into N frames when imported,
and `<stem>_gamemaker.txt` lists each sprite's speed in frames per second.
