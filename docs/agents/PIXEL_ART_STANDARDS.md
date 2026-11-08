# Pixel Art Standards

Owner: `pixel-art-director`. Checked with `.claude/skills/pixel-art-quality/scripts/pixel_qa.py` plus visual review.

| Asset class | Typical size | Palette limit | Outline |
|---|---|---|---|
| Character / creature | 16–128 px | 32 | consistent 1 px; selective outline allowed |
| Boss | 64–256 px | 48 | consistent |
| Item / weapon / loot | 16–32 px | 16 | 1 px dark outline |
| Prop | 16–64 px | 24 | matches environment style |
| Tile | 8–32 px | per tileset ≤ 32 | none inside tiles; edges must match |
| Effect | any | 12 per effect | none |
| UI | any | project UI palette | crisp, no AA on edges |

Hard fails:
- Semi-transparent pixels.
- Upscaled images presented as native-resolution pixel art.
- Palette above the limit.
- Blur or soft edges.
- Orphan-pixel noise over 5% of the opaque area.
- Mixed pixel scales.
- Inconsistent light direction within an asset family.
- Banding or pillow shading.
