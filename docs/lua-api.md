# AceStudio Lua API

AceStudio runs **Lua 5.4.9** scripts (embedded through sol2 3.3.0) in a sandbox.
You can run a script from:

- the app: **Scripts > Run Script…**;
- the command line: `acestudio-cli run script.lua [args…]`;
- the MCP server: the `run_lua` tool.

Frames, layers and pixel coordinates are **0-based**, the same as in the CLI,
the JSON formats and the MCP server. Lua lists returned by the API are 1-based
tables as usual. For example, `doc:layers()[1]` is the layer with `index = 0`.

## Sandbox

| Available | Removed |
|---|---|
| `string` (except `string.dump`), `table`, `math`, `utf8`, `coroutine` | `io`, `package`, `debug`, native modules |
| `require` for `.lua` modules in the allowed folders (see below) | `require` of anything else |
| base functions (`print`, `pairs`, `pcall`, `error`, `tostring`, `setmetatable`, …) | `dofile`, `loadfile` |
| `load` for **text** chunks only (`mode` is always `"t"`) | loading precompiled bytecode |
| `os.time`, `os.clock`, `os.date`, `os.difftime` | everything else in `os` (`execute`, `remove`, `rename`, `getenv`, `exit`, `tmpname`, …) |

Limits:

- **Files.** A script can reach files only through the `ace` functions below, and only inside the allowed folders:
  - The script's own folder is always allowed.
  - With the CLI, add more folders with `--allow dir;dir`.
  - In the app, the allowed folder is the script's folder and the folder of the current document.
  - Relative paths are resolved against the first allowed folder.
  - `..` and symlinks are resolved before the check.
- **Memory.** 512 MB by default (CLI `--memory MB`). The limit covers the Lua heap, the pixels of every image the script holds, and the documents it creates, opens or grows (cels and undo history). Going over the limit raises `not enough memory`; a large image is refused before it is allocated.
- **Time.** 30 s by default (CLI `--timeout s`, where 0 means no limit). The app's Cancel button stops a script, and `pcall` cannot catch either of these stops for long, because the check fires again.
- **Overwriting.** Writing tools refuse to replace existing files unless you pass `overwrite = true`.

Errors give the file name and line, for example `make.lua:12: there is no frame 4`.

## Modules

`require("name")` loads `name.lua` from the allowed folders (first match wins); dots are folders, so `require("lib.colors")` loads `lib/colors.lua`.

- Module names use letters, digits, `_`, `-` and `.`; paths such as `../x` or `/etc/x` are refused.
- A module runs in the same sandbox, once; later calls return the cached value (`true` when the module returns nothing).
- A module that requires itself, directly or through others, raises an error.

## Arguments and output

- `arg` is a list of the strings passed after the script name, for example `acestudio-cli run make.lua grass 16`.
- `print(...)` and `ace.log(...)` write to the CLI's output, the app's script console, or the MCP result.

## Values

- `ace.version` is the AceStudio version; `ace.lua` is `"Lua 5.4.9"`.
- **Colours** are written as `"#rrggbb"`, `"#rrggbbaa"` or `{r, g, b[, a]}` / `{r=…, g=…, b=…, a=…}`, with channels 0–255.

## Images

`ace.image(w, h[, colour])` makes a new image, transparent unless you give a colour. Sizes are 1–16384.
`ace.load_png(path)` loads a PNG.

| Member | |
|---|---|
| `img.width`, `img.height` | size |
| `img:get(x, y)` | `r, g, b, a` |
| `img:hex(x, y)` | `"#rrggbbaa"` |
| `img:set(x, y, colour)` | set one pixel (errors outside the image) |
| `img:fill(colour)` | fill everything |
| `img:rect(x, y, w, h, colour)` | fill a rectangle (clipped) |
| `img:line(x0, y0, x1, y1, colour)` | 1 px line |
| `img:copy([x, y, w, h])` | a new image (all, or a region) |
| `img:blit(src, x, y)` | copy `src` in, replacing pixels |
| `img:draw(src, x, y)` | draw `src` over, with alpha blending |
| `img:scale(w, h)` | nearest-neighbour resize (new image) |
| `img:flip(["h"\|"v"])` | mirrored copy |
| `img:rotate(90\|180\|270)` | rotated copy, clockwise |
| `img:outline(colour[, diagonal])` | copy with a 1 px outline outside the opaque pixels |
| `img:save(path)` | write a PNG |

`ace.write_text(path, text)` writes a text file, for example JSON you built yourself.

## Documents

- `ace.document()` is the document open in the app, or the CLI `--doc` file. It is `nil` when there is none.
  - All the changes a script makes to it become **one undo step** ("Run Script").
  - If the script fails, its changes are rolled back.
- `ace.new_document(w, h[, frames])` makes a new document with one layer. It is not shown in the app; save or export it yourself.
- `ace.open(path)` loads a `.acestudio` file.

| Member | |
|---|---|
| `doc.width`, `doc.height`, `doc.frames` | size and frame count |
| `doc:layers()` | list of `{index, name, visible, locked, opacity, blend, group}` |
| `doc:add_layer([name])` | adds a layer on top and returns its index |
| `doc:set_layer(layer, {name, visible, locked, opacity, blend})` | changes layer properties (any subset; opacity 0..255; blend such as `"multiply"`) |
| `doc:remove_layer(layer)` | removes a layer (a document keeps at least one) |
| `doc:cel(layer, frame)` | the cel as a canvas-sized image |
| `doc:set_cel(layer, frame, image[, x, y])` | replaces a cel |
| `doc:add_frame([ms])` | appends a frame (default 100 ms) and returns its index |
| `doc:remove_frame(frame)` | removes a frame (a document keeps at least one) |
| `doc:palette()` | the palette as a list of `"#rrggbbaa"` strings |
| `doc:set_palette({colour, ...})` | replaces the palette (up to 256 colours) |
| `doc:duration(frame)`, `doc:set_duration(frame, ms)` | frame timing |
| `doc:add_tag(name, from, to[, "forward"\|"reverse"\|"pingpong"])` | animation tag |
| `doc:tags()` | list of `{name, from, to, direction}` |
| `doc:composite([frame])` | the flattened frame as an image |
| `doc:save(path)` | write a `.acestudio` project |
| `doc:export_sheet(path[, opts])` | sprite sheet + JSON; returns the number of files written |

`export_sheet` options:

- `layout` = `"horizontal"`, `"vertical"`, `"grid"` or `"packed"`;
- `trim` = `true`;
- `scale` = `n`;
- `tag` = `"walk"`;
- `engines` = `"godot,unity,tiled"`;
- `overwrite` = `true`.

## Asset tools

These are the same generators as the Assets menu and the CLI.

```lua
local ts = ace.tileset{
  base = sand_image,                 -- level 0 texture
  terrains = {grass_image, snow},    -- level 1, 2, ... (each over the ones below)
  scheme = "blob47",                 -- or "wang16", "cardinal16"
  tile = 16,                         -- 0 = the texture size
  roundness = 1, noise = 0.3, noise_scale = 4, outline = true, seed = 1,
}
ts.levels          -- number of terrain levels
ts:image(level)    -- tileset image of a level (1-based level)
ts:seams()         -- seam problems found (0 = clean)
ts:write("ground.png", {engines = "godot,unity,tiled", names = {"sand", "grass", "snow"}, overwrite = false})
```

| Function | |
|---|---|
| `ace.check_seams(image[, scheme[, phases]])` | `issues, pairs_checked` for a tileset image |
| `ace.atlas({{name=, image=}, …}[, {padding, extrude, trim, rotate, max}])` | `{image, sprites = {{name, x, y, w, h, rotated}}, json}` |
| `ace.slice(image[, {background, tolerance, merge, grid = "WxH"\|"auto"}])` | list of `{name, x, y, w, h}` |
| `ace.write_uikit(path[, {scale, rounded, base, light, dark, outline, accent, text, engines, overwrite}])` | writes the UI kit; returns the file count |

## Example

```lua
-- make_walk.lua: a 4-frame bouncing square, exported for Godot and Unity
local d = ace.new_document(16, 16, 4)
for f = 0, 3 do
  local img = ace.image(16, 16)
  img:rect(4, 8 - f % 2 * 2, 8, 8, "#e04040")
  d:set_cel(0, f, img:outline("#201010"))
  d:set_duration(f, 120)
end
d:add_tag("walk", 0, 3)
print(d:export_sheet("walk.png", {engines = "godot,unity"}), "files")
```

Run it with:

```
acestudio-cli run make_walk.lua
```
