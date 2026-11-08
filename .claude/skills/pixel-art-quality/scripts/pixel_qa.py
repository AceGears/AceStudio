"""Static pixel-art checks for a single PNG (stdlib only). See ../SKILL.md."""
from __future__ import annotations

import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "animation-quality", "scripts"))
from imgio import read_png  # noqa: E402


def effective_scale(img) -> int:
    """Largest k in (8..2) such that the opaque image is made of k×k uniform blocks."""
    for k in (8, 6, 5, 4, 3, 2):
        if img.w % k or img.h % k:
            continue
        ok = True
        for by in range(0, img.h, k):
            for bx in range(0, img.w, k):
                p = img.get(bx, by)
                for y in range(by, by + k):
                    row = img.px[y * img.w + bx: y * img.w + bx + k]
                    if any(q != p for q in row):
                        ok = False
                        break
                if not ok:
                    break
            if not ok:
                break
        if ok:
            return k
    return 1


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--max-colors", type=int, default=32)
    ap.add_argument("--report")
    a = ap.parse_args()
    img = read_png(a.path)
    w, h, px = img.w, img.h, img.px
    opaque = [p for p in px if p[3]]
    semi = sum(1 for p in px if 0 < p[3] < 255)
    pal = sorted({p[:3] for p in opaque})
    near = 0
    for i in range(len(pal)):
        for j in range(i + 1, len(pal)):
            if max(abs(pal[i][c] - pal[j][c]) for c in range(3)) < 12:
                near += 1
    orphans = 0
    for i, p in enumerate(px):
        if not p[3]:
            continue
        x, y = i % w, i // w
        if not any(0 <= nx < w and 0 <= ny < h and px[ny * w + nx] == p
                   for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1))):
            orphans += 1
    scale = effective_scale(img)
    rep = {"file": os.path.basename(a.path), "size": [w, h], "opaque_px": len(opaque),
           "palette": len(pal), "semi_alpha": semi, "near_duplicate_colours": near,
           "orphans": orphans, "orphan_frac": round(orphans / max(1, len(opaque)), 4),
           "effective_scale": scale, "fails": []}
    if semi:
        rep["fails"].append(f"{semi} semi-transparent pixels")
    if len(pal) > a.max_colors:
        rep["fails"].append(f"palette {len(pal)} > limit {a.max_colors}")
    if scale > 1:
        rep["fails"].append(f"image is a {scale}x upscale of a {w // scale}x{h // scale} sprite")
    if rep["orphan_frac"] > 0.05:
        rep["fails"].append(f"orphan pixels {rep['orphan_frac']:.1%} of opaque area (noisy clusters)")
    if a.report:
        with open(a.report, "w", encoding="utf-8") as f:
            json.dump(rep, f, indent=2)
    print(json.dumps(rep, indent=2))
    return 1 if rep["fails"] else 0


if __name__ == "__main__":
    sys.exit(main())
