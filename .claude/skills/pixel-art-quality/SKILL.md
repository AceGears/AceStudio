---
name: pixel-art-quality
description: Review a pixel-art asset (sprite, tile, prop, UI art, animation frame) for palette discipline, binary alpha, grid alignment, clusters, outlines, orphan pixels and readability at native resolution; produce a PASS/FAIL verdict with pixel coordinates. Use for any generated or edited pixel-art output.
---

# Pixel Art Quality Review

## 1. Automated checks
```bash
python -I .claude/skills/pixel-art-quality/scripts/pixel_qa.py <asset.png> [--max-colors 32] [--report out.json]
```
Reports: dimensions, palette size, semi-transparent pixel count, orphan pixels (no same-colour 4-neighbour), effective pixel scale (detects upscaled "fake" pixel art: runs of identical 2×2/3×3/4×4 blocks), near-duplicate colours (ΔRGB < 12, wasted palette slots), and alpha-edge colours (soft fringes).

Hard fails: semi-transparency in pixel art; effective scale > 1 when native resolution was requested; palette over the class limit in `docs/agents/PIXEL_ART_STANDARDS.md`.

## 2. Visual inspection (required)
View at 1× (readability), and nearest-neighbor 4×/8× crops (clusters). Check:
- **Silhouette** readable at 1× and in solid black fill.
- **Clusters**: no noise/single-pixel speckle; no banding (parallel stair-stepped ramps); no pillow shading.
- **Lines**: consistent outline width; jaggies minimized (pixel runs follow 1:1, 1:2, 2:1 progressions).
- **Light**: single consistent light direction; hue-shifted ramps rather than pure darkening.
- **Materials**: metal/cloth/skin/stone read distinctly.
- **Consistency** with the project style lock and neighbouring assets.

## 3. Report
```
Asset: <file>  Size: <w×h>  Palette: <n> (limit <m>)
Metrics: semi=<n> orphans=<n> scale=<k> near-dupes=<n>
Verdict: PASS | FAIL
Defects: (x,y)-(x,y): <issue> → <fix>
```
