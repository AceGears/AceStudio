"""Frame-by-frame animation QA for sprite sheets and GIFs (stdlib only).

Usage examples:
  python anim_qa.py fish-swim8.png --json fish-swim8.json
  python anim_qa.py dragon_fly.png --grid 8x1
  python anim_qa.py walk.png --frame 64x64
  python anim_qa.py campfire.gif
  python anim_qa.py sheet.png --grid 4x2 --report out.json

Metrics are evidence for a human/agent reviewer, not a replacement for looking at frames.
Thresholds live in THRESHOLDS and should match docs/agents/ANIMATION_STANDARDS.md.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import sys
from statistics import mean, pstdev

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from imgio import Image, read_any  # noqa: E402

THRESHOLDS = {
    "area_cv_max": 0.08,            # silhouette area variation (rigid bodies); effects exempt
    "centroid_jitter_px_max": 1.0,  # std of frame-to-frame centroid deltas, in-place cycles
    "loop_seam_ratio_max": 1.5,     # diff(last->first) / median consecutive diff
    "semi_alpha_frac_max": 0.0,     # pixel art: alpha must be binary
    "static_region_frac_max": 0.35, # fraction of silhouette cells that never change
    "min_change_frac": 0.002,       # below this a frame is a duplicate / frozen
    "pingpong_pair_frac_min": 1.0,  # share of (i, n-i) pairs that must match to call a cycle ping-pong
    "top_region_loss_max": 0.40,    # top-most part may lose at most this share of its frame-0 area
    "top_band_frac": 0.20,          # the top-most region is sought in this top share of the frame-0 bbox
}


def slice_frames(img: Image, args) -> list:
    if args.json:
        with open(args.json, "r", encoding="utf-8") as f:
            meta = json.load(f)
        fr = meta.get("frames", meta)
        items = fr.values() if isinstance(fr, dict) else fr
        rects = []
        for it in items:
            r = it.get("frame", it)
            rects.append((int(r["x"]), int(r["y"]), int(r["w"]), int(r["h"])))
        return [img.crop(*r) for r in rects]
    if args.grid:
        cols, rows = (int(v) for v in args.grid.lower().split("x"))
        fw, fh = img.w // cols, img.h // rows
    elif args.frame:
        fw, fh = (int(v) for v in args.frame.lower().split("x"))
        cols, rows = img.w // fw, img.h // fh
    else:
        return [img]
    return [img.crop(c * fw, r * fh, fw, fh) for r in range(rows) for c in range(cols)]


def frame_metrics(f: Image) -> dict:
    w, h, px = f.w, f.h, f.px
    area = semi = 0
    sx = sy = 0
    minx, miny, maxx, maxy = w, h, -1, -1
    palette = set()
    for i, (r, g, b, a) in enumerate(px):
        if a == 0:
            continue
        x, y = i % w, i // w
        area += 1
        sx += x
        sy += y
        if a < 255:
            semi += 1
        palette.add((r, g, b))
        minx, miny = min(minx, x), min(miny, y)
        maxx, maxy = max(maxx, x), max(maxy, y)
    # connected components (4-connectivity) over opaque pixels
    seen = bytearray(w * h)
    comps = []
    for i in range(w * h):
        if px[i][3] == 0 or seen[i]:
            continue
        stack = [i]
        seen[i] = 1
        n = 0
        while stack:
            j = stack.pop()
            n += 1
            x, y = j % w, j // w
            for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
                if 0 <= nx < w and 0 <= ny < h:
                    k = ny * w + nx
                    if not seen[k] and px[k][3] != 0:
                        seen[k] = 1
                        stack.append(k)
        comps.append(n)
    # orphan pixels: opaque pixel with no 4-neighbour of the same colour
    orphans = 0
    for i, p in enumerate(px):
        if p[3] == 0:
            continue
        x, y = i % w, i // w
        same = False
        for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if 0 <= nx < w and 0 <= ny < h and px[ny * w + nx] == p:
                same = True
                break
        if not same:
            orphans += 1
    big = [c for c in comps if c >= 4]
    return {
        "area": area,
        "semi_alpha": semi,
        "centroid": (sx / area, sy / area) if area else None,
        "bbox": (minx, miny, maxx, maxy) if area else None,
        "components": len(big),
        "specks": len(comps) - len(big),
        "palette_size": len(palette),
        "palette": palette,
        "orphans": orphans,
    }


def diff_frac(a: Image, b: Image) -> float:
    if (a.w, a.h) != (b.w, b.h):
        return 1.0
    changed = sum(1 for p, q in zip(a.px, b.px) if p != q and (p[3] or q[3]))
    union = sum(1 for p, q in zip(a.px, b.px) if p[3] or q[3]) or 1
    return changed / union


def is_hmirror(a: Image, b: Image) -> bool:
    if (a.w, a.h) != (b.w, b.h):
        return False
    w = a.w
    for y in range(a.h):
        ra = a.px[y * w:(y + 1) * w]
        rb = b.px[y * w:(y + 1) * w][::-1]
        if ra != rb:
            return False
    return True


def static_regions(frames: list, cells: int = 8) -> dict:
    """Grid cells inside the silhouette whose pixels never change across all frames."""
    f0 = frames[0]
    w, h = f0.w, f0.h
    cw, ch = max(1, w // cells), max(1, h // cells)
    occupied = changing = 0
    static_cells = []
    for cy in range(0, h, ch):
        for cx in range(0, w, cw):
            occ = False
            chg = False
            for y in range(cy, min(h, cy + ch)):
                for x in range(cx, min(w, cx + cw)):
                    vals = {fr.px[y * w + x] for fr in frames}
                    if any(v[3] for v in vals):
                        occ = True
                        if len(vals) > 1:
                            chg = True
                            break
                if chg:
                    break
            if occ:
                occupied += 1
                if chg:
                    changing += 1
                else:
                    static_cells.append((cx, cy, cw, ch))
    frac = (occupied - changing) / occupied if occupied else 0.0
    return {"occupied_cells": occupied, "static_cells": len(static_cells),
            "static_frac": round(frac, 3), "static_cell_rects": static_cells[:32]}


def pingpong(frames: list) -> dict:
    """Ping-pong detector: in a palindromic cycle frame i equals frame n-i (i = 1 .. n-1).

    A ping-pong flap or swim plays the same poses forwards and backwards, so the motion
    reverses instead of following through (no arc, no overlap). Pairs are compared with
    diff_frac; a pair below min_change_frac counts as equal.
    """
    n = len(frames)
    pairs = [(i, n - i) for i in range(1, (n + 1) // 2) if i != n - i]
    matched = [(i, j) for i, j in pairs if diff_frac(frames[i], frames[j]) < THRESHOLDS["min_change_frac"]]
    unique = []
    for f in frames:
        if not any(diff_frac(f, u) < THRESHOLDS["min_change_frac"] for u in unique):
            unique.append(f)
    frac = len(matched) / len(pairs) if pairs else 0.0
    return {"pairs_checked": len(pairs), "pairs_matched": matched, "matched_frac": round(frac, 3),
            "unique_poses": len(unique),
            "is_pingpong": bool(n >= 4 and pairs and frac >= THRESHOLDS["pingpong_pair_frac_min"])}


def _components(mask: list, w: int, h: int) -> list:
    """4-connected components of a boolean mask, as lists of flat indices."""
    seen = bytearray(w * h)
    comps = []
    for i in range(w * h):
        if not mask[i] or seen[i]:
            continue
        stack, comp = [i], []
        seen[i] = 1
        while stack:
            j = stack.pop()
            comp.append(j)
            x, y = j % w, j // w
            for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
                if 0 <= nx < w and 0 <= ny < h:
                    k = ny * w + nx
                    if mask[k] and not seen[k]:
                        seen[k] = 1
                        stack.append(k)
        comps.append(comp)
    return comps


def top_region_presence(frames: list) -> dict:
    """Head/part-presence guard.

    The top-most opaque region of frame 0 (the 4-connected piece of the silhouette that
    holds the highest pixel, restricted to the top `top_band_frac` of the frame-0 bbox:
    usually the head, crest, horns or a wing tip) is used as a template. In every frame the
    template is searched within +-max(4, h/6) px; presence is the best share of its pixels
    found with the same colour. A part that is squashed away, hidden under another part,
    or cut off scores low, even when the whole silhouette area barely changes.
    """
    f0 = frames[0]
    w, h = f0.w, f0.h
    if not any(p[3] for p in f0.px):
        return {"available": False}
    x0, y0, x1, y1 = frame_metrics(f0)["bbox"]
    band_h = max(2, round((y1 - y0 + 1) * THRESHOLDS["top_band_frac"]))
    mask = [p[3] != 0 and y0 <= i // w < y0 + band_h for i, p in enumerate(f0.px)]
    comps = [c for c in _components(mask, w, h) if len(c) >= 4] or _components(mask, w, h)
    # The piece holding the highest pixel; ties go to the larger piece.
    comps.sort(key=lambda c: (min(j // w for j in c), -len(c)))
    region = comps[0]
    tpl = [(j % w, j // w, f0.px[j]) for j in region]
    rx0 = min(x for x, _, _ in tpl)
    ry0 = min(y for _, y, _ in tpl)
    rx1 = max(x for x, _, _ in tpl)
    ry1 = max(y for _, y, _ in tpl)
    reach = max(4, h // 6)
    presence = []
    for f in frames:
        if (f.w, f.h) != (w, h):
            presence.append(0.0)
            continue
        best = 0
        for dy in range(-reach, reach + 1):
            for dx in range(-reach, reach + 1):
                hit = 0
                for x, y, c in tpl:
                    tx, ty = x + dx, y + dy
                    if 0 <= tx < w and 0 <= ty < h and f.px[ty * w + tx] == c:
                        hit += 1
                best = max(best, hit)
                if best == len(tpl):
                    break
            if best == len(tpl):
                break
        presence.append(round(best / len(tpl), 3))
    worst = min(range(len(presence)), key=lambda i: presence[i])
    lost = [i for i, p in enumerate(presence) if 1 - p > THRESHOLDS["top_region_loss_max"]]
    return {"available": True, "bbox": [rx0, ry0, rx1, ry1], "area0": len(tpl), "presence": presence,
            "min_presence": presence[worst], "worst_frame": worst, "frames_lost": lost}


def analyze(frames: list, kind: str) -> dict:
    m = [frame_metrics(f) for f in frames]
    n = len(frames)
    rep = {"frames": n, "size": [frames[0].w, frames[0].h], "kind": kind, "per_frame": [],
           "warnings": []}
    for i, fm in enumerate(m):
        rep["per_frame"].append({k: v for k, v in fm.items() if k != "palette"})
    if n < 2:
        rep["warnings"].append("single frame: only static pixel-art checks apply")
    areas = [fm["area"] for fm in m]
    if n >= 2 and mean(areas):
        cv = pstdev(areas) / mean(areas)
        rep["area_cv"] = round(cv, 4)
        if kind == "body" and cv > THRESHOLDS["area_cv_max"]:
            rep["warnings"].append(f"silhouette area varies {cv:.1%} (> {THRESHOLDS['area_cv_max']:.0%}): "
                                   "possible proportion drift / missing or growing parts")
    cents = [fm["centroid"] for fm in m if fm["centroid"]]
    if len(cents) >= 3:
        d = [math.dist(cents[i], cents[i + 1]) for i in range(len(cents) - 1)]
        rep["centroid_jitter_px"] = round(pstdev(d), 3)
    comps = [fm["components"] for fm in m]
    rep["component_counts"] = comps
    if len(set(comps)) > 1:
        rep["warnings"].append(f"connected-part count changes across frames {comps}: "
                               "possible detached or missing body parts")
    semi = sum(fm["semi_alpha"] for fm in m)
    if semi:
        rep["warnings"].append(f"{semi} semi-transparent pixels: pixel art should use binary alpha")
    pal0 = m[0]["palette"]
    union = set().union(*(fm["palette"] for fm in m))
    rep["palette_union"] = len(union)
    rep["palette_frame0"] = len(pal0)
    new_cols = [i for i, fm in enumerate(m) if fm["palette"] - pal0]
    if kind == "body" and new_cols:
        rep["warnings"].append(f"frames {new_cols[:12]} introduce colours absent from frame 0 "
                               f"(union {len(union)} vs {len(pal0)}): palette drift")
    if n >= 2:
        diffs = [diff_frac(frames[i], frames[i + 1]) for i in range(n - 1)]
        seam = diff_frac(frames[-1], frames[0])
        med = sorted(diffs)[len(diffs) // 2] or 1e-9
        rep["consecutive_diff"] = [round(x, 3) for x in diffs]
        rep["loop_seam_diff"] = round(seam, 3)
        rep["loop_seam_ratio"] = round(seam / med, 2)
        if seam / med > THRESHOLDS["loop_seam_ratio_max"]:
            rep["warnings"].append(f"loop seam jump {seam:.3f} is {seam / med:.1f}x the typical "
                                   "frame change: visible pop when looping")
        dup = [i for i, x in enumerate(diffs) if x < THRESHOLDS["min_change_frac"]]
        if dup:
            rep["warnings"].append(f"frames {dup} -> next are (near) identical: held/duplicate frames")
        if kind == "effect" and len(diffs) >= 4:
            # a pure periodic sine-like change suggests a warped static image
            cv_d = pstdev(diffs) / (mean(diffs) or 1e-9)
            rep["diff_irregularity"] = round(cv_d, 3)
        mir = [i for i in range(n - 1) if is_hmirror(frames[i], frames[i + 1])]
        if mir:
            rep["warnings"].append(f"frame pairs {mir} are horizontal mirrors: abrupt sprite flip")
        sr = static_regions(frames)
        rep["static_regions"] = sr
        if kind == "body" and sr["static_frac"] > THRESHOLDS["static_region_frac_max"]:
            rep["warnings"].append(f"{sr['static_frac']:.0%} of silhouette cells never change: "
                                   "large parts are frozen (e.g. a stationary wing or fin)")
        # rigid-transform heuristic: palette and pixel counts identical every frame
        if kind == "body" and len(set(areas)) == 1 and len({frozenset(fm['palette']) for fm in m}) == 1 \
                and max(diffs) > 0.05:
            rep["warnings"].append("identical area and palette every frame with large motion: "
                                   "likely whole-sprite translation/warp rather than articulation")
        pp = pingpong(frames)
        rep["pingpong"] = pp
        if pp["is_pingpong"]:
            rep["warnings"].append(f"ping-pong cycle: frame i equals frame n-i for every pair {pp['pairs_matched']} "
                                   f"({pp['unique_poses']} unique poses in {n} frames): the motion reverses "
                                   "instead of following through")
        if kind == "body":
            tr = top_region_presence(frames)
            rep["top_region"] = tr
            if tr.get("available") and tr["frames_lost"]:
                rep["warnings"].append(
                    f"top-most part (head/crest/wing tip, bbox {tr['bbox']}, {tr['area0']} px in frame 0) "
                    f"loses >{THRESHOLDS['top_region_loss_max']:.0%} of its pixels in frames {tr['frames_lost'][:12]} "
                    f"(worst F{tr['worst_frame']}: {tr['min_presence']:.0%} present): part squashed, hidden or lost")
    return rep


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("path")
    ap.add_argument("--grid", help="COLSxROWS")
    ap.add_argument("--frame", help="WxH frame size")
    ap.add_argument("--json", help="Aseprite/Phaser-style frame JSON")
    ap.add_argument("--kind", choices=["body", "effect"], default="body",
                    help="body = character/creature; effect = fire/smoke/water (relaxed area/palette)")
    ap.add_argument("--report", help="write full JSON report here")
    args = ap.parse_args()
    frames = read_any(args.path)
    if len(frames) == 1:
        frames = slice_frames(frames[0], args)
    rep = analyze(frames, args.kind)
    rep["source"] = os.path.basename(args.path)
    if args.report:
        with open(args.report, "w", encoding="utf-8") as f:
            json.dump(rep, f, indent=2, default=list)
    summary = {}
    for k, v in rep.items():
        if k in ("per_frame", "static_regions", "pingpong", "top_region"):
            continue
        if isinstance(v, list) and len(v) > 16 and k != "warnings":
            v = f"{v[:16]} ... ({len(v)} values; full list in --report)"
        elif isinstance(v, list) and k != "warnings":
            v = str(v)
        summary[k] = v
    summary["static_frac"] = rep.get("static_regions", {}).get("static_frac")
    if "pingpong" in rep:
        pp = rep["pingpong"]
        summary["pingpong"] = f"{pp['is_pingpong']} ({len(pp['pairs_matched'])}/{pp['pairs_checked']} pairs i=n-i, " \
                              f"{pp['unique_poses']} unique poses)"
    if rep.get("top_region", {}).get("available"):
        tr = rep["top_region"]
        summary["top_region_presence"] = str(tr["presence"])
    print(json.dumps(summary, indent=2, default=list))
    return 1 if rep["warnings"] else 0


if __name__ == "__main__":
    sys.exit(main())
