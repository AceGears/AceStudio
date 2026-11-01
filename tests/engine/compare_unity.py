"""Compares Unity's import of the engine fixture with AceStudio's files.

usage: compare_unity.py <fixture folder> <acestudio_unity_report.json>

The report comes from tests/engine/unity/AceStudioImportCheck.cs. Checked:
  * every AceStudio texture imported as a Sprite (Multiple), Point filter,
    uncompressed, no mipmaps;
  * Unity's sprites are exactly the ones in AceStudio's .meta (names, rects,
    9-slice borders, pivots, pixels per unit);
  * the rects are the JSON frames flipped to Unity's bottom-left origin;
  * every .anim became a clip whose sprite keys, length and loop flag match;
  * painting map.json with AceStudio's tiles puts the same tile in every
    cell as AceStudio's own placement: its autotiles and the Rule Tiles for
    Blob47 and Cardinal16, and the dual grid (AceStudioDualGrid) for Wang16,
    both as painted and after a rebuild.
Exit 1 with a list of problems if anything differs.
"""
import json
import os
import re
import sys

fixture, report_path = sys.argv[1], sys.argv[2]
report = json.load(open(report_path, encoding="utf-8"))
problems = []
checked = {"textures": 0, "sprites": 0, "clips": 0, "keys": 0}


def problem(msg):
    problems.append(msg)


def parse_meta(path):
    """Sprites from an AceStudio TextureImporter .meta (the subset we write)."""
    text = open(path, encoding="utf-8").read()
    ppu = float(re.search(r"spritePixelsToUnits: ([\d.]+)", text).group(1))
    sprites = {}
    for block in re.split(r"\n    - serializedVersion: 2\n", text.split("spriteSheet:", 1)[1])[1:]:
        name = re.search(r"\n?      name: (.*)", block).group(1).strip()
        r = {k: float(re.search(rf"        {k}: ([-\d.]+)", block).group(1)) for k in ("x", "y", "width", "height")}
        pivot = re.search(r"pivot: \{x: ([-\d.e]+), y: ([-\d.e]+)\}", block)
        border = re.search(r"border: \{x: ([-\d.]+), y: ([-\d.]+), z: ([-\d.]+), w: ([-\d.]+)\}", block)
        internal = re.search(r"internalID: (-?\d+)", block)
        sprites[name] = {
            "rect": [r["x"], r["y"], r["width"], r["height"]],
            "pivot": [float(pivot.group(1)), float(pivot.group(2))],
            "border": [float(border.group(i)) for i in range(1, 5)],
            "internalID": int(internal.group(1)),
        }
    return ppu, sprites


def parse_anim(path):
    text = open(path, encoding="utf-8").read()
    keys = [(float(t), int(f)) for t, f in re.findall(r"- time: ([\d.e-]+)\n\s+value: \{fileID: (-?\d+)", text)]
    stop = float(re.search(r"m_StopTime: ([\d.e-]+)", text).group(1))
    loop = re.search(r"m_LoopTime: (\d)", text).group(1) == "1"
    return keys, stop, loop


near = lambda a, b, eps=1e-3: abs(a - b) <= eps

textures = {os.path.basename(t["path"]): t for t in report["textures"]}
metas = sorted(f[:-5] for f in os.listdir(fixture) if f.endswith(".png.meta"))
id_to_name = {}
for png in metas:
    t = textures.get(png)
    if t is None:
        problem(f"{png}: not in Unity's report (not imported?)")
        continue
    checked["textures"] += 1
    for key, want in (("textureType", "Sprite"), ("spriteImportMode", "Multiple"), ("filterMode", "Point"),
                      ("compression", "Uncompressed")):
        if t.get(key) != want:
            problem(f"{png}: {key} is {t.get(key)}, expected {want}")
    if t.get("mipmaps"):
        problem(f"{png}: mipmaps are on")
    ppu, ours = parse_meta(os.path.join(fixture, png + ".meta"))
    for name, s in ours.items():
        id_to_name[s["internalID"]] = (png, name)
    theirs = {s["name"]: s for s in t["sprites"]}
    if set(ours) != set(theirs):
        problem(f"{png}: sprites differ: only AceStudio {sorted(set(ours) - set(theirs))[:5]}, "
                f"only Unity {sorted(set(theirs) - set(ours))[:5]}")
    for name in sorted(set(ours) & set(theirs)):
        a, b = ours[name], theirs[name]
        checked["sprites"] += 1
        if any(not near(x, y) for x, y in zip(a["rect"], b["rect"])):
            problem(f"{png}/{name}: rect {b['rect']} in Unity, {a['rect']} in the .meta")
        if any(not near(x, y) for x, y in zip(a["border"], b["border"])):
            problem(f"{png}/{name}: border {b['border']} in Unity, {a['border']} in the .meta")
        if any(not near(x, y) for x, y in zip(a["pivot"], b["pivot"])):
            problem(f"{png}/{name}: pivot {b['pivot']} in Unity, {a['pivot']} in the .meta")
        if not near(b["ppu"], ppu):
            problem(f"{png}/{name}: {b['ppu']} pixels per unit in Unity, {ppu} in the .meta")
    # The rects against the JSON (top-left origin there, bottom-left in Unity).
    js = os.path.join(fixture, png[:-4] + ".json")
    if os.path.exists(js) and "height" in t:
        frames = json.load(open(js, encoding="utf-8")).get("frames")
        if isinstance(frames, dict):
            for index, (fname, fr) in enumerate(frames.items()):
                f = fr["frame"]
                # Sheet frames are "hero 0" in the JSON (Aseprite style) and
                # hero_0 in engine files; atlas and UI kit names are the same.
                u = theirs.get(fname) or theirs.get(png[:-4] + "_" + str(index))
                if u is None:
                    problem(f"{png}: JSON frame {fname!r} has no Unity sprite")
                    continue
                want = [f["x"], t["height"] - f["y"] - f["h"], f["w"], f["h"]]
                if any(not near(x, y) for x, y in zip(want, u["rect"])):
                    problem(f"{png}/{fname}: Unity rect {u['rect']}, JSON frame flipped is {want}")
                ns = fr.get("nineSlice")
                if ns:
                    want_b = [ns["left"], ns["bottom"], ns["right"], ns["top"]]
                    if any(not near(x, y) for x, y in zip(want_b, u["border"])):
                        problem(f"{png}/{fname}: Unity border {u['border']}, JSON 9-slice {want_b}")

clips = {os.path.basename(c["path"]): c for c in report["clips"]}
for anim in sorted(f for f in os.listdir(fixture) if f.endswith(".anim")):
    c = clips.get(anim)
    if c is None:
        problem(f"{anim}: not in Unity's report (not imported?)")
        continue
    checked["clips"] += 1
    keys, stop, loop = parse_anim(os.path.join(fixture, anim))
    sprite_bindings = [b for b in c["bindings"] if b["type"] == "SpriteRenderer" and b["property"] == "m_Sprite"]
    if len(sprite_bindings) != 1:
        problem(f"{anim}: {len(sprite_bindings)} SpriteRenderer.m_Sprite bindings in Unity, expected 1")
        continue
    got = [(k["time"], k["sprite"]) for k in sprite_bindings[0]["keys"]]
    want = [(t, id_to_name.get(f, ("?", f"fileID {f}"))[1]) for t, f in keys]
    dedupe = lambda seq: [x for i, x in enumerate(seq) if i == 0 or not (near(x[0], seq[i - 1][0]) and x[1] == seq[i - 1][1])]
    if len(dedupe(got)) != len(dedupe(want)) or any(not near(a[0], b[0]) or a[1] != b[1] for a, b in zip(dedupe(got), dedupe(want))):
        problem(f"{anim}: keys {got} in Unity, {want} in the .anim")
    checked["keys"] += len(got)
    for k in sprite_bindings[0]["keys"]:
        if k["texture"] and os.path.basename(k["texture"]) != "hero.png":
            problem(f"{anim}: a key uses {k['texture']}")
    if not near(c["length"], stop):
        problem(f"{anim}: length {c['length']} s in Unity, the animation lasts {stop} s")
    if c["loop"] != loop:
        problem(f"{anim}: loop {c['loop']} in Unity, {loop} in the .anim")

# Painted tiles: Unity's choice per cell against AceStudio's placement.
checked["cells"] = 0
level_map = json.load(open(os.path.join(fixture, "map.json"), encoding="utf-8"))
paint = report.get("paint") or {}
for scheme, cols in (("blob47", 8), ("cardinal16", 4)):
    for suffix, want_type in (("", "AceStudio.AceStudioAutoTile"), ("_ruletile", "UnityEngine.RuleTile")):
        got = paint.get(scheme + suffix)
        if not got or "error" in got:
            problem(f"{scheme}{suffix}: no paint result ({(got or {}).get('error', 'missing')})")
            continue
        if got.get("ruleType") != want_type:
            problem(f"{scheme}{suffix}: the tile asset loaded as {got.get('ruleType')}, not {want_type}")
        for li, (unity_cells, want_cells) in enumerate(zip(got["levels"], level_map["expected"][scheme])):
            for i, (u, (src, c, r)) in enumerate(zip(unity_cells, want_cells)):
                want = None if src < 0 else f"{scheme}_base" if src == 0 else f"{scheme}_level{src}_{r * cols + c}"
                checked["cells"] += 1
                if u != want:
                    problem(f"{scheme}{suffix} level {li + 1} cell ({i % level_map['w']}, {i // level_map['w']}): "
                            f"Unity drew {u}, AceStudio places {want}")
wang = paint.get("wang16")
if not wang or "error" in wang:
    problem(f"wang16: no paint result ({(wang or {}).get('error', 'missing')})")
else:
    if wang.get("setType") != "AceStudio.AceStudioWangSet":
        problem(f"wang16: the Wang set loaded as {wang.get('setType')}")
    cols = level_map["w"] + 1
    for li, (got, want_cells) in enumerate(zip(wang["levels"], level_map["expectedWang16"])):
        if got["offset"] != [-0.5, -0.5]:
            problem(f"wang16 level {li + 1}: the corner tiles sit at {got['offset']}, not half a cell down-left")
        for how in ("painted", "rebuilt"):
            for i, (u, k) in enumerate(zip(got[how], want_cells)):
                want = None if k < 0 else f"wang16_level{li + 1}_{k}"
                checked["cells"] += 1
                if u != want:
                    problem(f"wang16 level {li + 1} ({how}) corner ({i % cols}, {i // cols}): Unity drew {u}, AceStudio places {want}")

print(f"Unity {report.get('unity')}: {checked['textures']} textures, {checked['sprites']} sprites, "
      f"{checked['clips']} clips, {checked['keys']} keys, {checked['cells']} painted cells checked")
if problems:
    print(f"{len(problems)} problem(s):")
    for p in problems[:60]:
        print("  " + p)
    sys.exit(1)
print("all match")
