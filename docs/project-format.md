# .acestudio project format — schema version 1

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
