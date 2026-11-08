---
name: engine-export-validation
description: Verify AceStudio exports (sprite sheets, atlases, frame JSON, Godot .tres SpriteFrames, Unity/Phaser/GameMaker metadata, tilesets) by pixel round-trip and by importing them into the actual engines where installed, recording engine versions and results. Use after any export change and before releases.
---

# Engine Export Validation

## 1. Pixel round-trip (always)
Re-slice the exported sheet using its own metadata and compare each frame to the source frames pixel-exactly:
```bash
python -I .claude/skills/animation-quality/scripts/anim_qa.py <export.png> --json <export.json> --report rt.json
```
Check: frame count, frame sizes, per-frame durations, pivots/origins, trimmed offsets (`spriteSourceSize`), no filtering (palette unchanged), binary alpha for pixel art.

## 2. Metadata schema checks
- Aseprite/Phaser JSON: `frames` hash or array, `meta.size`, `meta.frameTags` if animations are tagged.
- Godot 4 `.tres`: `[gd_resource type="SpriteFrames" format=3]`, `AtlasTexture` regions match JSON rects, `speed`/`duration` correct, texture path relative (`res://`).
- Unity: sprite import mode Multiple, filter Point, compression None for pixel art, pivots in normalized coords.
- GameMaker: strip/frames count and origin.
- Tilesets: tile size, separation/margin, terrain/peering bits, collision polygons.

## 3. Engine import (when the engine is installed — else mark SKIPPED with reason)
- **Godot 4:** copy into a scratch project, run `godot --headless --path <proj> --script res://validate.gd` that loads the `.tres`, asserts frame counts/sizes per animation and quits with an exit code.
- **Unity:** use the existing `UnityImportTest` project; run in batchmode `-executeMethod` validator writing `acestudio_unity_report.json`; check the report.
- **Phaser:** load atlas JSON in a minimal headless test page (only if a local Node/browser setup exists).
- **GameMaker:** manual import, screenshot evidence.

## 4. Record
`docs/reviews/export/<date>.md`: engine + version, asset, result (PASS/FAIL/SKIPPED), evidence path.
