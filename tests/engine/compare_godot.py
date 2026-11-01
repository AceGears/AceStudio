"""Compares what Godot reports (check.gd) with what AceStudio exported.

    python3 compare_godot.py <fixture folder> <godot result.json>

Exit 0 when everything matches; prints every difference otherwise.
"""
import json
import sys


def main():
    fx, res = sys.argv[1], sys.argv[2]
    m = json.load(open(fx + "/map.json"))
    r = json.load(open(res))
    errors = []
    print("Godot", r["godot"])

    # SpriteFrames: tags walk (4 x 100 ms, loop), idle (ping-pong 4,5 = 2 frames), die (no loop).
    sf = r["sprite_frames"]
    print("SpriteFrames:", {k: (len(v["frames"]), v["speed"], v["loop"]) for k, v in sf.items()})
    if set(sf) != {"walk", "idle", "die"}:
        errors.append("animations %s" % sorted(sf))
    if sf.get("walk", {}).get("loop") is not True or sf.get("die", {}).get("loop") is not False:
        errors.append("loop flags")
    sheet = json.load(open(fx + "/hero.json"))
    frames = list(sheet["frames"].values())
    for name, v in sf.items():
        for f in v["frames"]:
            if f["atlas_size"] != [sheet["meta"]["size"]["w"], sheet["meta"]["size"]["h"]]:
                errors.append("%s: atlas size %s" % (name, f["atlas_size"]))
    walk = sf.get("walk", {"frames": []})["frames"]
    for i, f in enumerate(walk):
        fr = frames[i]["frame"]
        if f["region"] != [fr["x"], fr["y"], fr["w"], fr["h"]]:
            errors.append("walk frame %d region %s != %s" % (i, f["region"], fr))
    # idle: 300 ms then 150 ms -> speed 1000/150 gcd 150 -> durations 2, 1
    idle = sf.get("idle", {"frames": [], "speed": 0})
    ms = [round(f["duration"] * 1000 / idle["speed"]) for f in idle["frames"]] if idle["speed"] else []
    if ms != [300, 150]:
        errors.append("idle durations %s ms" % ms)

    for key in ("stylebox_panel", "stylebox_button"):
        print(key, r[key])
    ui = json.load(open(fx + "/ui.json"))
    for key, name in (("stylebox_panel", "panel"), ("stylebox_button", "button_normal")):
        fr = ui["frames"][name]
        ns = fr["nineSlice"]
        if r[key]["margins"] != [ns["left"], ns["top"], ns["right"], ns["bottom"]]:
            errors.append("%s margins %s" % (key, r[key]["margins"]))
        if r[key]["region"] != [fr["frame"]["x"], fr["frame"]["y"], fr["frame"]["w"], fr["frame"]["h"]]:
            errors.append("%s region %s" % (key, r[key]["region"]))

    for scheme in ("blob47", "wang16", "cardinal16"):
        t = r[scheme]
        print(scheme, "terrain sets", t["terrain_sets"], "modes", t["modes"], "terrains", t["terrains"], "sources", t["sources"], "tiles", t["tiles"])
        want_mode = {"blob47": 0, "wang16": 1, "cardinal16": 2}[scheme]
        if t["modes"] != [want_mode, want_mode]:
            errors.append("%s modes %s" % (scheme, t["modes"]))
        if t["terrains"] != [["water", "sand"], ["grass"]]:
            errors.append("%s terrains %s" % (scheme, t["terrains"]))
        w, h = m["w"], m["h"]
        for level in (1, 2):
            placed = t["placed"][level - 1]
            missing = sum(1 for y in range(h) for x in range(w) if m["levels"][y * w + x] >= level and placed[y * w + x][0] < 0)
            if missing:
                cells = [(x, y) for y in range(h) for x in range(w) if m["levels"][y * w + x] >= level and placed[y * w + x][0] < 0]

                def isolated(c):
                    x, y = c
                    return all(not (0 <= x + dx < w and 0 <= y + dy < h and m["levels"][(y + dy) * w + x + dx] >= level)
                               for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)))
                # Godot's corner mode works on the cell grid: a single cell of a
                # level painted over empty cells has no 16-tile corner tile
                # (AceStudio draws it on the dual grid). Known and documented.
                if scheme == "wang16" and level > 1 and all(isolated(c) for c in cells):
                    print("  level %d: %d isolated single cells left empty by Godot's corner mode (expected): %s" % (level, len(cells), cells))
                else:
                    errors.append("%s level %d: %d painted cells without a tile: %s" % (scheme, level, missing, cells))
            if scheme in m["expected"]:
                exp = m["expected"][scheme][level - 1]
                diff = [(i % w, i // w, exp[i], placed[i]) for i in range(w * h) if exp[i] != placed[i]]
                print("  level %d: %d of %d cells match AceStudio's placement" % (level, w * h - len(diff), w * h))
                for d in diff[:8]:
                    errors.append("%s level %d cell %s: AceStudio %s, Godot %s" % (scheme, level, d[:2], d[2], d[3]))
    if errors:
        print("DIFFERENCES:")
        for e in errors:
            print("  " + e)
        return 1
    print("All checks match.")
    return 0


sys.exit(main())
