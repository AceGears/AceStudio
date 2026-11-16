# .acestudio project format — schema versions 1–4

A file is written with the lowest schema that can hold it, so a project that uses no newer
feature stays readable by older builds:

| Schema | Needed when the document has |
|---|---|
| 1 | Raster and group layers, frames, cels, tags, palette, extensions |
| 2 | Tile mode, layer masks, clipping masks or adjustment layers |
| 3 | Levels or Curves adjustment layers, or linked cels |
| 4 | A rig (`"rig"` in `document.json`, below) |
| 5 | Vector shapes on cels (`"vectors"` in `document.json`, below) |

A project file is a standard ZIP archive. Any ZIP tool can open it for inspection.

## Entries

| Entry | Compression | Contents |
|---|---|---|
| `manifest.json` | deflate | Format identity and version (always written first) |
| `document.json` | deflate | Document model (no pixel data) |
| `cels/L<layerId>_F<frame>.png` | stored | One RGBA PNG per non-empty cel |

## manifest.json

```json
{
  "format": "acestudio.project",
  "schemaVersion": 1,
  "writerVersion": "0.1.0",
  "createdUtc": "2026-10-29T17:33:00.123Z",
  "modifiedUtc": "2026-10-29T18:01:12.456Z"
}
```

Readers must reject a different `format`. A `schemaVersion` greater than the reader supports
is refused with an "update AceStudio" message; never partially read.

## document.json

```json
{
  "canvas": { "width": 64, "height": 64, "colorMode": "rgba|indexed|grayscale",
              "pixelAspect": [1.0, 1.0], "colorProfile": "sRGB" },
  "palette": { "name": "AceStudio Starter 16", "colors": ["#000000FF", "..."] },
  "layers": [
    { "id": 1, "parent": 0, "kind": "raster|group", "name": "Body",
      "visible": true, "locked": false, "alphaLocked": false,
      "opacity": 255, "blend": "normal" }
  ],
  "frames": [ { "durationMs": 100 } ],
  "cels": [ { "layer": 1, "frame": 0, "x": 0, "y": 0, "file": "cels/L1_F0.png" } ],
  "tags": [ { "name": "idle", "from": 0, "to": 3, "direction": "forward|reverse|pingpong",
              "repeat": 0, "color": "#4F8CFF" } ],
  "extensions": { }
}
```

Rules:
- `layers` are ordered bottom → top. `parent` 0 = top level; otherwise the id of a group layer.
- Layer ids are unique, non-zero and stable across saves.
- `frames` is non-empty; each duration is 1–65535 ms.
- Cel `file` must equal `cels/L<layer>_F<frame>.png`; cels may only reference raster layers.
- Blend modes: normal, multiply, screen, overlay, darken, lighten, add, subtract, difference.
- `extensions` is a free-form object with namespaced keys (e.g. `ai`, `export`, `tileset`).
  Readers must preserve unknown keys.

## rig (schema 4)

Present only in documents that have a rig (ADR-ANIM-004). Parts are the document's own raster
layers; the rig stores no pixels.

```json
"rig": {
  "version": 1,
  "nextId": 6,
  "bones": [
    { "id": 1, "parent": 0, "name": "body", "x": 36.0, "y": 53.0, "length": 46.0, "angle": -1.5708 },
    { "id": 2, "parent": 1, "name": "leg0 thigh", "x": 31.5, "y": 53.0, "length": 3.0, "angle": 1.73,
      "limits": [-0.8, 0.8], "locked": true }
  ],
  "bindings": [ { "layer": 7, "bone": 2 } ],
  "chains": [ { "name": "leg0 IK", "tip": 3, "bend": -1 } ]
}
```

Rules:
- `version` greater than the reader supports is refused ("update AceStudio").
- Bones are listed parents first. Ids are unique, non-zero and below `nextId`; `parent` 0 is a
  root. Names are unique, 1–128 characters.
- `x`, `y` are the head (the joint the bone turns about) at rest, in canvas pixels, y down;
  `length` ≥ 0; `angle` is the rest direction in radians, clockwise on screen from +x.
- `limits` (optional): the smallest and largest turn from rest relative to the parent, radians,
  ordered, within a full turn either way. `locked` (optional): posing leaves the bone at rest.
- Each binding names a raster layer of the document and a bone; a layer is bound at most once.
- A chain's `tip` is a bone that has a parent; `bend` is 1 or -1; one chain per tip.
- At most 256 bones and 64 chains. Anything else invalid makes the file unreadable
  (`Rig: ...`), it is never partly read.

### Rig version 2: rest frame and pose keys

A rig with a rest frame other than 0, or with keys, is written as `"version": 2` with two more
fields (a rig without either is still written as version 1, so older readers keep opening it):

```json
"restFrame": 0,
"keys": [
  { "frame": 0, "pose": { "turn": [], "dx": 0, "dy": 0 }, "curve": { "type": "ease-in-out" } },
  { "frame": 6, "pose": { "turn": [ { "bone": 2, "angle": 0.61 } ], "dx": 0, "dy": -2 },
    "curve": { "type": "bezier", "points": [0.42, 0, 0.58, 1] } }
]
```

- `restFrame`: the frame whose part cels are the parts at rest; every pose and key is drawn from
  it. It must be a frame of the document.
- `keys`: in frame order, at most one per frame, at most 10,000, each on a frame of the document.
  `turn` angles are radians relative to the bone's rest turn (a missing bone means 0); `dx`, `dy`
  move the whole rig in pixels.
- `curve.type`: `linear`, `ease-in`, `ease-out`, `ease-in-out`, `stepped` or `bezier`; it shapes
  the frames from this key to the next. `bezier` carries `points` [x1, y1, x2, y2] with x in 0..1
  (a CSS-style cubic easing curve). The last key's curve is unused.
- Inserting, moving or deleting frames moves keys with their frames; a key on a deleted frame goes,
  and a deleted rest frame falls back to frame 0.

## vectors (schema 5)

Present only in documents that have vector shapes (ADR-ANIM-005). The cel's pixels are stored as
usual; the shapes are what they were drawn from.

```json
"vectors": [
  { "layer": 3, "frame": 0, "shapes": [
    { "kind": "path", "closed": true,
      "points": [ { "x": 4, "y": 20, "out": [6, -16] }, { "x": 28, "y": 20, "in": [-6, -16] }, { "x": 16, "y": 30 } ],
      "stroke": { "style": "dashed", "color": "#141414ff", "width": 1, "dash": 4, "gap": 2 },
      "fill": { "style": "linear", "color": "#dc2828ff", "color2": "#283cdcff", "dither": true, "from": [4, 20], "to": [28, 20] } },
    { "kind": "rectangle", "points": [ { "x": 2, "y": 2 }, { "x": 12, "y": 10 } ],
      "stroke": { "style": "solid", "color": "#000000ff", "width": 2, "dash": 4, "gap": 2 }, "fill": { "style": "none" } }
  ] }
]
```

Rules:
- `layer` is a raster layer and `frame` a frame of the document; each (layer, frame) appears once
  and has at least one shape (at most 1,000).
- `kind`: `path` (2 or more anchors, at most 4,096), `rectangle` or `ellipse` (exactly two opposite
  corners, inclusive pixels). Anchor `in` / `out` are Bezier handle offsets; omitted = 0 (a corner).
  `closed` joins the last anchor to the first; a path's fill always closes it.
- `stroke.style`: `none`, `solid`, `dashed` or `dotted`; `width` 1–32 (a square pen); `dash` and
  `gap` 1–256 pixels along the outline.
- `fill.style`: `none`, `solid`, `linear` or `radial`; gradients run from `color` at `from` to
  `color2` at `to` (radial: centre and a point on the edge), ordered-dithered when `dither`.
- Colours are `#rrggbb` or `#rrggbbaa`. Anything invalid makes the file unreadable
  (`Vector shapes: ...`), it is never partly read.

## Limits enforced on read

- Image dimensions ≤ 16384 per side; frames ≤ 10000; layers ≤ 1000; palette ≤ 256 colors.
- Each ZIP entry ≤ 512 MiB uncompressed; whole project ≤ 4 GiB uncompressed.
- CRC-32 of every entry is verified on extraction.

## Save procedure

1. Validate the in-memory document.
2. Serialize to memory and **deserialize it again** to verify.
3. Write to `<name>.acestudio.tmp-<random>` and flush to disk.
4. Copy the previous `<name>.acestudio` to `<name>.acestudio.bak`.
5. Atomically replace `<name>.acestudio` with the temp file.
